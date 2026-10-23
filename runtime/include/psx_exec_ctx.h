#ifndef PSXRECOMP_PSX_EXEC_CTX_H
#define PSXRECOMP_PSX_EXEC_CTX_H

/* Per-thread guest execution contexts for draw-only passes
 * (docs/THREADED_CONTEXTS.md).
 *
 * The main guest context is the process: global RAM, scratchpad, GTE side
 * state, PGXP engine, cycle/interrupt model, devices and mod hooks, driven by
 * the full faithful recompiled code. Nothing in this header changes it.
 *
 * A draw context runs the LEAN draw clones of a game's draw functions
 * (generated/<stem>_draw.c, emitted for the [threaded_draw] roots of
 * game.toml and their static callees) against private state taken from a
 * snapshot of the main context:
 *   - RAM and scratchpad: private copies (stores never reach the machine);
 *   - CPU and GTE registers: the context's CPUState;
 *   - GTE side state and a PGXP engine of its own (gte_ctx.h, pgxp_ctx.h);
 *   - no cycle, I-cache, interrupt, debug or cosim model at all;
 *   - GPU: GP0/GP1 writes and GPU (channel 2) DMA are captured into the
 *     context's command list, with each DMA word's source address so its
 *     PGXP precision can be looked up afterwards; OTC (channel 6) DMA fills
 *     the private RAM; DMA, GPUSTAT and interrupt-controller registers are
 *     private copies;
 *   - every other device: writes are dropped and counted, reads fault.
 * A context runs on whatever thread calls it; any number of contexts may run
 * at once, concurrently with the main context, from one snapshot. Faults
 * (a dispatch miss outside the draw clones, an unsupported device read, a
 * runaway) abort the call with a status, never touch the machine. */

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

struct PgxpCtxEngine;
struct PsxGteCtx;
typedef struct PsxCtxSnapshot PsxCtxSnapshot;
typedef struct PsxExecCtx PsxExecCtx;

#define PSX_CTX_MAX 16              /* live contexts per process */
#define PSX_CTX_STOP_PC 0xFFFFFFF0u  /* default return address of psx_ctx_call */

/* One captured GPU word. src is the guest RAM address a GPU DMA read it
 * from, or one of the port tags for a CPU write to the GPU registers. */
#define PSX_CTX_SRC_GP0 0xFFFFFFFFu
#define PSX_CTX_SRC_GP1 0xFFFFFFFEu
typedef struct PsxCtxGpuWord {
    uint32_t word;
    uint32_t src;
} PsxCtxGpuWord;

typedef enum {
    PSX_CTX_OK = 0,
    PSX_CTX_FAULT_MISS,         /* transfer to a PC with no draw clone */
    PSX_CTX_FAULT_TEXT,         /* clone's code bytes differ in the snapshot */
    PSX_CTX_FAULT_DEVICE_READ,  /* read of a device a context does not model */
    PSX_CTX_FAULT_DMA,          /* DMA a context does not model */
    PSX_CTX_FAULT_UNSUPPORTED,  /* runtime service a clone may not use */
    PSX_CTX_FAULT_EXCEPTION,    /* syscall/break/overflow/address error */
    PSX_CTX_FAULT_RUNAWAY,      /* dispatch step or DMA budget exhausted */
    PSX_CTX_FAULT_NO_TABLE,     /* this build has no draw clones */
    PSX_CTX_FAULT_BAD_ARG,
} PsxCtxStatus;

typedef struct PsxCtxStats {
    uint64_t dispatches;        /* trampoline transfers */
    uint64_t gpu_words;         /* captured words */
    uint64_t gp0_port_words, gp1_words;
    uint64_t dma_gpu, dma_otc;  /* channel 2 / 6 transfers */
    uint64_t dropped_writes;    /* device writes a context drops */
    uint64_t hooks_run, hooks_stripped;
    uint64_t isolated_stores;   /* SR.IsC stores (cache, not RAM) */
} PsxCtxStats;

/* ---- hot state read by the draw clones (psx_dctx_codegen.h) --------------
 * Layout is private to this runtime build; only generated draw clones and the
 * runtime read it. */
struct PsxExecCtx {
    uint8_t *ram;                /* private RAM, live size */
    uint32_t ram_mask;           /* g_psx_ram_mask of the snapshot */
    int32_t  ws_x_margin;        /* psx_ws_x_margin() of the snapshot */
    int      pgxp_active;
    int      entry_hooks;        /* registered draw-context entry hooks */
    uint64_t block_budget;       /* clone blocks left in this call */
    struct PgxpCtxEngine *pgxp;
    struct PsxGteCtx *gte;
    uint8_t  scratch[1024];
    /* ---- cold ---- */
    CPUState cpu;
    int      slot;
    const PsxCtxSnapshot *snap;
    PsxCtxStatus status;
    uint32_t fault_pc;           /* PC / address the fault names */
    uint32_t fault_ra;           /* $ra at the fault */
    uint32_t trail[16];          /* last dispatched PCs, newest at trail_n-1 */
    uint32_t trail_n;
    const char *fault_what;
    jmp_buf  abort;
    int      in_call;
    PsxCtxGpuWord *gpu;
    size_t   gpu_n, gpu_cap;
    uint32_t gpu_dma_dir;        /* GP1(04) */
    uint32_t gpustat;            /* snapshot GPUSTAT (ready bits forced) */
    uint32_t dma_madr[7], dma_bcr[7], dma_chcr[7];
    uint32_t dpcr, dicr, i_stat, i_mask;
    uint32_t timer_counter[3], timer_mode[3], timer_target[3], timer_reads[3];
    uint8_t *text_ok;            /* per draw-table row: 0 unknown, 1 ok, 2 bad */
    size_t   text_ok_n;
    uint64_t step_limit, block_limit;
    PsxCtxStats stats;
};

