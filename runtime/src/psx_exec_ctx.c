/* psx_exec_ctx.c - per-thread draw contexts (psx_exec_ctx.h,
 * docs/THREADED_CONTEXTS.md).
 *
 * Owns snapshots, contexts, the dispatch trampoline for the lean draw clones,
 * their slow memory path (BIOS ROM, GPU / DMA / interrupt-controller
 * registers) and the GPU command capture. Nothing here writes main-context
 * state: snapshot capture only reads it through side-effect-free accessors,
 * and a context only ever touches its own memory. */

#include "psx_exec_ctx.h"
#include "psx_dctx_mem.h"
#include "psx_dctx_bios.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "dma.h"
#include "gpu.h"
#include "gte_ctx.h"
#include "pgxp_ctx.h"
#include "psx_memory.h"
#include "timers.h"

/* memory.c / gpu.c / interrupts (main context, read at snapshot only) */
extern uint8_t *memory_get_ram_ptr(void);
extern uint8_t *memory_get_scratchpad_ptr(void);
extern const uint8_t *memory_get_bios_rom_ptr(void);
extern int psx_text_ref_ranges_match(const uint8_t *ctx_ram, uint32_t ram_mask,
                                     const uint32_t *lo_len_pairs, uint32_t count);
extern uint32_t i_stat;
extern uint32_t i_mask;
extern int psx_ws_x_margin(void);

#define BIOS_ROM_BYTES (512u * 1024u)
#define CTX_DEFAULT_STEP_LIMIT 50000000ull
#define CTX_DEFAULT_BLOCK_LIMIT 50000000ull
#define CTX_DMA_NODE_LIMIT     (1u << 22)

/* ---- draw table ----------------------------------------------------------- */

static const PsxDctxTable *s_table;

void psx_dctx_register_table(const PsxDctxTable *table) { s_table = table; }

int psx_ctx_available(void) { return s_table && s_table->row_count != 0; }

static const PsxDctxRow *dctx_find(uint32_t pc, size_t *index) {
    if (!s_table) return NULL;
    uint32_t lo = 0, hi = s_table->row_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        uint32_t key = s_table->rows[mid].key;
        if (key < pc) lo = mid + 1u;
        else if (key > pc) hi = mid;
        else {
            if (index) *index = mid;
            return &s_table->rows[mid];
        }
    }
    return NULL;
}

int psx_ctx_draw_function(uint32_t addr) {
    const PsxDctxRow *r = dctx_find(addr, NULL);
    return r && r->resume == 0u;
}

/* ---- entry hooks ---------------------------------------------------------- */

#define CTX_MAX_ENTRY_HOOKS 64
typedef struct { uint32_t key; PsxCtxEntryHook hook; void *user; } CtxHook;
static CtxHook s_hooks[CTX_MAX_ENTRY_HOOKS];
static int s_hook_count;

int psx_ctx_register_entry_hook(uint32_t address, PsxCtxEntryHook hook, void *user) {
    if (!hook || s_hook_count >= CTX_MAX_ENTRY_HOOKS) return 0;
    s_hooks[s_hook_count].key = address & 0x1FFFFFFFu;
    s_hooks[s_hook_count].hook = hook;
    s_hooks[s_hook_count].user = user;
    s_hook_count++;
    return 1;
}

void psx_ctx_clear_entry_hooks(void) { s_hook_count = 0; }

int psx_dctx_entry_hook(CPUState *cpu, uint32_t addr) {
    PsxExecCtx *x = cpu->ctx;
    const uint32_t key = addr & 0x1FFFFFFFu;
    for (int i = 0; i < s_hook_count; i++) {
        if (s_hooks[i].key != key) continue;
        x->stats.hooks_run++;
        if (s_hooks[i].hook(cpu, addr, s_hooks[i].user)) {
            cpu->pc = cpu->gpr[31];
            return 1;
        }
    }
    return 0;
}

void psx_dctx_note_stripped_hook(CPUState *cpu) { cpu->ctx->stats.hooks_stripped++; }

/* ---- faults --------------------------------------------------------------- */

void psx_dctx_fault(CPUState *cpu, int status, uint32_t pc, const char *what) {
    PsxExecCtx *x = cpu->ctx;
    x->status = (PsxCtxStatus)status;
    x->fault_pc = pc;
    x->fault_ra = cpu->gpr[31];
    x->fault_what = what;
    longjmp(x->abort, 1);
}

