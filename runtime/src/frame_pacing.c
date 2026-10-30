/*
 * frame_pacing.c — race-free wall-clock frame pacing.
 * See frame_pacing.h for the Bug B history this replaces.
 */
#include "frame_pacing.h"

/* FRAME_PACING_PURE_ONLY: tests compile only the SDL-free decision
 * function (tests/test_frame_pacing.c includes this file directly). */
#ifndef FRAME_PACING_PURE_ONLY
#include "psx_sdl.h"
#include "host_time.h"
#include "draw_service.h"
#endif

uint32_t frame_pacing_sleep_ms(uint64_t now, uint64_t deadline,
                               uint64_t freq, uint64_t period) {
    if (now >= deadline) return 0;            /* compare BEFORE subtract */
    uint64_t remaining = deadline - now;       /* cannot underflow */
    if (remaining > period) remaining = period;/* hard cap: one frame max */
    if (freq == 0) return 0;
    /* remaining <= period (~one frame of ticks), so *1000 cannot overflow. */
    uint64_t ms = (remaining * 1000u) / freq;
    if (ms < 2) return 0;                      /* sub-2ms: spin instead */
    return (uint32_t)(ms - 1);                 /* undershoot; spin covers rest */
}

uint32_t frame_pacing_sleep_us(uint64_t now,uint64_t deadline,uint64_t freq,uint64_t period) {
    if(now>=deadline || !freq)return 0;
    uint64_t remaining=deadline-now;
    if(remaining>period)remaining=period;
    uint64_t us=remaining*1000000u/freq;
    return us>200 ? (uint32_t)(us-200) : 0;
}

#ifndef FRAME_PACING_PURE_ONLY

/* Bounded catch-up window, in periods. A transient stall (heavy frame, CD
 * burst) leaves next_deadline in the past; KEEPING that debt and running
 * unpaced until it is repaid preserves the long-term rate at exactly one
 * period per frame — which the audio pipeline depends on (the SPU produces
 * 768 guest cycles per output sample; every re-anchor that forgives debt
 * permanently starves the audio ring by the forgiven amount. Measured on
 * MMX5: forgiving all >1-period debt averaged 59.80 Hz against a 59.94
 * target = -0.4% chronic audio underrun). Only debt beyond this window —
 * sustained sub-realtime emulation, not a hiccup — is forgiven, else the
 * pacer would demand unbounded catch-up. */
/* Vigilante 8's streamed FMV transitions have measured host stalls near
 * 140 ms. Eight 60 Hz periods are only 133.3 ms, so the old bound classified
 * those finite transitions as sustained slowness and permanently forgave the
 * guest/audio debt. Keep a bounded 12-period (200 ms at 60 Hz) window: enough
 * to repay the observed transition without turning a real hang, suspend, or
 * sub-realtime workload into an unbounded catch-up burst. */
#define FRAME_PACER_CATCHUP_MAX_PERIODS 12u

void frame_pacer_wait(FramePacer *p, double period_ms) {
    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t period = (uint64_t)((double)freq * (period_ms / 1000.0));
    uint64_t now = SDL_GetPerformanceCounter();

    if (p->next_deadline == 0 ||
        now >= p->next_deadline + period * FRAME_PACER_CATCHUP_MAX_PERIODS) {
        /* First frame, or sustained slowness beyond the catch-up window:
         * re-anchor (forgive the debt). */
        p->next_deadline = now + period;
        return;
    }
    if (now >= p->next_deadline) {
        /* In debt from a recent stall: run this frame unpaced and advance
         * the deadline, repaying one period of debt per fast frame. */
        p->next_deadline += period;
        return;
    }

    for (;;) {
        draw_service_poll();
        now = SDL_GetPerformanceCounter();     /* ONE read per iteration */
        uint32_t us = frame_pacing_sleep_us(now, p->next_deadline, freq, period);
        if (us == 0) break;
        /* Waitable timer on Win32; usleep on Unix — not coarse Sleep/SDL_Delay. */
        /* Keep completed optional frames serviceable while main is paced.
         * Poll checks thread ownership, so render-thread waits remain inert. */
        const uint32_t sleep_us=draw_service_active() && us > 1000 ? 1000 : us;
#ifndef PSX_NO_DEBUG_TOOLS
        const uint64_t sleep_start=p->trace_waits?SDL_GetPerformanceCounter():0;
#endif
        psx_host_sleep_micros(sleep_us);
#ifndef PSX_NO_DEBUG_TOOLS
        if(p->trace_waits) {
            const uint64_t elapsed=SDL_GetPerformanceCounter()-sleep_start;
            const uint64_t requested=(uint64_t)sleep_us*freq/1000000;
            p->sleep_ticks+=elapsed;
            if(elapsed>requested) p->sleep_overshoot_ticks+=elapsed-requested;
        }
#endif
    }
    while (SDL_GetPerformanceCounter() < p->next_deadline) {
        /* At most a 200-us final scheduling margin. */
    }
    p->next_deadline += period;
}
#endif /* FRAME_PACING_PURE_ONLY */
