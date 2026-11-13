/* Actual rewind request/poll, actual interrupt continuation gate and actual
 * rbengine ring. The serializer is a counted two-register machine double:
 * checked-load rejection intentionally happens AFTER it changes the CPU. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "interrupts.c"
#include "psx_rewind.c"
#include "pst_wire.h"
#include <assert.h>
#if defined(PSX_SDL3)
#include "psx_sdl.h"
#endif

static CPUState machine;
static uint32_t host_pacing;
static int function_entry_active, provider_ready = 1, capture_fails;
static int capture_calls, load_attempts, quiet_rollbacks, observations;
static int frontend_restores, cycle_restores, gpu_present_resets, cd_restores, resumes;
static uint32_t hd_live;
#if PSX_NATIVE_CAPTURE_JOBS
static void (*queued_complete)(void *);
static void *queued_context;
static uint32_t queued_value;
static int in_gpu_callback;
void gr_snapshot_submit(uint16_t *vram, void (*complete)(void *), void *context) {
    assert(!queued_complete); memset(vram,0,1024*512*2);
    queued_value=machine.gpr[4]; queued_complete=complete; queued_context=context;
}
void gr_snapshot_barrier(void) {
    if(queued_complete) {
        in_gpu_callback=1; queued_complete(queued_context); in_gpu_callback=0;
        queued_complete=NULL;
    }
}
#endif
static size_t hd_blob_size = 8;
static jmp_buf resumed;
int g_cosim_dirty_pump_site, g_call_unit_depth, g_psx_dispatch_depth;
uint32_t g_dirty_safe_resume_pc;
int g_ls_suppress_record;

int psx_mod_function_entry_active(void) { return function_entry_active; }
int psx_mod_snapshot_ready(void) { return provider_ready; }
int psx_netplay_active(void) { return 0; }
int gpu_display_is_depth24(void) { return 0; }
int mdec_recently_active(uint32_t frames) { (void)frames;return 0; }
int psx_hle_scheduler_enabled(void) { return 1; }
int psx_is_dispatchable(uint32_t pc) { return pc == 0x80025070u || pc == 0x80025074u; }
uint32_t psx_netplay_rb_sticky_bb_pc(void) { return 0; }
uint32_t sio_get_seq(void) { return 0; }
uint64_t psx_get_cycle_count(void) { return 0; }
void gpu_vblank_clear_deferred_present(void) { ++gpu_present_resets; }
void gpu_get_display_info(GpuDisplayInfo *out) { memset(out,0,sizeof(*out)); out->disabled=1; }
uint32_t gpu_display_pixel_argb(const GpuDisplayInfo *out,uint32_t x,uint32_t y) {
    (void)out; (void)x; (void)y; assert(0); return 0;
}
void gpu_display_pixel_rgb_in(const uint16_t *image,const GpuDisplayInfo *di,
                              uint32_t x,uint32_t y,uint8_t *r,uint8_t *g,uint8_t *b) {
    (void)image;(void)di;(void)x;(void)y;(void)r;(void)g;(void)b;assert(0);
}
void host_osd_push(const char *text,int ms) { (void)text; (void)ms; }
void psx_cycles_resync_after_restore(CPUState *cpu) { assert(cpu==&machine); ++cycle_restores; }
void cdrom_accelerate_after_savestate(void) { ++cd_restores; }
void psx_frontend_on_savestate_loaded(void) { ++frontend_restores; hd_live=0; }
int gpu_hd_textures_residency_save(uint8_t **data,size_t *size) {
    *data=malloc(hd_blob_size); assert(*data); *size=hd_blob_size;
#if PSX_NATIVE_CAPTURE_JOBS
    uint32_t value=in_gpu_callback ? queued_value : machine.gpr[4];
    memcpy(*data,&value,sizeof value);
#else
    memcpy(*data,&machine.gpr[4],sizeof(uint32_t));
#endif
    return 1;
}
int gpu_hd_textures_residency_load(const uint8_t *data,size_t size) {
    assert(data && size>=sizeof(uint32_t)); memcpy(&hd_live,data,sizeof(hd_live)); return 1;
}
void psx_scheduler_resume_at(uint32_t pc) {
    assert(pc==machine.pc && psx_is_dispatchable(pc)); ++resumes; longjmp(resumed,1);
}

static uint8_t *machine_image(uint32_t pc,uint32_t value) {
    uint8_t *data=malloc(8); PstW w; assert(data);
    pst_w_init(&w,data,8); assert(pst_w_u32(&w,pc) && pst_w_u32(&w,value));
    return data;
}
int boot_state_save_buffer_raw(const CPUState *cpu,uint32_t bios,uint32_t entry,
                               uint8_t **data,size_t *len) {
    assert(bios==123 && entry==0x801b0000u); ++capture_calls;
    *data=NULL; *len=0;
    if(capture_fails)return 0;
    *data=machine_image(cpu->pc,cpu->gpr[4]); *len=8; return 1;
}
#if PSX_NATIVE_CAPTURE_JOBS
int boot_state_capture_deferred_vram(const CPUState *cpu,uint32_t bios,uint32_t entry,
        uint8_t **data,size_t *capacity,size_t *len,size_t *offset) {
    assert(bios==123 && entry==0x801b0000u); ++capture_calls;
    if(capture_fails)return 0;
    size_t need=8+1024*512*2;
    if(*capacity<need){*data=realloc(*data,need);assert(*data);*capacity=need;}
    PstW w;pst_w_init(&w,*data,8);
    assert(pst_w_u32(&w,cpu->pc) && pst_w_u32(&w,cpu->gpr[4]));
    *len=need;*offset=8;return 1;
}
#endif
static int restore_image(const uint8_t *data,size_t len,CPUState *cpu) {
    PstR r; uint32_t pc,value;
    if(!data || (len!=8 && len!=8+1024*512*2))return 0;
    pst_r_init(&r,data,len);
    if(!pst_r_u32(&r,&pc) || !pst_r_u32(&r,&value))return 0;
    cpu->pc=pc; cpu->gpr[4]=value; return 1;
}
int boot_state_load_buffer_checked(const uint8_t *data,size_t len,uint32_t bios,
                                   uint32_t entry,CPUState *cpu,BootStateResumeAccept accept) {
    assert(bios==123 && entry==0x801b0000u); ++load_attempts;
    if(!restore_image(data,len,cpu) || (accept && !accept(cpu->pc)))return 0;
    ++observations; return 1;
}
int boot_state_load_buffer_quiet(const uint8_t *data,size_t len,uint32_t bios,
                                 uint32_t entry,CPUState *cpu) {
    assert(bios==123 && entry==0x801b0000u); ++quiet_rollbacks;
    return restore_image(data,len,cpu);
}

static void capture(uint32_t tick,uint32_t value) {
    machine.pc=0x80025070u; machine.gpr[4]=value;host_pacing=value+1;
    s_frame=tick; s_capture_due=1;
    psx_rewind_poll(&machine,machine.pc);
#if PSX_NATIVE_CAPTURE_JOBS
    assert(s_pending.active && !atomic_load(&s_pending.ready));
    uint32_t count=s_count;
    host_pacing=0xabcdef;
    machine.gpr[4]=0xdeadbeef; /* guest advances while render work is pending */
    pending_finish(1); assert(s_count==count);
    gr_snapshot_barrier();
    pending_finish(1);
    assert(machine.gpr[4]==0xdeadbeef);
    machine.gpr[4]=value;
