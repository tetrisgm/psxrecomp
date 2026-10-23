#define PSX_DRAW_CTX_BUILD 1
#define PSX_NO_DEBUG_TOOLS 1
#include "psx_dctx_bios.h"
#include "psx_dctx_mem.h"
#include "pgxp_ctx.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define CHECK(v) do { if (!(v)) { puts(#v); abort(); } } while (0)
static uint8_t rom[512 * 1024];
static _Thread_local CPUState *reference_cpu;
static _Thread_local uint32_t g_debug_last_store_pc;
static _Thread_local uint64_t event_hash, event_count;
static _Thread_local int in_memcpy;
static void event(uint32_t kind, uint32_t i, uint32_t v, uint32_t a, uint32_t b) {
    /* Start after the qualified vector/dispatcher; the reference body starts
     * at memcpy itself. Compare every hook's instruction and operands. */
    if (i == 0x00801025u) { event_hash=1469598103934665603ull; event_count=0; in_memcpy=1; }
    if (!in_memcpy) return;
    const uint32_t values[]={kind,i,v,a,b};
    for(unsigned k=0;k<5;k++) { event_hash ^= values[k]; event_hash *= 1099511628211ull; }
    ++event_count;
}
uint32_t psx_dctx_read_slow(CPUState *c, uint32_t a, uint32_t n) {
    (void)c; uint32_t p = a & 0x1FFFFFFFu, v = 0;
    CHECK(p >= 0x1FC00000u && p + n <= 0x1FC80000u);
    memcpy(&v, rom + p - 0x1FC00000u, n); return v;
}
void psx_dctx_write_slow(CPUState *c, uint32_t a, uint32_t v, uint32_t n) {
    (void)c; (void)a; (void)v; (void)n; abort();
}
void psx_dctx_fault(CPUState *c, int status, uint32_t pc, const char *what) {
    (void)pc; (void)what; c->ctx->status = status; longjmp(c->ctx->abort, 1);
}
/* Compare the PGXP call contract; engine state semantics have their own tests. */
void pgxp_ctx_alu(PgxpCtxEngine *e, CPUState *c, uint32_t i, uint32_t v, uint32_t a, uint32_t b) {
    (void)e;(void)c; event(0,i,v,a,b);
}
void pgxp_ctx_load(PgxpCtxEngine *e, CPUState *c, uint32_t i, uint32_t a, uint32_t v) {
    (void)e;(void)c; event(1,i,v,a,0);
}
void pgxp_ctx_store(PgxpCtxEngine *e, CPUState *c, uint32_t i, uint32_t a, uint32_t v) {
    (void)e;(void)c; event(2,i,v,a,0);
}
static void reference_store(uint32_t a, uint8_t v) { psx_dctx_sb(reference_cpu, a, v); }
#define debug_server_log_call_entry(a) ((void)0)
#define psx_check_interrupts_at(c,a) ((void)0)
#undef PGXP_ALU
#undef PGXP_LOAD
#undef PGXP_STORE
#define PGXP_ALU(i,v,a,b) pgxp_ctx_alu(NULL,cpu,i,v,a,b)
#define PGXP_LOAD(i,a,v) pgxp_ctx_load(NULL,cpu,i,a,v)
#define PGXP_STORE(i,a,v) pgxp_ctx_store(NULL,cpu,i,a,v)
#define psx_cyc_load_byte(c,a,r,m) psx_dctx_lb(c,a)
#include "dctx_bios_reference.inc"

