/* present_thread.h — opt-in present thread under the render thread
 * ([video] present_thread, docs/RENDER_THREAD.md "Present thread").
 *
 * The graphics owner composes each presented frame into
 * an offscreen slot instead of the window, then hands the slot over; the
 * present thread owns a second graphics context on the same window and does
 * the final copy and the swap. The swap's wait on the window compositor then
 * lands on the present thread, not on rendering.
 *
 * This core is backend-neutral: a ring of `slots` numbered slots, each FREE,
 * COMPOSING (the producer's current slot, exactly one), QUEUED or PRESENTING.
 * Accepted slots are presented in order. With no free slot, submission is
 * refused immediately and the producer retains its composing slot.
 *
 * Fences are opaque: `ready` travels with a submitted slot to present();
 * present() returns a `done` token (e.g. a fence after its copy) that the
 * producer receives when it next takes that slot, before drawing into it.
 *
 * Producer calls (pt_current / pt_submit) may come from different threads
 * over time only across renderer shutdown/startup, never concurrently. */
#ifndef PSX_PRESENT_THREAD_H
#define PSX_PRESENT_THREAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PT_MAX_SLOTS 8

/* Conservative color+depth attachment budget. Returns 0 if two slots do not fit. */
int pt_budget_slots(int requested, uint32_t width, uint32_t height,
                    uint32_t bytes_per_pixel, uint64_t budget_bytes);

/* Present thread: make the present context current (current != 0, return 0 on
 * failure: pt_start then fails) or release it (current == 0). */
typedef int   (*PtCtxFn)(void *user, int current);
/* Present thread: show `slot` (wait for `ready`, copy, swap). Returns the
 * slot's done token (may be NULL). */
typedef void *(*PtPresentFn)(void *user, int slot, void *ready);
/* Present thread, after the last present at stop or for a token nobody will
 * collect: dispose of a done token. Optional. */
typedef void  (*PtDisposeFn)(void *user, void *token);

typedef struct PtConfig {
    int         slots;     /* 2..PT_MAX_SLOTS */
    PtCtxFn     ctx;
    PtPresentFn present;
    PtDisposeFn dispose;   /* optional */
    void       *user;
} PtConfig;

/* Start the thread; returns once its ctx(1) has run. 1 on success (the
 * producer's current slot is 0), 0 on failure (nothing running). */
int  pt_start(const PtConfig *cfg);
/* Present everything queued, release the context on the present thread,
 * join. Done tokens still held are passed to dispose on the present thread
 * before it releases its context. Safe when not running. */
void pt_stop(void);
int  pt_running(void);

/* Producer: the slot being composed. */
int  pt_current(void);
/* Producer: queue only when another slot is free. On refusal (-2), retain the
 * composing slot and ready token. On success next_done receives the next
 * slot's done token. wait_ns is always zero; -1 means not running. */
int  pt_submit(void *ready, void **next_done, uint64_t *wait_ns);
/* Independent output scheduling. Times are monotonic nanoseconds from
 * pt_clock_ns(). Set a new nonzero epoch after a rate/session/clock reset.
 * Invalidating an epoch wakes the presenter and retires queued timed outputs;
 * untimed submissions retain their existing FIFO behavior. */
typedef struct PtTiming {
    uint64_t epoch, not_before_ns, expires_ns;
    uint64_t generation, index; /* optional diagnostic provenance */
    uint32_t alpha_q16;
} PtTiming;
uint64_t pt_clock_ns(void);
void pt_set_epoch(uint64_t epoch);
/* Producer: submit only if another slot is free, the epoch matches,
 * and the output has not expired. Never waits for a deadline.
 * Returns new slot, -1 when stopped, -2 on refusal. On refusal the producer
 * keeps its current slot AND ready token; next_done is NULL. Accepted timed
 * outputs wait on the present thread, and drop if expired/reset before use.
 * A dropped ready token returns as done so GPU work is still fenced on reuse.
 * Stop discards timed pending outputs rather than waiting on future times. */
int pt_try_submit_at(void *ready, const PtTiming *timing, void **next_done);
/* Callback: read provenance only while this slot is being presented. */
int pt_present_timing(unsigned slot, PtTiming *out);
/* Producer: wait until nothing is queued or presenting. */
void pt_drain(void);

typedef struct PtStats {
    uint64_t refused;
    uint64_t submits, presents, waits, wait_ns, present_ns, present_max_ns;
    uint64_t present_gap_max_ns, present_gaps_over_25ms;
    uint64_t timed_dropped, timed_refused;
    int      slots, queued_high;
    /* Retained diagnostic fields: admission never waits; always zero. */
    uint64_t timed_waited, timed_wait_ns;
} PtStats;
void pt_get_stats(PtStats *out);

#ifdef __cplusplus
}
#endif
#endif /* PSX_PRESENT_THREAD_H */