const char *psx_ctx_status_name(PsxCtxStatus s) {
    switch (s) {
    case PSX_CTX_OK: return "ok";
    case PSX_CTX_FAULT_MISS: return "miss";
    case PSX_CTX_FAULT_TEXT: return "text";
    case PSX_CTX_FAULT_DEVICE_READ: return "device_read";
    case PSX_CTX_FAULT_DMA: return "dma";
    case PSX_CTX_FAULT_UNSUPPORTED: return "unsupported";
    case PSX_CTX_FAULT_EXCEPTION: return "exception";
    case PSX_CTX_FAULT_RUNAWAY: return "runaway";
    case PSX_CTX_FAULT_NO_TABLE: return "no_table";
    case PSX_CTX_FAULT_BAD_ARG: return "bad_arg";
    }
    return "?";
}

/* ---- slots: CPUState memory pointers without a cpu argument ---------------- */

static PsxExecCtx *_Atomic s_slots[PSX_CTX_MAX];

#define CTX_SLOT_THUNKS(n)                                                        \
    static uint32_t ctx_rw_##n(uint32_t a) { return psx_dctx_lw(&s_slots[n]->cpu, a); } \
    static uint16_t ctx_rh_##n(uint32_t a) { return psx_dctx_lh(&s_slots[n]->cpu, a); } \
    static uint8_t  ctx_rb_##n(uint32_t a) { return psx_dctx_lb(&s_slots[n]->cpu, a); } \
    static void ctx_ww_##n(uint32_t a, uint32_t v) { psx_dctx_sw(&s_slots[n]->cpu, a, v); } \
    static void ctx_wh_##n(uint32_t a, uint16_t v) { psx_dctx_sh(&s_slots[n]->cpu, a, v); } \
    static void ctx_wb_##n(uint32_t a, uint8_t v)  { psx_dctx_sb(&s_slots[n]->cpu, a, v); }
CTX_SLOT_THUNKS(0)  CTX_SLOT_THUNKS(1)  CTX_SLOT_THUNKS(2)  CTX_SLOT_THUNKS(3)
CTX_SLOT_THUNKS(4)  CTX_SLOT_THUNKS(5)  CTX_SLOT_THUNKS(6)  CTX_SLOT_THUNKS(7)
CTX_SLOT_THUNKS(8)  CTX_SLOT_THUNKS(9)  CTX_SLOT_THUNKS(10) CTX_SLOT_THUNKS(11)
CTX_SLOT_THUNKS(12) CTX_SLOT_THUNKS(13) CTX_SLOT_THUNKS(14) CTX_SLOT_THUNKS(15)

typedef struct {
    uint32_t (*rw)(uint32_t); uint16_t (*rh)(uint32_t); uint8_t (*rb)(uint32_t);
    void (*ww)(uint32_t, uint32_t); void (*wh)(uint32_t, uint16_t); void (*wb)(uint32_t, uint8_t);
} CtxThunks;
#define CTX_T(n) { ctx_rw_##n, ctx_rh_##n, ctx_rb_##n, ctx_ww_##n, ctx_wh_##n, ctx_wb_##n }
static const CtxThunks s_thunks[PSX_CTX_MAX] = {
    CTX_T(0), CTX_T(1), CTX_T(2), CTX_T(3), CTX_T(4), CTX_T(5), CTX_T(6), CTX_T(7),
    CTX_T(8), CTX_T(9), CTX_T(10), CTX_T(11), CTX_T(12), CTX_T(13), CTX_T(14), CTX_T(15),
};

static void wire_cpu(PsxExecCtx *x) {
    const CtxThunks *t = &s_thunks[x->slot];
    x->cpu.read_word = t->rw;
    x->cpu.read_half = t->rh;
    x->cpu.read_byte = t->rb;
    x->cpu.write_word = t->ww;
    x->cpu.write_half = t->wh;
    x->cpu.write_byte = t->wb;
    x->cpu.ctx = x;
}

/* ---- snapshots ------------------------------------------------------------ */

struct PsxCtxSnapshot {
    CPUState cpu;
    uint8_t *ram;
    uint32_t ram_size, ram_mask;
    uint8_t  scratch[1024];
    PgxpCtxExport *pgxp;
    PsxGteCtx *gte;
    int32_t  ws_x_margin;
    uint32_t gpustat;
    uint32_t dma_madr[7], dma_bcr[7], dma_chcr[7];
    uint32_t dpcr, dicr, i_stat, i_mask;
    uint32_t timer_counter[3], timer_mode[3], timer_target[3];
};