static PsxExecCtx *make_context(void) {
    PsxExecCtx *x = calloc(1, sizeof *x); CHECK(x);
    x->ram_mask = 0x1FFFFFu;
    x->ram = malloc(x->ram_mask + 1u); CHECK(x->ram);
    for (unsigned i=0;i<=x->ram_mask;i++) x->ram[i] = (uint8_t)(i * 37u + i / 11u);
    x->cpu.ctx=x; x->block_budget=100000; x->pgxp_active=1;
    const uint32_t vector[]={0x24082A80,0x01000008,0};
    const uint32_t dispatch[]={0x3C080000,0x25080200,0x00095080,0x01485020,0x8D4A0000,0x240800A0,0x01400008,0};
    memcpy(x->ram+0xA0,vector,sizeof vector); memcpy(x->ram+0x2A80,dispatch,sizeof dispatch);
    psx_dctx_sw(&x->cpu,0x2A8,0xBFC085D8);
    x->cpu.gpr[9]=0x2A; x->cpu.gpr[31]=0x80010000;
    return x;
}
static void destroy(PsxExecCtx *x) { free(x->ram); free(x); }
static void *corpus(void *unused) {
    (void)unused;
    const uint32_t cases[][3]={{0x80010000,0x80011000,0},{0x80010001,0x80011003,513},
        {0x80011001,0x80011000,127},{0x80011000,0x80011001,127},
        {0x801FFFF0,0x80011000,64},{0x1F800001,0x80011000,500},
        {0x80011000,0x1F800003,500},{0xA0011000,0x80011000,32}};
    for (unsigned k=0;k<sizeof cases/sizeof cases[0];k++) {
        PsxExecCtx *x=make_context(), *r=make_context();
        for(unsigned i=0;i<3;i++) x->cpu.gpr[4+i]=r->cpu.gpr[4+i]=cases[k][i];
        reference_cpu=&r->cpu; r->cpu.write_byte=reference_store;
        OpenBIOS_func_1FC085D8(&r->cpu);
        const uint64_t expected_hash=event_hash, expected_count=event_count;
        in_memcpy=0;
        CHECK(psx_dctx_bios_call(&x->cpu,0xA0));
        CHECK(event_hash==expected_hash && event_count==expected_count);
        CHECK(!memcmp(x->ram,r->ram,x->ram_mask+1u)); CHECK(!memcmp(x->scratch,r->scratch,1024));
        for(unsigned reg=0;reg<32;reg++) if(reg!=8 && reg!=10) CHECK(x->cpu.gpr[reg]==r->cpu.gpr[reg]);
        CHECK(x->cpu.pc==r->cpu.pc); CHECK(x->cpu.gpr[8]==0xA0); CHECK(x->cpu.gpr[10]==0xBFC085D8);
        destroy(x); destroy(r);
    }
    PsxExecCtx *x=make_context();
    CPUState before=x->cpu;
    x->ram[0xA0]^=1; CHECK(!psx_dctx_bios_call(&x->cpu,0xA0)); CHECK(!memcmp(&before,&x->cpu,sizeof before));
    x->ram[0xA0]^=1; x->ram[0x2A80]^=1; CHECK(!psx_dctx_bios_call(&x->cpu,0xA0));
    x->ram[0x2A80]^=1; psx_dctx_sw(&x->cpu,0x2A8,0xBFC085DC); CHECK(!psx_dctx_bios_call(&x->cpu,0xA0));
    psx_dctx_sw(&x->cpu,0x2A8,0xBFC085D8); x->cpu.gpr[4]=0x80010000; x->cpu.gpr[5]=0x80011000; x->cpu.gpr[6]=100; x->block_budget=3;
    if(!setjmp(x->abort)) { psx_dctx_bios_call(&x->cpu,0xA0); abort(); }
    CHECK(x->status==PSX_CTX_FAULT_RUNAWAY);
    destroy(x); return NULL;
}
int main(int argc,char **argv) {
    CHECK(argc==2); FILE *f=fopen(argv[1],"rb"); CHECK(f); CHECK(fread(rom,1,sizeof rom,f)==sizeof rom); fclose(f);
    corpus(NULL); pthread_t threads[4];
    for(unsigned i=0;i<4;i++) CHECK(!pthread_create(&threads[i],NULL,corpus,NULL));
    for(unsigned i=0;i<4;i++) CHECK(!pthread_join(threads[i],NULL));
    puts("draw BIOS memory/ABI corpus: serial and four threads passed against recompiled OpenBIOS");
}
