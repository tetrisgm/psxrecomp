/* present_thread_test — the present thread core (present_thread.c) against a
 * fake presenter: submission order is presentation order, a slot is never
 * composed while queued or presenting, each done token reaches the producer
 * that next takes its slot, a slow display blocks the producer (and a fast one
 * still keeps every invariant), stop presents everything queued and disposes leftover tokens, a
 * failed context start reports failure. */
#include "present_thread.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int checks, failures;
static void check(int ok, const char *label) {
    checks++;
    if (!ok) { fprintf(stderr, "FAIL %s\n", label); failures++; }
}
static void sleep_us(long us) {
    struct timespec ts = { us / 1000000, (us % 1000000) * 1000 };
    nanosleep(&ts, NULL);
}

#define N_FRAMES 600
static _Atomic int slot_busy[PT_MAX_SLOTS];   /* 1 while queued or presenting */
static uintptr_t presented[N_FRAMES + 8];
static _Atomic int n_presented;
static int ctx_ok = 1, ctx_on, ctx_off;
static long present_us = 0;
static _Atomic int disposed;
static uintptr_t last_done[PT_MAX_SLOTS];
static _Atomic int order_bad;

static int fake_ctx(void *u, int cur) { (void)u; if (cur) { ctx_on++; return ctx_ok; } ctx_off++; return 1; }
static void *fake_present(void *u, int slot, void *ready) {
    (void)u;
    if (!atomic_load(&slot_busy[slot])) atomic_store(&order_bad, 1);
    int i = atomic_fetch_add(&n_presented, 1);
    if (i < N_FRAMES + 8) presented[i] = (uintptr_t)ready;
    if (present_us) sleep_us(present_us + (rand() % 3) * present_us / 2);
    atomic_store(&slot_busy[slot], 0);
    /* done token: slot in the low byte, frame above */
    return (void *)(((uintptr_t)ready << 8) | (uintptr_t)slot | 0x80u);
}
static void fake_dispose(void *u, void *t) { (void)u; (void)t; atomic_fetch_add(&disposed, 1); }

static PtConfig cfg(int slots) {
    PtConfig c = { slots, fake_ctx, fake_present, fake_dispose, NULL };
    return c;
}
static void reset(void) {
    for (int i = 0; i < PT_MAX_SLOTS; i++) { atomic_store(&slot_busy[i], 0); last_done[i] = 0; }
    atomic_store(&n_presented, 0);
    atomic_store(&disposed, 0);
    atomic_store(&order_bad, 0);
    ctx_on = ctx_off = 0;
}

static void run(int slots, long us, int expect_waits) {
    reset();
    present_us = us;
    PtConfig c = cfg(slots);
    check(pt_start(&c), "start");
    check(pt_current() == 0, "first slot is 0");
    int bad_reuse = 0, bad_token = 0;
    for (int f = 1; f <= N_FRAMES; f++) {
        const int cur = pt_current();
        if (atomic_load(&slot_busy[cur])) bad_reuse++;
        /* producer "composes" for a bit */
        if (f % 7 == 0) sleep_us(200);
        atomic_store(&slot_busy[cur], 1);
        void *done = NULL;
        uint64_t w = 0;
        const int next = pt_submit((void *)(uintptr_t)f, &done, &w);
        if (next < 0 || next >= slots || next == cur || atomic_load(&slot_busy[next])) bad_reuse++;
        if ((uintptr_t)done != last_done[next]) bad_token++;
        /* this slot's next token is unknown until presented again */
        last_done[next] = 0;
        /* remember what present will hand back for cur */
        last_done[cur] = (((uintptr_t)f) << 8) | (uintptr_t)cur | 0x80u;
    }
    pt_drain();
    check(atomic_load(&n_presented) == N_FRAMES, "every submitted slot presented");
    int ordered = 1;
    for (int i = 0; i < N_FRAMES; i++) if (presented[i] != (uintptr_t)(i + 1)) ordered = 0;
    check(ordered, "presented in submission order");
    check(!bad_reuse, "a slot is never composed while queued or presenting");
    check(!bad_token, "done token reaches the producer taking the slot");
    check(!atomic_load(&order_bad), "present sees only queued slots");
    PtStats st;
    pt_get_stats(&st);
    check(st.submits == N_FRAMES && st.presents == N_FRAMES, "stats count");
    check(st.queued_high <= slots, "bounded queue");
    if (expect_waits > 0) check(st.waits > 0 && st.wait_ns > 0, "slow display blocks the producer");
    /* stop with frames queued: they are presented, leftover tokens disposed */
    present_us = 2000;
    for (int f = 0; f < slots - 1; f++) {
        void *d = NULL;
        atomic_store(&slot_busy[pt_current()], 1);
        (void)pt_submit((void *)(uintptr_t)(N_FRAMES + 1 + f), &d, NULL);
    }
    pt_stop();
    check(atomic_load(&n_presented) == N_FRAMES + slots - 1, "stop presents the queue");
    check(atomic_load(&disposed) > 0, "stop disposes uncollected tokens");
    check(ctx_on == 1 && ctx_off == 1, "context taken and released once");
    check(!pt_running() && pt_submit(NULL, NULL, NULL) == -1, "stopped");
}

static uint64_t timed_presented_at;
static long timed_pause_us;
static void *timed_present(void *u, int slot, void *ready) {
    (void)u; (void)slot;
    timed_presented_at = pt_clock_ns();
    int i = atomic_fetch_add(&n_presented, 1);
    if (i < N_FRAMES + 8) presented[i] = (uintptr_t)ready;
    if (timed_pause_us) sleep_us(timed_pause_us);
    return ready;
}

