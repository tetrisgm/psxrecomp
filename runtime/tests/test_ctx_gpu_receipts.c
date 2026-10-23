/* Real context DMA capture: packet reuse must not rewrite prior receipts. */
#include "../src/psx_exec_ctx.c"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
void pgxp_ctx_capture_vertex(PgxpCtxEngine *e,uint32_t a,uint32_t w,PgxpCtxVertex *v) {
    (void)e;(void)a;(void)w; memset(v,0,sizeof(*v));
}
int main(void) {
    PsxExecCtx x={0}; x.cpu.ctx=&x; x.ram_mask=0x1FFFFFu;
    x.ram=calloc(1,(size_t)x.ram_mask+1); assert(x.ram);
    if(setjmp(x.abort)) { fprintf(stderr,"fault: %s\n",x.fault_what); return 1; }
    uint32_t *ram=(uint32_t *)x.ram;
    ram[0x100/4]=0x200; /* empty OT node */
    ram[0x200/4]=0x02800000; ram[0x204/4]=0xE3000402; ram[0x208/4]=0xE4000803;
    x.dma_chcr[2]=1u|(2u<<9); x.dma_madr[2]=0x100; dma_gpu(&x);
    assert(x.gpu_lists_n==1 && x.gpu_nodes_n==2 && x.gpu_n==2);
    assert(x.gpu_nodes[0].word_offset==0 && x.gpu_nodes[1].word_offset==0);
    assert(x.gpu_lists[0].word_end==2 && x.gpu_lists[0].root==0x100);
    ram[0x204/4]=0xE3000C04;
    x.dma_madr[2]=0x100; dma_gpu(&x);
    assert(x.gpu_lists_n==2 && x.gpu_nodes_n==4 && x.gpu_n==4);
    uint32_t v;
    assert(psx_ctx_gpu_list_read(&x,0,0x80000204,4,&v) && v==0xE3000402);
    assert(psx_ctx_gpu_list_read(&x,1,0x204,4,&v) && v==0xE3000C04);
    assert(psx_ctx_gpu_list_read(&x,0,0x206,2,&v) && v==0xE300);
    assert(!psx_ctx_gpu_list_read(&x,0,0x300,4,&v));
    assert(!psx_ctx_gpu_list_read(&x,0,0x207,2,&v));
    assert(!psx_ctx_gpu_list_read(&x,2,0x204,4,&v));
    free(x.ram); free(x.gpu); free(x.gpu_nodes); free(x.gpu_memory); free(x.gpu_lists);
    puts("ctx_gpu_receipts: PASS"); return 0;
}
