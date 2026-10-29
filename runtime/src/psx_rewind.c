/* Per-frame native delta history and hold-to-rewind. No GL reads. */
#include "psx_rewind.h"
#include "rewind_delta.h"
#include "boot_state.h"
#include "interrupts.h"
#include "psx_cycles.h"
#include "psx_netplay.h"
#include "psx_netplay_rb.h"
#include "psx_scheduler.h"
#include "savestate.h"
#include "present_thread.h"
#include "host_time.h"
#include <stdlib.h>
#include <string.h>

int g_psx_rewind_pending;
static RwHistory history;
static BootStatePending *pending;
static int capturing;
static uint32_t capture_tick;
static double capture_cost;
static uint8_t *staging;
static size_t staging_cap;
static uint32_t bios, entry, frame, depth=600;
static int enabled, configured, blocked, held, debug_held, active, due;
static PsxRewindStats stats;
static int verify;
void psx_rewind_debug_verify(int on) { verify=on!=0; }
/* Host services input/debug and publishes a restored native frame. */
extern void psx_frontend_rewind_service(int present);
extern void psx_frontend_rewind_audio(int paused);
void psx_rewind_set_enabled(int on) {
    const char *e=getenv("PSX_REWIND");
    enabled=e&&*e?atoi(e)!=0:on!=0;
    if(!enabled)g_psx_rewind_pending=0;
}
void psx_rewind_set_depth(uint32_t n) {
    /* Legacy settings expressed sparse snapshot counts; retain their duration
     * at the new every-frame cadence (50 -> 750 frames). */
    depth=n?(n>240u?3600u:n*15u):600u;
    if(depth>3600) depth=3600;
}
void psx_rewind_set_interval(uint32_t n) { (void)n; } /* always per frame */
void psx_rewind_shutdown(void) {
    boot_state_pending_destroy(pending);pending=NULL;capturing=0;
    rw_clear(&history); free(staging); staging=NULL; staging_cap=0;
    configured=held=debug_held=active=due=g_psx_rewind_pending=0;
}
void psx_rewind_configure(uint32_t b, uint32_t e) {
    psx_rewind_shutdown(); bios=b; entry=e; frame=0;
    memset(&stats,0,sizeof(stats)); verify=0;
    if(enabled)pending=boot_state_pending_create();
    configured=enabled&&pending!=NULL;
}
uint32_t psx_rewind_discard_history(void) {
    uint32_t n=history.count;
    if(capturing){boot_state_pending_destroy(pending);pending=boot_state_pending_create();capturing=0;if(!pending)configured=0;}
    rw_clear(&history);due=0;g_psx_rewind_pending=0;return n;
}
int psx_rewind_enabled(void) { return enabled&&configured; }
int psx_rewind_title_blocked(void) { return blocked; }
void psx_rewind_set_title_blocked(int b) {
    blocked=b!=0;
    if(blocked) { held=debug_held=0; due=0;g_psx_rewind_pending=capturing; }
}
uint32_t psx_rewind_snap_count(void) { return history.size?history.count+1:0; }
int psx_rewind_is_open(void) { return active; }
int psx_rewind_needs_present(void) { return active; }
void psx_rewind_set_held(int down) { held=down!=0; g_psx_rewind_pending=enabled&&!blocked&&(due||held||debug_held||capturing); }
void psx_rewind_debug_hold(int down) { debug_held=down!=0; g_psx_rewind_pending=enabled&&!blocked&&(due||held||debug_held||capturing); }
static int wants_rewind(void) {
    return (held||debug_held)&&!blocked&&!psx_netplay_active();
}
void psx_rewind_note_frame(void) {
    if(psx_rewind_enabled()&&!active&&!blocked&&!psx_netplay_active()) {
        frame++; due=1; g_psx_rewind_pending=1;
    }
}
void psx_rewind_get_stats(PsxRewindStats *out) {
    *out=stats; out->frame=history.tick; out->bytes=history.bytes+history.size;
}
static int resume_pc_ok(uint32_t pc)
{
    return pc != 0u && (pc & 3u) == 0u && psx_is_dispatchable(pc) &&
           pc != 0x80000080u && pc != 0xbfc00180u && pc != 0x80000000u;
}

/* Mid-FMV / present-edge IRQ paths often pass hint=0 — same latch walk as
 * savestate_resolve_resume_pc / selfcheck sc_pick_snap_pc. */