PsxCtxSnapshot *psx_ctx_snapshot_capture(const CPUState *cpu, unsigned flags) {
    if (!cpu) return NULL;
    PsxCtxSnapshot *s = (PsxCtxSnapshot *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->ram_size = psx_ram_live_bytes();
    s->ram_mask = g_psx_ram_mask;
    s->ram = (uint8_t *)malloc(s->ram_size);
    s->gte = gte_ctx_create(NULL);
    if (!s->ram || !s->gte) { psx_ctx_snapshot_release(s); return NULL; }
    memcpy(s->ram, memory_get_ram_ptr(), s->ram_size);
    memcpy(s->scratch, memory_get_scratchpad_ptr(), sizeof s->scratch);
    s->cpu = *cpu;
    s->cpu.ctx = NULL;
    if (flags & PSX_CTX_SNAP_PGXP) {
        s->pgxp = pgxp_ctx_export_main();
        if (!s->pgxp) { psx_ctx_snapshot_release(s); return NULL; }
    }
    gte_ctx_snapshot_main(s->gte);
    s->ws_x_margin = psx_ws_x_margin();
    s->gpustat = gpu_peek_gpustat();
    for (int ch = 0; ch < 7; ch++)
        dma_peek_channel(ch, &s->dma_madr[ch], &s->dma_bcr[ch], &s->dma_chcr[ch]);
    s->dpcr = dma_get_dpcr();
    s->dicr = dma_get_dicr();
    s->i_stat = i_stat;
    s->i_mask = i_mask;
    {
        uint16_t counter[3], target[3];
        uint32_t mode[3], frac[3];
        int32_t irq_line[3];
        timers_get_snapshot(counter, mode, target, irq_line, frac);
        for (int i = 0; i < 3; i++) {
            s->timer_counter[i] = counter[i];
            s->timer_mode[i] = mode[i];
            s->timer_target[i] = target[i];
        }
    }
    return s;
}

void psx_ctx_snapshot_release(PsxCtxSnapshot *s) {
    if (!s) return;
    free(s->ram);
    pgxp_ctx_export_free(s->pgxp);
    gte_ctx_destroy(s->gte);
    free(s);
}

const uint8_t *psx_ctx_snapshot_ram(const PsxCtxSnapshot *s, uint32_t *size) {
    if (size) *size = s ? s->ram_size : 0;
    return s ? s->ram : NULL;
}

/* ---- contexts ------------------------------------------------------------- */

PsxExecCtx *psx_ctx_create(void) {
    PsxExecCtx *x = (PsxExecCtx *)calloc(1, sizeof *x);
    if (!x) return NULL;
    x->slot = -1;
    for (int i = 0; i < PSX_CTX_MAX; i++) {
        PsxExecCtx *expected = NULL;
        if (atomic_compare_exchange_strong(&s_slots[i], &expected, x)) {
            x->slot = i;
            break;
        }
    }
    x->pgxp = pgxp_ctx_create();
    x->gte = gte_ctx_create(x->pgxp);
    if (x->slot < 0 || !x->pgxp || !x->gte) { psx_ctx_destroy(x); return NULL; }
    x->step_limit = CTX_DEFAULT_STEP_LIMIT;
    x->block_limit = CTX_DEFAULT_BLOCK_LIMIT;
    wire_cpu(x);
    return x;
}

void psx_ctx_destroy(PsxExecCtx *x) {
    if (!x) return;
    if (x->slot >= 0) atomic_store(&s_slots[x->slot], NULL);
    gte_ctx_destroy(x->gte);
    pgxp_ctx_destroy(x->pgxp);
    free(x->ram);
    free(x->gpu);
    free(x->gpu_nodes); free(x->gpu_lists); free(x->gpu_memory);
    free(x->text_ok);
    free(x);
}

int psx_ctx_begin(PsxExecCtx *x, const PsxCtxSnapshot *s) {
    if (!x || !s) return 0;
    if (!x->ram || x->ram_mask != s->ram_mask) {
        free(x->ram);
        x->ram = (uint8_t *)malloc((size_t)s->ram_mask + 1u);
        if (!x->ram) return 0;
    }
    memcpy(x->ram, s->ram, s->ram_size);
    x->ram_mask = s->ram_mask;
    memcpy(x->scratch, s->scratch, sizeof x->scratch);
    x->cpu = s->cpu;
    x->cpu.pc = 0;
    wire_cpu(x);
    x->snap = s;
    x->pgxp_active = pgxp_ctx_begin(x->pgxp, s->pgxp);
    if (x->pgxp_active < 0) { x->pgxp_active = 0; return 0; }
    gte_ctx_begin(x->gte, s->gte);
    x->ws_x_margin = s->ws_x_margin;
    x->entry_hooks = s_hook_count;
    x->gpustat = s->gpustat;
    x->gpu_dma_dir = (s->gpustat >> 29) & 3u;
    memcpy(x->dma_madr, s->dma_madr, sizeof x->dma_madr);
    memcpy(x->dma_bcr, s->dma_bcr, sizeof x->dma_bcr);
    memcpy(x->dma_chcr, s->dma_chcr, sizeof x->dma_chcr);
    x->dpcr = s->dpcr;
    x->dicr = s->dicr & 0x7FFFFFFFu;
    x->i_stat = s->i_stat;
    x->i_mask = s->i_mask;
    memcpy(x->timer_counter, s->timer_counter, sizeof x->timer_counter);
    memcpy(x->timer_mode, s->timer_mode, sizeof x->timer_mode);
    memcpy(x->timer_target, s->timer_target, sizeof x->timer_target);
    memset(x->timer_reads, 0, sizeof x->timer_reads);
    x->gpu_n = 0;
    x->gpu_nodes_n=x->gpu_lists_n=x->gpu_memory_n=0;
    memset(&x->stats, 0, sizeof x->stats);
    x->status = PSX_CTX_OK;
    x->fault_pc = 0;
    x->fault_what = NULL;
    const size_t rows = s_table ? s_table->row_count : 0u;
    if (x->text_ok_n != rows) {
        free(x->text_ok);
        x->text_ok = rows ? (uint8_t *)malloc(rows) : NULL;
        x->text_ok_n = x->text_ok ? rows : 0u;
    }
    if (x->text_ok) memset(x->text_ok, 0, x->text_ok_n);
    return 1;
}

CPUState *psx_ctx_cpu(PsxExecCtx *x) { return x ? &x->cpu : NULL; }

/* ---- code validity: the clone's bytes must be the snapshot's bytes -------- */

static int row_text_ok(PsxExecCtx *x, const PsxDctxRow *row, size_t index) {
    if (index < x->text_ok_n && x->text_ok[index]) return x->text_ok[index] == 1;
    int ok = row->range_count != 0 &&
             psx_text_ref_ranges_match(x->ram, x->ram_mask,
                                       &s_table->ranges[2u * row->range_index],
                                       row->range_count);
    if (index < x->text_ok_n) x->text_ok[index] = ok ? 1 : 2;
    return ok;
}

int psx_dctx_text_ok(CPUState *cpu, uint32_t addr) {
    size_t index = 0;
    const PsxDctxRow *row = dctx_find(addr, &index);
    return row && row_text_ok(cpu->ctx, row, index);
}

/* ---- the trampoline ------------------------------------------------------- */

/* The dispatch loop: separate from the setjmp frame so nothing it keeps in
 * registers is live across a longjmp. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static void ctx_run(PsxExecCtx *x, uint32_t pc, uint32_t stop, uint32_t sp) {
    CPUState *cpu = &x->cpu;
    uint64_t steps = 0;
    for (;;) {
        if (pc == stop && cpu->gpr[29] == sp) return;
        if (pc == 0u) {
            if (cpu->gpr[29] == sp && cpu->gpr[31] == stop) return;
            psx_dctx_fault(cpu, PSX_CTX_FAULT_MISS, 0u, "transfer to PC 0");
        }
        size_t index = 0;
        if ((pc & 0x1FFFFFFFu) == 0xA0u && psx_dctx_bios_call(cpu, pc)) {
            pc = cpu->pc;
            continue;
        }
        const PsxDctxRow *row = dctx_find(pc, &index);
        if (!row) psx_dctx_fault(cpu, PSX_CTX_FAULT_MISS, pc, "no draw clone");
        if (!row_text_ok(x, row, index))
            psx_dctx_fault(cpu, PSX_CTX_FAULT_TEXT, pc, "code differs from the compiled image");
        if (++steps > x->step_limit)
            psx_dctx_fault(cpu, PSX_CTX_FAULT_RUNAWAY, pc, "dispatch budget");
        x->stats.dispatches++;
        x->trail[x->trail_n++ & 15u] = pc;
        cpu->pc = row->resume;
        row->fn(cpu);
        pc = cpu->pc;
    }
}

PsxCtxStatus psx_ctx_call(PsxExecCtx *x, uint32_t fn, const uint32_t *args,
                          int nargs, uint32_t stop_pc, uint32_t *ret) {
    if (!x || !x->snap || nargs < 0 || nargs > 4 || x->in_call)
        return PSX_CTX_FAULT_BAD_ARG;
    if (!psx_ctx_available()) return x->status = PSX_CTX_FAULT_NO_TABLE;
    CPUState *cpu = &x->cpu;
    for (int i = 0; i < nargs; i++) cpu->gpr[4 + i] = args[i];
    cpu->gpr[31] = stop_pc ? stop_pc : PSX_CTX_STOP_PC;
    x->status = PSX_CTX_OK;
    x->block_budget = x->block_limit;
    x->trail_n = 0;
    x->in_call = 1;
    if (setjmp(x->abort)) {
        x->in_call = 0;
        x->cpu.pc = 0;
        return x->status;
    }
    ctx_run(x, fn, x->cpu.gpr[31], x->cpu.gpr[29]);
    x->in_call = 0;
    x->cpu.pc = 0;
    if (ret) *ret = x->cpu.gpr[2];
    return PSX_CTX_OK;
}

/* ---- syscalls --------------------------------------------------------------
 * Only the two the kernel handles in place: Enter/ExitCriticalSection (a0 =
 * 1 / 2) change SR's interrupt enables, as the BIOS does. */
int psx_dctx_syscall(CPUState *cpu, uint32_t code) {
    (void)code;
    const uint32_t fn = cpu->gpr[4];
    uint32_t *sr = &cpu->cop0[12];
    if (fn == 1u) {
        cpu->gpr[2] = ((*sr & 0x404u) == 0x404u) ? 1u : 0u;
        *sr &= ~0x404u;
        return 0;
    }
    if (fn == 2u) {
        *sr |= 0x404u;
        return 0;
    }
    psx_dctx_fault(cpu, PSX_CTX_FAULT_EXCEPTION, fn, "syscall");
}

/* ---- GPU capture ---------------------------------------------------------- */

static void gpu_push(PsxExecCtx *x, uint32_t word, uint32_t src) {
    if (x->gpu_n == x->gpu_cap) {
        size_t cap = x->gpu_cap ? x->gpu_cap * 2u : 65536u;
        PsxCtxGpuWord *p = (PsxCtxGpuWord *)realloc(x->gpu, cap * sizeof *p);
        if (!p) psx_dctx_fault(&x->cpu, PSX_CTX_FAULT_RUNAWAY, 0u, "capture allocation");
        x->gpu = p;
        x->gpu_cap = cap;
    }
    x->gpu[x->gpu_n].word = word;
    x->gpu[x->gpu_n].src = src;
    pgxp_ctx_capture_vertex(x->pgxp, src >= PSX_CTX_SRC_GP1 ? 0xFFFFFFFFu : src,
                            word, &x->gpu[x->gpu_n].vertex);
    x->gpu_n++;
    x->stats.gpu_words++;
}

static uint32_t ctx_gpustat(const PsxExecCtx *x) {
    uint32_t st = x->gpustat & ~((3u << 29) | (1u << 25) | (1u << 27));
    st |= (1u << 26) | (1u << 28);           /* ready: the capture never blocks */
    st |= (x->gpu_dma_dir & 3u) << 29;
    if (x->gpu_dma_dir != 0u) st |= 1u << 25;
    return st;
}

/* ---- DMA (channels 2 and 6) ------------------------------------------------ */

static inline uint32_t ram32(PsxExecCtx *x, uint32_t addr) {
    uint32_t v;
    memcpy(&v, x->ram + (addr & 0x00FFFFFCu & x->ram_mask), 4);
    return v;
}

static void dma_complete(PsxExecCtx *x, int ch) {
    x->dma_chcr[ch] &= ~((1u << 24) | (1u << 28));
    if ((x->dicr & (1u << 23)) && (x->dicr & (1u << (16 + ch)))) {
        const int was_master = (x->dicr & (1u << 15)) ||
                               ((x->dicr & (1u << 23)) && (x->dicr & 0x7F000000u));
        x->dicr |= 1u << (24 + ch);
        if (!was_master) x->i_stat |= 1u << 3;   /* IRQ3 on the master edge */
    }
}

static void ctx_capture_grow(PsxExecCtx *x,void **data,size_t *capacity,size_t need,size_t unit) {
    if (need<=*capacity) return;
    size_t cap=*capacity ? *capacity : 256;
    while (cap<need) {
        if(cap>SIZE_MAX/2) psx_dctx_fault(&x->cpu,PSX_CTX_FAULT_RUNAWAY,0,"DMA receipt capacity");
        cap*=2;
    }
    if(cap>SIZE_MAX/unit) psx_dctx_fault(&x->cpu,PSX_CTX_FAULT_RUNAWAY,0,"DMA receipt size");
    void *next=realloc(*data,cap*unit);
    if(!next) psx_dctx_fault(&x->cpu,PSX_CTX_FAULT_RUNAWAY,0,"DMA receipt allocation");
    *data=next; *capacity=cap;
}
static int ctx_memory_compare(const void *a,const void *b) {
    const PsxCtxGpuMemWord *x=a,*y=b;
    return (x->address>y->address)-(x->address<y->address);
}
int psx_ctx_gpu_list_read(const PsxExecCtx *x,size_t list,uint32_t address,
                          unsigned width,uint32_t *out) {
    if(!x || !out || list>=x->gpu_lists_n || (width!=1 && width!=2 && width!=4) ||
        (address&3u)+width>4) return 0;
    const PsxCtxGpuList *g=&x->gpu_lists[list];
    uint32_t key=address & x->ram_mask & ~3u;
    size_t lo=g->memory_begin,hi=g->memory_end;
    while(lo<hi) { size_t mid=lo+(hi-lo)/2;
        if(x->gpu_memory[mid].address<key) lo=mid+1; else hi=mid;
    }
    if(lo==g->memory_end || x->gpu_memory[lo].address!=key) return 0;
    *out=x->gpu_memory[lo].word >> (8u*(address&3u));
    if(width<4) *out &= (1u<<(width*8u))-1u;
    return 1;
}

static void dma_gpu(PsxExecCtx *x) {
    const uint32_t chcr = x->dma_chcr[2];
    const uint32_t mode = (chcr >> 9) & 3u;
    if (!(chcr & 1u))
        psx_dctx_fault(&x->cpu, PSX_CTX_FAULT_DMA, chcr, "GPU DMA to RAM (VRAM readback)");
    x->stats.dma_gpu++;
    if (mode == 2u) {
        uint32_t addr = x->dma_madr[2] & 0x00FFFFFCu;
        ctx_capture_grow(x,(void **)&x->gpu_lists,&x->gpu_lists_cap,x->gpu_lists_n+1,sizeof(*x->gpu_lists));
        PsxCtxGpuList *list=&x->gpu_lists[x->gpu_lists_n++];
        *list=(PsxCtxGpuList){.word_begin=x->gpu_n,.node_begin=x->gpu_nodes_n,
                            .memory_begin=x->gpu_memory_n,.root=addr};
        for (uint32_t nodes = 0;; nodes++) {
            if (nodes > CTX_DMA_NODE_LIMIT)
                psx_dctx_fault(&x->cpu, PSX_CTX_FAULT_RUNAWAY, addr, "GPU linked list");
            const uint32_t header = ram32(x, addr);
            const uint32_t n = header >> 24;
            ctx_capture_grow(x,(void **)&x->gpu_nodes,&x->gpu_nodes_cap,x->gpu_nodes_n+1,sizeof(*x->gpu_nodes));
            x->gpu_nodes[x->gpu_nodes_n++]=(PsxCtxGpuNode){x->gpu_n,addr,header};
            ctx_capture_grow(x,(void **)&x->gpu_memory,&x->gpu_memory_cap,x->gpu_memory_n+n+1,sizeof(*x->gpu_memory));
            x->gpu_memory[x->gpu_memory_n++]=(PsxCtxGpuMemWord){addr & x->ram_mask,header};
            for (uint32_t i = 1; i <= n; i++) {
                const uint32_t a = (addr + 4u * i) & 0x00FFFFFCu & x->ram_mask;
                uint32_t word=ram32(x,a);
                x->gpu_memory[x->gpu_memory_n++]=(PsxCtxGpuMemWord){a,word};
                gpu_push(x,word,a);
            }
            if (header & 0x00800000u) break;
            addr = header & 0x00FFFFFCu;
        }
        list->word_end=x->gpu_n; list->node_end=x->gpu_nodes_n; list->memory_end=x->gpu_memory_n;
        qsort(x->gpu_memory+list->memory_begin,list->memory_end-list->memory_begin,
              sizeof(*x->gpu_memory),ctx_memory_compare);
        x->dma_madr[2] = 0x00FFFFFFu;
    } else {
        uint32_t words = x->dma_bcr[2] & 0xFFFFu;
        if (mode == 0u) { if (!words) words = 0x10000u; }
        else words = words * (x->dma_bcr[2] >> 16);
        const int step = (chcr & 2u) ? -4 : 4;
        uint32_t addr = x->dma_madr[2] & 0x00FFFFFCu;
        for (uint32_t i = 0; i < words; i++) {
            const uint32_t a = addr & x->ram_mask;
            gpu_push(x, ram32(x, a), a);
            addr = (addr + (uint32_t)step) & 0x00FFFFFCu;
        }
    }
    dma_complete(x, 2);
}

static void dma_otc(PsxExecCtx *x) {
    uint32_t n = x->dma_bcr[6] & 0xFFFFu;
    if (!n) n = 0x10000u;
    uint32_t addr = x->dma_madr[6] & 0x00FFFFFCu;
    x->stats.dma_otc++;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t v = (i == n - 1u) ? 0x00FFFFFFu : ((addr - 4u) & 0x00FFFFFFu);
        const uint32_t a = addr & x->ram_mask;
        memcpy(x->ram + a, &v, 4);
        if (x->pgxp_active) pgxp_ctx_invalidate_word(x->pgxp, 0x80000000u | a);
        addr = (addr - 4u) & 0x00FFFFFCu;
    }
    dma_complete(x, 6);
}