#endif
    assert(!s_capture_due && s_last_capture_frame==tick && rbe_snap_ring_has(s_ring,tick));
}
static void choose_first(void) {
    assert(psx_rewind_toggle() && s_open);
    while(s_sel>0)psx_rewind_move(-1);
    assert(psx_rewind_accept() && s_load_pending && s_load_tick==10);
}
static void assert_deferred(void) {
    psx_rewind_poll(&machine,machine.pc);
    assert(s_load_pending && s_load_tick==10 && s_frame==20 && s_count==2);
    assert(rbe_snap_ring_has(s_ring,10) && rbe_snap_ring_has(s_ring,20));
    assert(machine.pc==0x80025074u && machine.gpr[4]==303);
    assert(capture_calls==2 && !load_attempts && !quiet_rollbacks && !observations && !resumes);
}

static int playback_ended, playback_commit, playback_available=1;
static uint32_t playback_applied, playback_target;
static int playback_begin(uint64_t *key,uint64_t *tick) {*key=20;*tick=20;return 1;}
static int playback_step(uint64_t key,uint32_t oldest,uint64_t *shown,uint64_t *tick) {
    (void)key;assert(oldest==10 || oldest==11 || oldest==12);*shown=13;*tick=13;return 1;
}
static int playback_inputs(uint32_t from,uint32_t through) {assert((from==11 || from==13) && through==13);return playback_available;}
static void playback_apply(uint32_t tick) {assert(tick>=11 && tick<=13);playback_applied++;if(tick<13)machine.gpr[4]+=tick;}
static void playback_end(uint64_t key,uint32_t tick,int commit) {
    assert(key==13);playback_ended++;playback_commit=commit;playback_target=tick;
}
static int missing_redraw(uint32_t tick) {(void)tick;return 0;}
static int missing_input(uint32_t from,uint32_t through) {(void)from;(void)through;return 0;}
static void test_redraw_failure_bound(void) {
    PsxRewindPlayback callbacks={0};
    callbacks.presented=missing_redraw;callbacks.inputs_available=missing_input;
    callbacks.end=playback_end;
    psx_rewind_set_playback(&callbacks);
    s_direct=s_preview_ready=1;s_play_tick=s_frame=13;s_play_key=13;
    assert(!psx_rewind_after_present() && !s_direct && !s_seeking && !playback_commit);
    s_direct=s_preview_ready=1;s_play_tick=13;s_frame=17;
    callbacks.inputs_available=NULL; /* bound must short-circuit input access */
    psx_rewind_set_playback(&callbacks);
    assert(!psx_rewind_after_present() && !s_direct && !playback_commit);
    playback_ended=0;psx_rewind_set_playback(NULL);
}
static void test_direct_playback(void) {
    psx_rewind_configure(123,0x801b0000u);capture(10,101);capture(20,202);
    PsxRewindPlayback callbacks={NULL,playback_begin,playback_step,playback_inputs,playback_apply,playback_end};
    psx_rewind_set_playback(&callbacks);
    assert(psx_rewind_toggle() && psx_rewind_direct());
    psx_rewind_playback_tick(100);psx_rewind_playback_tick(200);
    playback_available=0;assert(!psx_rewind_release());
    assert(!psx_rewind_direct() && playback_ended==1 && !playback_commit && machine.gpr[4]==202);
    playback_available=1;assert(psx_rewind_toggle());
    psx_rewind_playback_tick(100);psx_rewind_playback_tick(200);
    assert(psx_rewind_release() && psx_rewind_seeking() && !psx_rewind_is_open());
    int cd_before=cd_restores;
    if(setjmp(resumed)==0){psx_rewind_poll(&machine,machine.pc);assert(0);}
    assert(machine.gpr[4]==101 && s_frame==10 && cd_restores==cd_before);
    psx_rewind_note_frame();assert(psx_rewind_replay_input());
    psx_rewind_note_frame();assert(psx_rewind_replay_input());
    psx_rewind_note_frame();assert(!psx_rewind_replay_input());
    assert(playback_applied==2 && machine.gpr[4]==124 && playback_target==13);
    assert(playback_ended==2 && playback_commit && !psx_rewind_direct());
    assert(s_count==1 && !rbe_snap_ring_has(s_ring,20));
    psx_rewind_shutdown();psx_rewind_set_playback(NULL);
}