static void run_timed(void) {
    reset();
    PtConfig c = {3, fake_ctx, timed_present, fake_dispose, NULL};
    check(pt_start(&c), "timed presenter starts");
    pt_set_epoch(1);
    uint64_t begin = pt_clock_ns();
    PtTiming t = {1, begin + 20000000u, begin + 1000000000u};
    void *done = NULL;
    check(pt_try_submit_at((void *)1, &t, &done) >= 0,
          "future output queues without waiting for deadline");
    pt_drain();
    check(atomic_load(&n_presented) == 1 && timed_presented_at >= t.not_before_ns,
          "present thread observes not-before deadline");
    PtStats st;
    pt_get_stats(&st);
    check(st.waits == 0, "timed producer never blocks");
    pt_stop();

    reset();
    check(pt_start(&c), "timestamp-order presenter starts");
    pt_set_epoch(5);
    begin=pt_clock_ns();
    t=(PtTiming){5,begin+40000000u,begin+1000000000u};
    check(pt_try_submit_at((void *)32,&t,&done)>=0,"future base queued first");
    t.not_before_ns=begin+20000000u;
    check(pt_try_submit_at((void *)31,&t,&done)>=0,"earlier worker arrives after future base");
    pt_drain();
    check(atomic_load(&n_presented)==2 && presented[0]==31 && presented[1]==32,
          "presentation follows timestamps rather than worker completion order");
    pt_get_stats(&st);
    check(!st.waits && !st.timed_dropped,"reordering neither waits producer nor expires earlier image");
    pt_stop();

    reset();
    c.slots = 2;
    check(pt_start(&c), "bounded timed presenter starts");
    pt_set_epoch(2);
    begin = pt_clock_ns();
    t = (PtTiming){2, begin + 5000000000ull, begin + 6000000000ull};
    check(pt_try_submit_at((void *)11, &t, &done) == 1,
          "timed queue keeps a composing slot");
    check(pt_try_submit_at((void *)12, &t, &done) == -2 &&
          pt_current() == 1 && done == NULL,
          "full queue refuses without consuming slot or fence");
    pt_set_epoch(3);
    pt_drain();
    check(atomic_load(&n_presented) == 0, "epoch reset wakes and drops pending output");
    check(pt_clock_ns() - begin < 1000000000u, "reset does not wait for old deadline");
    check(pt_try_submit_at((void *)13, &t, &done) == -2,
          "old epoch submission rejected");
    t.epoch = 3;
    check(pt_try_submit_at((void *)14, &t, &done) == 0 && done == (void *)11,
          "dropped ready fence returned for safe slot reuse");
    begin = pt_clock_ns();
    pt_stop();
    check(pt_clock_ns() - begin < 1000000000u,
          "shutdown cancels future timed output promptly");
    check(atomic_load(&n_presented) == 0, "shutdown never presents cancelled timed output");
    pt_get_stats(&st);
    check(st.timed_dropped == 2 && st.timed_refused == 2 && st.waits == 0,
          "timed drops/refusals counted separately from presents and waits");

    reset();
    c.slots = 3;
    check(pt_start(&c), "late-output presenter starts");
    pt_set_epoch(4);
    timed_pause_us = 200000;
    begin = pt_clock_ns();
    t = (PtTiming){4, begin, begin + 1000000000u};
    check(pt_try_submit_at((void *)21, &t, &done) >= 0, "first slow output accepted");
    t.expires_ns = begin + 100000000u;
    check(pt_try_submit_at((void *)22, &t, &done) >= 0, "next output queued before expiry");
    pt_drain();
    check(atomic_load(&n_presented) == 1 && presented[0] == 21,
          "output expired behind slow swap drops without presenting out of order");
    pt_get_stats(&st);
    check(st.timed_dropped == 1 && st.waits == 0, "consumer rechecks expiry");
    timed_pause_us = 0;
    t.not_before_ns = 1; t.expires_ns = 2;
    check(pt_try_submit_at((void *)23, &t, &done) == -2,
          "already expired output refused at admission");
    pt_stop();
}

int main(void) {
    srand(7);
    run(3, 0, 0);
    run(3, 1500, 1);
    run(2, 800, 1);
    run(4, 300, -1);   /* may or may not wait: invariants only */
    run(6, 100, -1);
    run(8, 100, -1);
    check(pt_budget_slots(8,7680,4320,8,2048ull<<20)==8, "8K eight slots fit 2 GiB");
    check(pt_budget_slots(8,7680,4320,8,1024ull<<20)==4, "8K clamps to four at 1 GiB");
    check(pt_budget_slots(8,7680,4320,8,128ull<<20)==0, "insufficient budget refuses");
    check(pt_budget_slots(8,UINT32_MAX,UINT32_MAX,8,UINT64_MAX)==0, "overflow refused");
    run_timed();
    /* context failure */
    reset();
    ctx_ok = 0;
    PtConfig c = cfg(3);
    check(!pt_start(&c), "start fails when the context cannot be made current");
    check(!pt_running(), "not running after failed start");
    ctx_ok = 1;
    PtConfig bad = cfg(1);
    check(!pt_start(&bad), "one slot refused");
    bad = cfg(PT_MAX_SLOTS + 1);
    check(!pt_start(&bad), "above maximum refused");
    printf("present_thread_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