static void dma_kick(PsxExecCtx *x, int ch) {
    if (!(x->dma_chcr[ch] & (1u << 24))) return;
    if (!(x->dpcr & (8u << (4 * ch)))) return;   /* channel disabled: stays pending */
    x->dma_chcr[ch] &= ~(1u << 28);
    if (ch == 2) dma_gpu(x);
    else if (ch == 6) dma_otc(x);
    else psx_dctx_fault(&x->cpu, PSX_CTX_FAULT_DMA, (uint32_t)ch, "DMA channel");
}

/* ---- memory slow path ----------------------------------------------------- */

static uint32_t mmio_read32(PsxExecCtx *x, uint32_t phys) {
    if (phys == 0x1F801070u) return x->i_stat;
    if (phys == 0x1F801074u) return x->i_mask;
    if (phys >= 0x1F801080u && phys <= 0x1F8010EFu) {
        const uint32_t ch = (phys - 0x1F801080u) >> 4;
        switch (phys & 0xCu) {
        case 0x0u: return x->dma_madr[ch];
        case 0x4u: return x->dma_bcr[ch];
        case 0x8u: return x->dma_chcr[ch];
        default:   return 0u;
        }
    }
    if (phys == 0x1F8010F0u) return x->dpcr;
    if (phys == 0x1F8010F4u) {
        const uint32_t v = x->dicr;
        const int master = (v & (1u << 15)) || ((v & (1u << 23)) && (v & 0x7F000000u));
        return v | (master ? 0x80000000u : 0u);
    }
    if (phys == 0x1F801814u) return ctx_gpustat(x);
    /* Root counters: guest time does not run in a draw context. A counter
     * read returns the snapshot value advanced by one every 64 reads of it:
     * consecutive reads mostly agree (VSync(-1) re-reads until two match),
     * a loop polling for a change still makes progress, and every context
     * sees the same values for the same reads. */
    if (phys >= 0x1F801100u && phys < 0x1F801130u && (phys & 0xFu) < 0xCu) {
        const uint32_t t = (phys - 0x1F801100u) >> 4;
        switch (phys & 0xCu) {
        case 0x0u: {
            const uint32_t v = (x->timer_counter[t] + (x->timer_reads[t]++ >> 6)) & 0xFFFFu;
            return v;
        }
        case 0x4u: {
            const uint32_t m = x->timer_mode[t];
            x->timer_mode[t] &= ~(3u << 11);    /* reached-flags clear on read */
            return m;
        }
        default:   return x->timer_target[t];
        }
    }
    psx_dctx_fault(&x->cpu, PSX_CTX_FAULT_DEVICE_READ, phys,
                   phys == 0x1F801810u ? "GPUREAD" : "device register");
}