#if RW_COMPRESSED_HISTORY
static void test_compressed_state_playback(void) {
    PsxRewindPlayback callbacks={NULL,playback_begin,playback_step,playback_inputs,playback_apply,playback_end,1,NULL};
    psx_rewind_set_playback(&callbacks);s_depth=50;s_interval=15;
    psx_rewind_configure(123,0x801b0000u);
    for(uint32_t tick=10;tick<=100;tick+=10) {
        capture(tick,tick==10?101:tick);
        for(int tries=0;tries<1000 && !rewind_history_contains(s_state_history,tick);tries++)SDL_Delay(1);
        assert(rewind_history_contains(s_state_history,tick));
    }
    assert(!rbe_snap_ring_has(s_ring,10) && s_count==10);
    assert(psx_rewind_toggle());psx_rewind_playback_tick(100);psx_rewind_playback_tick(200);
    assert(s_seeking && !s_open && s_load_tick==10);
    if(setjmp(resumed)==0){psx_rewind_poll(&machine,machine.pc);assert(0);}
    assert(machine.gpr[4]==101 && host_pacing==102);
    psx_rewind_note_frame();assert(psx_rewind_replay_input());
    psx_rewind_note_frame();assert(psx_rewind_replay_input());
    psx_rewind_note_frame();assert(psx_rewind_replay_input());
    assert(!s_seeking && !s_open && psx_rewind_after_present());
    assert(s_open && machine.gpr[4]==124);
    assert(psx_rewind_release() && !s_direct && playback_target==13);
    assert(!rewind_history_contains(s_state_history,20));
    psx_rewind_shutdown();psx_rewind_set_playback(NULL);
}
static void test_command_playback(void) {
    PsxRewindPlayback callbacks={NULL,playback_begin,playback_step,playback_inputs,playback_apply,playback_end,2,NULL,NULL,2};
    psx_rewind_set_playback(&callbacks);s_depth=50;s_interval=15;
    psx_rewind_configure(123,0x801b0000u);
    /* Denser safe anchors retain the original bounded history horizon. */
    assert(s_interval==1 && s_depth==750);
    capture(10,101);capture(12,124);capture(20,202);
    assert(!s_thumbs[0].px && !s_thumbs[1].px);
    assert(psx_rewind_toggle());psx_rewind_playback_tick(100);psx_rewind_playback_tick(200);
    assert(s_open && !s_seeking && machine.gpr[4]==202 && s_frame==20);
    assert(psx_rewind_release() && s_release_pending && s_load_tick==12);
    if(setjmp(resumed)==0){psx_rewind_poll(&machine,machine.pc);assert(0);}
    assert(machine.gpr[4]==124);
    for(int n=0;n<1;n++){psx_rewind_note_frame();assert(psx_rewind_replay_input());}
    assert(!psx_rewind_after_present() && !s_direct && !s_open && !s_release_pending);
    assert(playback_commit && playback_target==13 && machine.gpr[4]==124);
    psx_rewind_shutdown();psx_rewind_set_playback(NULL);
}

