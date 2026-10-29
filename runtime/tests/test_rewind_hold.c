/* Exercise the production hold controller with deterministic device doubles. */
#include "../src/psx_rewind.c"
#include <assert.h>
#include <stdio.h>
static uint64_t clock_ns;
static int muted, presented, resumed, netplay, safe=1;
static uint8_t ram_image[8193];
uint64_t pt_clock_ns(void) { return clock_ns; }
void psx_host_sleep_ms(unsigned ms) { clock_ns+=(uint64_t)ms*1000000; }
int psx_netplay_active(void) { return netplay; }
int psx_is_dispatchable(uint32_t pc) { return pc==0x80010000; }
int psx_irq_resume_context_snapshot_site(void) { return 0; }
uint32_t psx_irq_resume_context_snapshot_pc(void) { return 0; }
uint32_t psx_compiled_irq_resume_pc(void) { return 0; }
uint32_t psx_last_irq_check_pc(void) { return 0; }
uint32_t psx_netplay_rb_sticky_bb_pc(void) { return 0; }
int psx_irq_resume_context_snapshot_safe_at(uint32_t pc) { (void)pc; return safe; }
int psx_hle_scheduler_enabled(void) { return 1; }
void psx_cycles_resync_after_restore(CPUState *cpu) { (void)cpu; }
void interrupts_resync_after_restore(void) {}
void psx_frontend_on_savestate_loaded(void) {}
void psx_scheduler_resume_at(uint32_t pc) { assert(pc==0x80010000); resumed++; }
void psx_frontend_rewind_audio(int pause) { muted=pause; }
void psx_frontend_rewind_service(int present) {
    assert(muted);
    if(present) {
        presented++;
        assert(ram_image[8192]==(uint8_t)(5-presented));
        if(presented==3) psx_rewind_set_held(0);
    }
}
int boot_state_save_buffer_reuse(const CPUState *cpu,uint32_t b,uint32_t e,
        uint8_t **data,size_t *cap,size_t *len) {
    (void)b;(void)e;
    *len=sizeof(*cpu)+sizeof(ram_image);
    if(*cap<*len) { *data=realloc(*data,*len); *cap=*len; }
    assert(*data);
    memcpy(*data,cpu,sizeof(*cpu)); memcpy(*data+sizeof(*cpu),ram_image,sizeof(ram_image));
    return 1;
}
struct BootStatePending { uint8_t data[sizeof(CPUState)+8193];int polls; };
BootStatePending *boot_state_pending_create(void) { return calloc(1,sizeof(BootStatePending)); }
void boot_state_pending_destroy(BootStatePending *p) { free(p); }
int boot_state_begin_native(const CPUState *cpu,uint32_t b,uint32_t e,BootStateSink sink,void *context,BootStatePending *p) {
    (void)b;(void)e;p->polls=0;
    memcpy(p->data,cpu,sizeof(*cpu));memcpy(p->data+sizeof(*cpu),ram_image,sizeof(ram_image));
    return sink(context,NULL,sizeof(p->data));
}
int boot_state_pending_finish(BootStatePending *p,BootStatePatch patch,void *context) {
    if(++p->polls<2)return 0;
    return patch(context,0,p->data,sizeof(p->data))?1:-1;
}
int boot_state_visit_native(const CPUState *cpu,uint32_t b,uint32_t e,BootStateSink sink,void *context) {
    (void)b;(void)e; return sink(context,cpu,sizeof(*cpu)) && sink(context,ram_image,sizeof(ram_image));
}
int boot_state_load_buffer(const uint8_t *data,size_t size,uint32_t b,uint32_t e,CPUState *cpu) {
    (void)b;(void)e; assert(size==sizeof(*cpu)+sizeof(ram_image));
    memcpy(cpu,data,sizeof(*cpu)); memcpy(ram_image,data+sizeof(*cpu),sizeof(ram_image)); return 1;
}
int main(void) {
    CPUState cpu={0}; cpu.pc=0x80010000;
    psx_rewind_set_enabled(1); psx_rewind_configure(0,0); psx_rewind_debug_verify(1);
    for(unsigned i=1;i<=5;i++) {
        cpu.gpr[1]=i; ram_image[8192]=(uint8_t)i;
        psx_rewind_note_frame(); psx_rewind_poll(&cpu,cpu.pc);
        psx_rewind_poll(&cpu,cpu.pc); psx_rewind_poll(&cpu,cpu.pc);
    }
    assert(stats.captures==5);
    psx_rewind_set_held(1); safe=0; psx_rewind_poll(&cpu,cpu.pc);
    assert(!presented && !resumed);
    safe=1; psx_rewind_poll(&cpu,cpu.pc);
    assert(presented==3 && resumed==1 && !muted && !active);
    assert(cpu.gpr[1]==2 && stats.verified==3 && stats.verify_failures==0);
    assert(history.tick==2 && history.count==1);
    cpu.gpr[1]=99; psx_rewind_note_frame(); psx_rewind_poll(&cpu,cpu.pc);
        psx_rewind_poll(&cpu,cpu.pc); psx_rewind_poll(&cpu,cpu.pc);
    assert(history.tick==3 && history.count==2);
    netplay=1; psx_rewind_note_frame(); psx_rewind_poll(&cpu,cpu.pc);
        psx_rewind_poll(&cpu,cpu.pc); psx_rewind_poll(&cpu,cpu.pc);
    assert(stats.captures==6);
    netplay=0; psx_rewind_set_title_blocked(1);
    psx_rewind_note_frame(); psx_rewind_poll(&cpu,cpu.pc);
        psx_rewind_poll(&cpu,cpu.pc); psx_rewind_poll(&cpu,cpu.pc); assert(stats.captures==6);
    psx_rewind_shutdown(); assert(!psx_rewind_snap_count());
    puts("hold rewind: safe boundary, reverse frames, mute, release, branch, gates PASS");
    return 0;
}