uint32_t psx_dctx_read_slow(CPUState *cpu, uint32_t addr, uint32_t size) {
    PsxExecCtx *x = cpu->ctx;
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (addr < 0xC0000000u && phys >= 0x1FC00000u && phys < 0x1FC00000u + BIOS_ROM_BYTES) {
        const uint8_t *rom = memory_get_bios_rom_ptr() + (phys - 0x1FC00000u);
        uint32_t v = 0;
        memcpy(&v, rom, size);
        return v;
    }
    if (addr < 0xC0000000u && phys >= 0x1F801000u && phys < 0x1F803000u) {
        const uint32_t v = mmio_read32(x, phys & ~3u);
        const uint32_t shift = (phys & 3u) * 8u;
        const uint32_t mask = size == 4u ? 0xFFFFFFFFu : ((1u << (size * 8u)) - 1u);
        return (v >> shift) & mask;
    }
    psx_dctx_fault(cpu, PSX_CTX_FAULT_DEVICE_READ, addr, "unmapped or unmodelled read");
}

static void mmio_write(PsxExecCtx *x, uint32_t phys, uint32_t v, uint32_t size) {
    const uint32_t reg = phys & ~3u;
    const uint32_t shift = (phys & 3u) * 8u;
    const uint32_t mask = (size == 4u ? 0xFFFFFFFFu : ((1u << (size * 8u)) - 1u)) << shift;
    v <<= shift;
    if (reg == 0x1F801810u && size == 4u) {
        gpu_push(x, v, PSX_CTX_SRC_GP0);
        x->stats.gp0_port_words++;
        return;
    }
    if (reg == 0x1F801814u && size == 4u) {
        gpu_push(x, v, PSX_CTX_SRC_GP1);
        x->stats.gp1_words++;
        if ((v >> 24) == 0x04u) x->gpu_dma_dir = v & 3u;
        return;
    }
    if (reg == 0x1F801070u) { x->i_stat &= v | ~mask; return; }
    if (reg == 0x1F801074u) { x->i_mask = (x->i_mask & ~mask) | (v & mask); return; }
    if (reg >= 0x1F801080u && reg <= 0x1F8010EFu) {
        const int ch = (int)((reg - 0x1F801080u) >> 4);
        switch (reg & 0xCu) {
        case 0x0u: x->dma_madr[ch] = (x->dma_madr[ch] & ~mask) | (v & mask); return;
        case 0x4u: x->dma_bcr[ch] = (x->dma_bcr[ch] & ~mask) | (v & mask); return;
        case 0x8u:
            x->dma_chcr[ch] = (x->dma_chcr[ch] & ~mask) | (v & mask);
            if (mask & (1u << 24)) dma_kick(x, ch);
            return;
        default: return;
        }
    }
    if (reg == 0x1F8010F0u) { x->dpcr = (x->dpcr & ~mask) | (v & mask); return; }
    if (reg == 0x1F8010F4u) {
        const uint32_t wmask = 0x00FF807Fu & mask, rmask = 0x7F000000u & mask;
        x->dicr = (x->dicr & ~wmask) | (v & wmask);
        x->dicr &= ~(v & rmask);
        return;
    }
    /* SPU, CD-ROM, timers, SIO, MDEC, memory control: a draw pass's writes to
     * them must not reach the machine. Dropped and counted, as a render pass
     * does (memory.c render_pass_store). */
    x->stats.dropped_writes++;
}