#endif

int main(void) {
    test_redraw_failure_bound();
    assert(psx_rewind_register_region(&host_pacing,sizeof host_pacing));
    assert(!psx_rewind_register_region(&host_pacing,sizeof host_pacing));
    s_enabled=1; /* Ignore the runner's optional rewind environment setting. */
    psx_rewind_configure(123,0x801b0000u);
    assert(psx_rewind_enabled());
    capture(10,101); capture(20,202);
    assert(s_hd_bytes==16);
    choose_first(); machine.pc=0x80025074u; machine.gpr[4]=303;
    /* The title is ready, but a registered native callback is still live. */
    function_entry_active=1; assert_deferred(); function_entry_active=0;
    /* Both layers of a nested non-resumable adapter must unwind. */
    psx_snapshot_host_call_begin(); psx_snapshot_host_call_begin(); assert_deferred();
    psx_snapshot_host_call_end(); assert_deferred(); psx_snapshot_host_call_end();
    /* A plausible PC from an unretired interpreter site is not a safe load. */
    g_cosim_dirty_pump_site=5; g_dirty_safe_resume_pc=machine.pc;
    assert_deferred(); g_cosim_dirty_pump_site=0; g_dirty_safe_resume_pc=0;
    /* Title-specific projection readiness remains an independent condition. */
    provider_ready=0; assert_deferred(); provider_ready=1;
    if(setjmp(resumed)==0) {
        psx_rewind_poll(&machine,machine.pc); assert(0);
    }
    assert(!s_load_pending && machine.pc==0x80025070u && machine.gpr[4]==101);
    assert(load_attempts==1 && capture_calls==3 && !quiet_rollbacks && observations==1 && resumes==1);
    assert(frontend_restores==1 && cycle_restores==1 && gpu_present_resets==1 && cd_restores==1);
    assert(s_frame==10 && s_count==1 && !rbe_snap_ring_has(s_ring,20));
    assert(host_pacing==102);
    assert(hd_live==101 && s_hd_bytes==8); /* restore after frontend invalidation */

    /* Checked load changes the machine before it discovers a bad resume PC.
     * Rewind must quietly restore its rollback and retain the selected history. */
    assert(rbe_snap_ring_store(s_ring,10,machine_image(0xfffffffcu,999),8));
    machine.pc=0x80025074u; machine.gpr[4]=303; choose_first();
    psx_rewind_poll(&machine,machine.pc);
    assert(!s_load_pending && machine.pc==0x80025074u && machine.gpr[4]==303);
    assert(load_attempts==2 && quiet_rollbacks==1 && observations==1 && resumes==1);
    assert(s_count==1 && s_frame==10 && rbe_snap_ring_has(s_ring,10));
    assert(hd_live==303 && s_hd_bytes==8); /* rejected target preserves live provenance */
    assert(frontend_restores==1 && cycle_restores==1 && gpu_present_resets==1 && cd_restores==1);
    /* Failure to obtain rollback refuses before the target ever applies. */
    machine.gpr[4]=404; choose_first(); capture_fails=1;
    psx_rewind_poll(&machine,machine.pc);
    assert(machine.pc==0x80025074u && machine.gpr[4]==404);
    assert(load_attempts==2 && quiet_rollbacks==1 && observations==1 && resumes==1);
    assert(s_count==1 && s_frame==10 && rbe_snap_ring_has(s_ring,10));
    capture_fails=0;
    /* Replacement at the same tick, normal depth eviction and branching all
     * retain one owner for each small residency allocation. */
    capture(10,505); assert(s_count==1 && s_hd_bytes==8);
    for(uint32_t tick=30;tick<100;++tick)capture(tick,tick);
    assert(s_count==s_depth && s_hd_bytes==s_depth*8);
    hd_blob_size=RW_HD_BUDGET/2+1;
    capture(1000,606); capture(1010,707);
    assert(s_count==1 && s_ticks[0]==1010 && s_hd_bytes==hd_blob_size);
    assert(!rbe_snap_ring_has(s_ring,1000) && rbe_snap_ring_count(s_ring)==1);
    assert(psx_rewind_discard_history()==1 && s_hd_bytes==0 && s_count==0);
    hd_blob_size=8;
    capture(1020,808);
#if PSX_NATIVE_CAPTURE_JOBS
    s_frame=1030;s_capture_due=1;
    psx_rewind_poll(&machine,machine.pc);
    assert(s_pending.active && queued_complete);
#endif
    psx_rewind_shutdown();
#if PSX_NATIVE_CAPTURE_JOBS
    assert(!s_pending.active && !queued_complete && !s_pending.data);
#endif
    assert(s_hd_bytes==0);
    test_direct_playback();
#if RW_COMPRESSED_HISTORY
    test_compressed_state_playback();
    test_command_playback();
#endif
    puts("rewind loads defer live host stacks and quietly undo rejected resume PCs");
    return 0;
}