static uint32_t rewind_resolve_resume_pc(const CPUState *cpu, uint32_t hint)
{
    uint32_t cands[7];
    int n = 0;
    int i;

    if (psx_irq_resume_context_snapshot_site() != 0)
        cands[n++] = psx_irq_resume_context_snapshot_pc();
    cands[n++] = hint;
    cands[n++] = cpu ? cpu->pc : 0u;
    cands[n++] = psx_compiled_irq_resume_pc();
    cands[n++] = psx_last_irq_check_pc();
    cands[n++] = psx_netplay_rb_sticky_bb_pc();
    cands[n++] = cpu ? cpu->gpr[31] : 0u;

    for (i = 0; i < n; ++i) {
        if (resume_pc_ok(cands[i]))
            return cands[i];
    }
    return 0u;
}


void psx_rewind_poll(CPUState *cpu, uint32_t resume_pc) {
    if(!psx_rewind_enabled()||!cpu||active) return;
    if(capturing) {
        uint64_t start=pt_clock_ns();
        int result=boot_state_pending_finish(pending,rw_patch,&history);
        if(!result) {capture_cost+=(double)(pt_clock_ns()-start)/1e6;return;}
        if(result>0 && rw_finish(&history,capture_tick)) {
            double finish_ms=(double)(pt_clock_ns()-start)/1e6;
            double ms=capture_cost+finish_ms;
            stats.captures++;stats.capture_ms+=ms;stats.delta_ms+=finish_ms;
            if(ms>stats.max_ms)stats.max_ms=ms;
        } else {rw_abort(&history);stats.failures++;}
        capturing=0;
    }
    g_psx_rewind_pending=due||held||debug_held;
    if(!due&&!held&&!debug_held)return;
    uint32_t pc=rewind_resolve_resume_pc(cpu,resume_pc);
    if(!resume_pc_ok(pc)||!psx_irq_resume_context_snapshot_safe_at(pc))return;
    if(due&&!blocked&&!psx_netplay_active()) {
        uint64_t start=pt_clock_ns();
        CPUState snap=*cpu;snap.pc=pc;
        int saved=rw_begin(&history,depth);
        if(saved) {
            saved=boot_state_begin_native(&snap,bios,entry,rw_write,&history,pending);
            if(!saved)rw_abort(&history);
        }
        capture_cost=(double)(pt_clock_ns()-start)/1e6;
        if(saved) {
            stats.serialize_ms+=capture_cost;
            capture_tick=frame;capturing=1;due=0;g_psx_rewind_pending=1;
        } else stats.failures++;
        return;
    }
    if(!wants_rewind()||!psx_hle_scheduler_enabled()||!history.size) return;
    active=1;
    psx_frontend_rewind_audio(1);
    uint64_t next=pt_clock_ns();
    int restored=0;
    while(wants_rewind()) {
        psx_frontend_rewind_service(0);
        if(!wants_rewind()) break;
        uint64_t now=pt_clock_ns();
        if(now>=next) {
            if(rw_back(&history)) {
                if(!boot_state_load_buffer(history.image,history.size,bios,entry,cpu)) {
                    stats.failures++; held=debug_held=0; break;
                }
                psx_cycles_resync_after_restore(cpu);
                interrupts_resync_after_restore();
                if(verify) {
                    size_t n=0;
                    if(boot_state_save_buffer_reuse(cpu,bios,entry,&staging,&staging_cap,&n)
                       && n==history.size && !memcmp(staging,history.image,n)) stats.verified++;
                    else {
                        stats.verify_failures++;
                        size_t i=0; while(i<n && i<history.size && staging[i]==history.image[i]) i++;
                        stats.first_difference=(uint32_t)i;
                    }
                }
                frame=history.tick; due=0; restored=1; stats.restores++;
                psx_frontend_rewind_service(1);
            }
            /* Schedule from step start; a slow restore never triggers a burst. */
            next=now+16666667u;
        }
        psx_host_sleep_ms(1);
    }
    active=0; g_psx_rewind_pending=0;
    psx_frontend_rewind_audio(0);
    if(restored) {
        psx_frontend_on_savestate_loaded();
        psx_scheduler_resume_at(cpu->pc);
    }
}
/* ABI compatibility for overlay providers. The default input uses held state;
 * there is no selection menu, thumbnails, or accept-to-load step. */
int psx_rewind_toggle(void) { psx_rewind_set_held(!held);return 1; }
int psx_rewind_cancel(void) { held=debug_held=0;g_psx_rewind_pending=due||capturing;return 1; }
int psx_rewind_accept(void) { return psx_rewind_cancel(); }
void psx_rewind_move(int delta) { (void)delta; }
void psx_rewind_nav_held(int l,int r,int a,int c,uint32_t ms) {
    (void)l;(void)r;(void)a;(void)ms; if(c) psx_rewind_cancel();
}
void psx_rewind_present_tick(uint32_t ms) { (void)ms; }
float psx_rewind_slide(void) { return 0; }
int psx_rewind_overlay_image(const uint32_t **p,int *w,int *h) {
    if(p)*p=NULL; if(w)*w=0; if(h)*h=0; return 0;
}