void psx_dctx_write_slow(CPUState *cpu, uint32_t addr, uint32_t value, uint32_t size) {
    PsxExecCtx *x = cpu->ctx;
    if (cpu->cop0[12] & 0x10000u) { x->stats.isolated_stores++; return; }
    if (addr >= 0xC0000000u) { x->stats.dropped_writes++; return; }   /* cache control */
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys >= 0x1F801000u && phys < 0x1F803000u) { mmio_write(x, phys, value, size); return; }
    if (phys >= 0x1FC00000u && phys < 0x1FC00000u + BIOS_ROM_BYTES) return;   /* ROM */
    if (phys >= 0x1F000000u && phys < 0x1F800000u) { x->stats.dropped_writes++; return; }
    psx_dctx_fault(cpu, PSX_CTX_FAULT_UNSUPPORTED, addr, "unmapped write");
}

/* ---- host accessors ------------------------------------------------------- */

uint32_t psx_ctx_read32(PsxExecCtx *x, uint32_t addr) {
    uint8_t *p = psx_dctx_ptr(x, addr & ~3u);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

void psx_ctx_write32(PsxExecCtx *x, uint32_t addr, uint32_t value) {
    uint8_t *p = psx_dctx_ptr(x, addr & ~3u);
    if (!p) return;
    memcpy(p, &value, 4);
    if (x->pgxp_active) pgxp_ctx_invalidate_word(x->pgxp, addr & ~3u);
}

const PsxCtxGpuWord *psx_ctx_gpu_words(const PsxExecCtx *x, size_t *count) {
    if (count) *count = x ? x->gpu_n : 0u;
    return x ? x->gpu : NULL;
}

int psx_ctx_precise_vertex(PsxExecCtx *x, const PsxCtxGpuWord *w, int32_t int_x,
                           int32_t int_y, int32_t *x16, int32_t *y16, uint16_t *sz) {
    if (!x || !w || !x16 || !y16 || !sz) return 0;
    const int32_t ix = (int32_t)((w->word & 0x7ffu) ^ 0x400u) - 0x400;
    const int32_t iy = (int32_t)(((w->word >> 16) & 0x7ffu) ^ 0x400u) - 0x400;
    if (int_x != ix || int_y != iy) {
        *x16 = int_x * 65536; *y16 = int_y * 65536; *sz = 0;
        return 0;
    }
    *x16 = w->vertex.x16; *y16 = w->vertex.y16; *sz = w->vertex.sz;
    return w->vertex.source;
}

const PsxCtxStats *psx_ctx_stats(const PsxExecCtx *x) { return x ? &x->stats : NULL; }

int psx_ctx_trail(const PsxExecCtx *x, uint32_t *out, int cap, uint32_t *fault_ra) {
    if (fault_ra) *fault_ra = x ? x->fault_ra : 0u;
    if (!x) return 0;
    const uint32_t n = x->trail_n < 16u ? x->trail_n : 16u;
    int k = 0;
    for (uint32_t i = x->trail_n - n; i < x->trail_n && k < cap; i++)
        out[k++] = x->trail[i & 15u];
    return k;
}

void psx_ctx_set_budget(PsxExecCtx *x, uint64_t dispatches, uint64_t blocks) {
    x->step_limit = dispatches ? dispatches : CTX_DEFAULT_STEP_LIMIT;
    x->block_limit = blocks ? blocks : CTX_DEFAULT_BLOCK_LIMIT;
}

PsxCtxStatus psx_ctx_status(const PsxExecCtx *x, uint32_t *fault_pc, const char **what) {
    if (fault_pc) *fault_pc = x ? x->fault_pc : 0u;
    if (what) *what = x ? x->fault_what : NULL;
    return x ? x->status : PSX_CTX_FAULT_BAD_ARG;
}