/* ---- snapshots (main emulation thread only) ------------------------------ */
enum {
    PSX_CTX_SNAP_PGXP = 1u << 0,   /* carry the PGXP configuration/registers */
};
/* Copy what a draw pass needs out of the main context. Read-only on the
 * machine: no device service, no cycle publication, no guest-visible change.
 * Returns NULL on allocation failure. */
PsxCtxSnapshot *psx_ctx_snapshot_capture(const CPUState *cpu, unsigned flags);
void psx_ctx_snapshot_release(PsxCtxSnapshot *snap);
/* Guest RAM of the snapshot (for the caller's own reads). */
const uint8_t *psx_ctx_snapshot_ram(const PsxCtxSnapshot *snap, uint32_t *size);

/* ---- contexts ------------------------------------------------------------ */
PsxExecCtx *psx_ctx_create(void);
void psx_ctx_destroy(PsxExecCtx *x);
/* Reset the context to `snap`: RAM, scratchpad, registers, GTE/PGXP side
 * state, device registers; clears the command list and stats. Any thread. */
int  psx_ctx_begin(PsxExecCtx *x, const PsxCtxSnapshot *snap);
CPUState *psx_ctx_cpu(PsxExecCtx *x);

/* Call guest function `fn` with a0..a3 = args[0..3] (nargs <= 4; stack
 * arguments are the caller's to write) and $ra = stop_pc (0 picks
 * PSX_CTX_STOP_PC). Runs draw clones until the guest returns to stop_pc on
 * the entry $sp. Returns PSX_CTX_OK and the guest's v0 in *ret, or a fault
 * (psx_ctx_fault_*). The context's RAM and command list keep everything the
 * call did; a later call continues from there. */
PsxCtxStatus psx_ctx_call(PsxExecCtx *x, uint32_t fn, const uint32_t *args,
                          int nargs, uint32_t stop_pc, uint32_t *ret);

/* Context memory for host code between calls (aligned; RAM + scratchpad). */
uint32_t psx_ctx_read32(PsxExecCtx *x, uint32_t addr);
void     psx_ctx_write32(PsxExecCtx *x, uint32_t addr, uint32_t value);

/* Captured GPU command words since psx_ctx_begin. */
const PsxCtxGpuWord *psx_ctx_gpu_words(const PsxExecCtx *x, size_t *count);
/* PGXP precision of a captured word (pgxp_get_precise_vertex semantics on
 * the context's engine; int_x/int_y are the packet's 11-bit integers). */
int psx_ctx_precise_vertex(PsxExecCtx *x, const PsxCtxGpuWord *w,
                           int32_t int_x, int32_t int_y,
                           int32_t *x16, int32_t *y16, uint16_t *sz);
const PsxCtxStats *psx_ctx_stats(const PsxExecCtx *x);
PsxCtxStatus psx_ctx_status(const PsxExecCtx *x, uint32_t *fault_pc,
                            const char **what);
/* The last dispatched PCs before a fault, oldest first; returns the count. */
int psx_ctx_trail(const PsxExecCtx *x, uint32_t *out, int cap, uint32_t *fault_ra);
/* Per-call budgets (0 keeps the default): trampoline dispatches and clone
 * blocks. Exhausting either faults the call as PSX_CTX_FAULT_RUNAWAY. */
void psx_ctx_set_budget(PsxExecCtx *x, uint64_t dispatches, uint64_t blocks);
const char *psx_ctx_status_name(PsxCtxStatus s);

/* Entry hooks a draw context runs (the draw-pass plugin hooks a lean clone
 * keeps; every other psx_mod_function_entry site is stripped). Register
 * before running contexts; the callback runs on the context's thread with
 * the context's CPUState and may only use psx_ctx_* on psx_ctx_of(cpu).
 * Returns nonzero to complete the guest function itself (pc <- $ra). */
typedef int (*PsxCtxEntryHook)(CPUState *cpu, uint32_t address, void *user);
int psx_ctx_register_entry_hook(uint32_t address, PsxCtxEntryHook hook, void *user);
void psx_ctx_clear_entry_hooks(void);
static inline PsxExecCtx *psx_ctx_of(CPUState *cpu) { return cpu->ctx; }

/* Whether this build has draw clones, and of which roots. */
int psx_ctx_available(void);
int psx_ctx_draw_function(uint32_t addr);   /* 1 iff addr has a draw clone entry */

/* ---- generated draw table (psx_dctx_codegen.h) --------------------------- */
typedef void (*PsxDctxFn)(CPUState *cpu);
typedef struct PsxDctxRow {
    uint32_t key;         /* PC */
    uint32_t resume;      /* 0 = function entry, else continuation PC */
    uint32_t range_index, range_count;
    PsxDctxFn fn;
} PsxDctxRow;
typedef struct PsxDctxTable {
    const char *stem;
    const PsxDctxRow *rows;
    uint32_t row_count;
    const uint32_t *ranges;   /* {lo, len} pairs */
    uint32_t range_count;
    const uint32_t *roots;
    uint32_t root_count;
} PsxDctxTable;
void psx_dctx_register_table(const PsxDctxTable *table);

/* Runtime services of the draw clones (psx_dctx_codegen.h). */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void psx_dctx_fault(CPUState *cpu, int status, uint32_t pc, const char *what);
int  psx_dctx_text_ok(CPUState *cpu, uint32_t addr);
int  psx_dctx_entry_hook(CPUState *cpu, uint32_t addr);
int  psx_dctx_syscall(CPUState *cpu, uint32_t code);
void psx_dctx_note_stripped_hook(CPUState *cpu);

#ifdef __cplusplus
}
#endif

#endif
