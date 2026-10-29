/* Per-frame native delta history and hold-to-rewind. No GL reads. */
#include "psx_rewind.h"
#include "rewind_delta.h"
#include "rewind_worker.h"
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
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
/* Scheduler diagnostic, not a duration conversion: Windows documents these
 * as processor-dependent cycle units. Wall-clock maxima remain authoritative. */
static uint64_t capture_thread_cycles(void) {
#ifdef _WIN32
    ULONG64 cycles=0;QueryThreadCycleTime(GetCurrentThread(),&cycles);return cycles;
#else
    return 0;
#endif
}

/* Capture has a short frame deadline, like the existing presentation worker.
 * Avoid ordinary background/worker wakeups preempting its bounded RAM copy.
 * Never lower an existing higher priority; restore the caller's setting when
 * rewind is disabled/reconfigured. This is not real-time scheduling. */
#ifdef _WIN32
static int capture_priority_changed,capture_previous_priority;
#endif
static void capture_priority(int enabled) {
#ifdef _WIN32
    if(enabled) {
        int prior=GetThreadPriority(GetCurrentThread());
        if(prior<=THREAD_PRIORITY_NORMAL &&
           SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL)) {
            capture_previous_priority=prior;capture_priority_changed=1;
        }
    } else if(capture_priority_changed) {
        SetThreadPriority(GetCurrentThread(),capture_previous_priority);
        capture_priority_changed=0;
    }
#else
    (void)enabled;
#endif
}

int g_psx_rewind_pending;
static RwHistory history;
static BootStatePending *pending;
static RewindWorker *worker;
static uint8_t *capture_image;
static size_t capture_size,capture_capacity;
static struct { size_t offset,size;const void *data; } patches[2];
static unsigned patch_count;
static int worker_ok;
static double worker_cost;
/* Only the emulation thread publishes these after the worker's acquire. */
static uint32_t published_tick,published_count;
static uint64_t published_bytes;
static int capturing;
static unsigned poll_wait;
static uint32_t capture_tick;
static double capture_cost;
static uint8_t *staging;
static size_t staging_cap;
static uint32_t bios, entry, frame, depth=600;
static int enabled, configured, blocked, held, debug_held, active, due;
static PsxRewindStats stats;
static int verify;
/* Fixed 1-us bins; the exact maximum includes host preemption. */
static uint64_t capture_hist[10001];
void psx_rewind_reset_stats(void) {
    memset(&stats,0,sizeof(stats));memset(capture_hist,0,sizeof(capture_hist));
}
static double percentile(unsigned pct) {
    uint64_t target=(stats.captures*pct+99)/100, sum=0;
    if(!target)return 0;
    for(unsigned i=0;i<10001;i++){sum+=capture_hist[i];if(sum>=target)return (i+1)*.001;}
    return 10.001;
}
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
static int defer_patch(void *unused,size_t offset,const void *data,size_t size) {
    (void)unused;
    if(patch_count==2)return 0;
    patches[patch_count].offset=offset;patches[patch_count].data=data;
    patches[patch_count++].size=size;return 1;
}
/* The guest thread only copies immutable native bytes. Comparisons, reverse
 * page construction, pool copies and eviction all belong to the CPU worker. */
static int capture_write(void *unused,const void *data,size_t size) {
    (void)unused;
    if(size>capture_capacity-capture_size)return 0;
    if(data)memcpy(capture_image+capture_size,data,size);
    capture_size+=size;return 1;
}
static void finish_delta(void *unused) {
    (void)unused;uint64_t start=pt_clock_ns();worker_ok=1;
    for(unsigned i=0;i<patch_count && worker_ok;i++) {
        if(patches[i].offset>capture_size || patches[i].size>capture_size-patches[i].offset)
            worker_ok=0;
        else memcpy(capture_image+patches[i].offset,patches[i].data,patches[i].size);
    }
    if(worker_ok)worker_ok=rw_push(&history,capture_image,capture_size,capture_tick,history.capacity);
    worker_cost=(double)(pt_clock_ns()-start)/1e6;
}
static void publish_history(void) {
    published_tick=history.tick;published_count=history.size?history.count+1:0;
    published_bytes=history.bytes+history.size;
}
void psx_rewind_shutdown(void) {
    capture_priority(0);
    rewind_worker_destroy(worker);worker=NULL;
    boot_state_pending_destroy(pending);pending=NULL;capturing=0;
    rw_clear(&history);free(capture_image);capture_image=NULL;capture_size=capture_capacity=0; free(staging); staging=NULL; staging_cap=0;
    configured=held=debug_held=active=due=g_psx_rewind_pending=0;
    published_tick=published_count=0;published_bytes=0;
}
void psx_rewind_configure(uint32_t b, uint32_t e) {
    psx_rewind_shutdown(); bios=b; entry=e; frame=0;
    psx_rewind_reset_stats(); verify=0;
    if(enabled)pending=boot_state_pending_create();
    configured=enabled&&pending!=NULL&&rw_prepare(&history,depth,32u*1024u*1024u,256u*1024u*1024u);
    if(configured) {
        capture_capacity=history.image_capacity;
        capture_image=(uint8_t*)calloc(1,capture_capacity);
        if(capture_image) {
            for(size_t i=0;i<capture_capacity;i+=RW_PAGE)((volatile uint8_t*)capture_image)[i]=0;
            worker=rewind_worker_create(finish_delta,NULL);
        }
        configured=worker!=NULL;
    }
    if(configured)capture_priority(1);
}
uint32_t psx_rewind_discard_history(void) {
    rewind_worker_wait(worker);
    uint32_t n=history.count;
    if(capturing){boot_state_pending_destroy(pending);pending=boot_state_pending_create();capturing=0;if(!pending)configured=0;}
    rw_reset(&history);publish_history();due=0;g_psx_rewind_pending=0;return n;
}
int psx_rewind_enabled(void) { return enabled&&configured; }
int psx_rewind_title_blocked(void) { return blocked; }
void psx_rewind_set_title_blocked(int b) {
    blocked=b!=0;
    if(blocked) { held=debug_held=0; due=0;g_psx_rewind_pending=capturing; }
}
uint32_t psx_rewind_snap_count(void) { return published_count; }
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
    *out=stats;out->p50_ms=percentile(50);out->p99_ms=percentile(99); out->frame=published_tick; out->bytes=published_bytes;
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
        /* Guest BB polls can run millions of times a second. A pending CPU
         * checkpoint needs no timed readiness probe at every block. Frame
         * and input requests bypass the bounded coalescing interval. */
        if(poll_wait && !due && !held && !debug_held){poll_wait--;return;}
        uint64_t start=pt_clock_ns();
        if(capturing==1) {
            patch_count=0;
            int result=boot_state_pending_finish(pending,defer_patch,NULL);
            if(!result) {
                double ms=(double)(pt_clock_ns()-start)/1e6;
                capture_cost+=ms;if(ms>stats.poll_max_ms)stats.poll_max_ms=ms;
                poll_wait=63;return;
            }
            if(result<0 || !rewind_worker_submit(worker)) {
                stats.failures++;capturing=0;return;
            }
            capturing=2;
        }
        if(!rewind_worker_ready(worker)) {
            capture_cost+=(double)(pt_clock_ns()-start)/1e6;poll_wait=63;return;
        }
        publish_history();
        if(worker_ok) {
            double finish_ms=(double)(pt_clock_ns()-start)/1e6;
            double ms=capture_cost+finish_ms;
            if(finish_ms>stats.finish_max_ms)stats.finish_max_ms=finish_ms;
            unsigned bin=ms>=10.0?10000:(unsigned)(ms*1000);capture_hist[bin]++;
            stats.captures++;stats.capture_ms+=ms;stats.delta_ms+=finish_ms;
            stats.worker_ms+=worker_cost;
            if(worker_cost>stats.worker_max_ms)stats.worker_max_ms=worker_cost;
            if(ms>stats.max_ms)stats.max_ms=ms;
        } else stats.failures++;
        capturing=0;
    }
    g_psx_rewind_pending=due||held||debug_held;
    if(!due&&!held&&!debug_held)return;
    uint32_t pc=rewind_resolve_resume_pc(cpu,resume_pc);
    if(!resume_pc_ok(pc)||!psx_irq_resume_context_snapshot_safe_at(pc))return;
    if(due&&!blocked&&!psx_netplay_active()) {
        uint64_t start=pt_clock_ns();
        double prior_profile[6];boot_state_native_capture_profile(prior_profile);
        uint64_t thread_start=capture_thread_cycles();
        CPUState snap=*cpu;snap.pc=pc;
        capture_size=0;
        int saved=boot_state_begin_native(&snap,bios,entry,capture_write,NULL,pending);
        uint64_t thread_cycles=capture_thread_cycles()-thread_start;
        capture_cost=(double)(pt_clock_ns()-start)/1e6;
        stats.begin_thread_cycles+=thread_cycles;
        if(thread_cycles>stats.max_begin_thread_cycles)stats.max_begin_thread_cycles=thread_cycles;
        if(capture_cost>stats.begin_max_ms) {
            stats.begin_max_ms=capture_cost;
            stats.worst_begin_thread_cycles=thread_cycles;
            boot_state_native_capture_profile(stats.worst_begin_profile_ms);
            for(unsigned i=0;i<6;i++)stats.worst_begin_profile_ms[i]-=prior_profile[i];
        }
        if(saved) {
            stats.serialize_ms+=capture_cost;
            capture_tick=frame;capturing=1;poll_wait=0;due=0;g_psx_rewind_pending=1;
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
                publish_history();frame=history.tick; due=0; restored=1; stats.restores++;
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
