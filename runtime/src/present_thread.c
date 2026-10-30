/* present_thread.c — present thread core. See present_thread.h and
 * docs/RENDER_THREAD.md ("Present thread").
 *
 * Presents run at display rate (tens to a few hundred per second), so one
 * mutex guards all slot state; nothing is held while present() runs. */

#include "present_thread.h"
#include "host_thread_name.h"

#include <string.h>

int pt_budget_slots(int requested, uint32_t width, uint32_t height,
                    uint32_t bytes_per_pixel, uint64_t budget_bytes) {
    if (!width || !height || !bytes_per_pixel) return 0;
    uint64_t pixels = (uint64_t)width * height;
    if (pixels > UINT64_MAX / bytes_per_pixel) return 0;
    uint64_t fit = budget_bytes / (pixels * bytes_per_pixel);
    if (requested > PT_MAX_SLOTS) requested = PT_MAX_SLOTS;
    if (requested < 2 || fit < 2) return 0;
    return fit < (uint64_t)requested ? (int)fit : requested;
}

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
typedef SRWLOCK            pt_mutex;
typedef CONDITION_VARIABLE pt_cond;
static void mtx_init(pt_mutex *m) { InitializeSRWLock(m); }
static void mtx_lock(pt_mutex *m) { AcquireSRWLockExclusive(m); }
static void mtx_unlock(pt_mutex *m) { ReleaseSRWLockExclusive(m); }
static void cnd_init(pt_cond *c) { InitializeConditionVariable(c); }
static void cnd_wait(pt_cond *c, pt_mutex *m) { SleepConditionVariableSRW(c, m, INFINITE, 0); }
static void cnd_wait_ns(pt_cond *c, pt_mutex *m, uint64_t ns) {
    DWORD ms = ns / 1000000u >= 1000u ? 1000u : (DWORD)((ns + 999999u) / 1000000u);
    SleepConditionVariableSRW(c, m, ms, 0);
}
static void cnd_bcast(pt_cond *c) { WakeAllConditionVariable(c); }
static uint64_t now_ns(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)f.QuadPart);
}
typedef HANDLE pt_thread;
#else
#  include <pthread.h>
#  if defined(__APPLE__)
#    include <pthread/qos.h>
#  endif
#  include <time.h>
typedef pthread_mutex_t pt_mutex;
typedef pthread_cond_t  pt_cond;
static void mtx_init(pt_mutex *m) { pthread_mutex_init(m, NULL); }
static void mtx_lock(pt_mutex *m) { pthread_mutex_lock(m); }
static void mtx_unlock(pt_mutex *m) { pthread_mutex_unlock(m); }
static void cnd_init(pt_cond *c) { pthread_cond_init(c, NULL); }
static void cnd_wait(pt_cond *c, pt_mutex *m) { pthread_cond_wait(c, m); }
static void cnd_wait_ns(pt_cond *c, pt_mutex *m, uint64_t ns) {
    struct timespec ts;
    if (ns > 1000000000u) ns = 1000000000u;
    clock_gettime(CLOCK_REALTIME, &ts);
    ns += (uint64_t)ts.tv_nsec;
    ts.tv_sec += (time_t)(ns / 1000000000u);
    ts.tv_nsec = (long)(ns % 1000000000u);
    pthread_cond_timedwait(c, m, &ts);
}
static void cnd_bcast(pt_cond *c) { pthread_cond_broadcast(c); }
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
typedef pthread_t pt_thread;
#endif

enum { S_FREE = 0, S_COMPOSING, S_QUEUED, S_PRESENTING };

static struct {
    PtConfig cfg;
    pt_mutex mtx;
    pt_cond  cv;              /* any state change; both sides wait on it */
    pt_thread thread;
    int      running, stop, init_done, init_ok;
    int      state[PT_MAX_SLOTS];
    void    *ready[PT_MAX_SLOTS];
    void    *done[PT_MAX_SLOTS];
    PtTiming timing[PT_MAX_SLOTS];
    uint64_t epoch;
    int      queue[PT_MAX_SLOTS];
    int      q_head, q_len;
    int      current;
    PtStats  st;
    uint64_t last_present_ns;
} P;
static int s_pt_inited = 0;

static void *present_loop(void) {
    psx_host_thread_name(L"PSX present");
    int ok = P.cfg.ctx(P.cfg.user, 1);
    mtx_lock(&P.mtx);
    P.init_done = 1;
    P.init_ok = ok;
    cnd_bcast(&P.cv);
    if (!ok) { mtx_unlock(&P.mtx); return NULL; }
    for (;;) {
        while (!P.q_len && !P.stop) cnd_wait(&P.cv, &P.mtx);
        if (!P.q_len) break;                 /* stop with nothing queued */
        const int slot = P.queue[P.q_head];
        const PtTiming *timing = &P.timing[slot];
        const uint64_t now = now_ns();
        int drop = timing->epoch && (P.stop || timing->epoch != P.epoch ||
                                      now >= timing->expires_ns);
        if (timing->epoch && !drop && now < timing->not_before_ns) {
            cnd_wait_ns(&P.cv, &P.mtx, timing->not_before_ns - now);
            continue;  /* recheck stop/epoch/expiry after every wake */
        }
        P.q_head = (P.q_head + 1) % PT_MAX_SLOTS;
        P.q_len--;
        P.state[slot] = S_PRESENTING;
        void *ready = P.ready[slot];
        P.ready[slot] = NULL;
        mtx_unlock(&P.mtx);
        const uint64_t t0 = now_ns();
        void *done = drop ? ready : P.cfg.present(P.cfg.user, slot, ready);
        const uint64_t finished = now_ns();
        const uint64_t dt = finished - t0;
        mtx_lock(&P.mtx);
        if (drop) P.st.timed_dropped++;
        else {
            P.st.presents++;
            P.st.present_ns += dt;
            if (dt > P.st.present_max_ns) P.st.present_max_ns = dt;
            if (P.last_present_ns) {
                const uint64_t gap = finished - P.last_present_ns;
                if (gap > P.st.present_gap_max_ns) P.st.present_gap_max_ns = gap;
                if (gap > 25000000ull) P.st.present_gaps_over_25ms++;
            }
            P.last_present_ns = finished;
        }
        P.done[slot] = done;
        P.state[slot] = S_FREE;
        cnd_bcast(&P.cv);
    }
    /* Tokens nobody will collect any more. */
    for (int i = 0; i < P.cfg.slots; i++) {
        if (P.done[i] && P.cfg.dispose) P.cfg.dispose(P.cfg.user, P.done[i]);
        P.done[i] = NULL;
    }
    mtx_unlock(&P.mtx);
    P.cfg.ctx(P.cfg.user, 0);
    return NULL;
}

#ifdef _WIN32
static DWORD WINAPI present_main(LPVOID a) { (void)a; present_loop(); return 0; }
#else
static void *present_main(void *a) { (void)a; return present_loop(); }
#endif

int pt_start(const PtConfig *cfg) {
    if (P.running || !cfg || !cfg->ctx || !cfg->present) return 0;
    if (cfg->slots < 2 || cfg->slots > PT_MAX_SLOTS) return 0;
    if (!s_pt_inited) { mtx_init(&P.mtx); cnd_init(&P.cv); s_pt_inited = 1; }
    mtx_lock(&P.mtx);
    P.cfg = *cfg;
    P.stop = P.init_done = P.init_ok = 0;
    P.q_head = P.q_len = 0;
    P.epoch = 0;
    memset(P.timing, 0, sizeof P.timing);
    for (int i = 0; i < PT_MAX_SLOTS; i++) {
        P.state[i] = S_FREE;
        P.ready[i] = P.done[i] = NULL;
    }
    P.current = 0;
    P.state[0] = S_COMPOSING;
    memset(&P.st, 0, sizeof P.st);
    P.last_present_ns = 0;
    P.st.slots = cfg->slots;
    mtx_unlock(&P.mtx);
    int ok;
#ifdef _WIN32
    P.thread = CreateThread(NULL, 0, present_main, NULL, 0, NULL);
    ok = P.thread != NULL;
    if (ok) SetThreadPriority(P.thread, THREAD_PRIORITY_ABOVE_NORMAL);
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
#  if defined(__APPLE__)
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
#  endif
    ok = pthread_create(&P.thread, &attr, present_main, NULL) == 0;
    pthread_attr_destroy(&attr);
#endif
    if (!ok) return 0;
    mtx_lock(&P.mtx);
    while (!P.init_done) cnd_wait(&P.cv, &P.mtx);
    ok = P.init_ok;
    mtx_unlock(&P.mtx);
    if (!ok) {
#ifdef _WIN32
        WaitForSingleObject(P.thread, INFINITE);
        CloseHandle(P.thread);
#else
        pthread_join(P.thread, NULL);
#endif
        return 0;
    }
    P.running = 1;
    return 1;
}

void pt_stop(void) {
    if (!P.running) return;
    mtx_lock(&P.mtx);
    P.stop = 1;
    cnd_bcast(&P.cv);
    mtx_unlock(&P.mtx);
#ifdef _WIN32
    WaitForSingleObject(P.thread, INFINITE);
    CloseHandle(P.thread);
#else
    pthread_join(P.thread, NULL);
#endif
    P.running = 0;
}

int pt_running(void) { return P.running; }
int pt_current(void) { return P.running ? P.current : -1; }

int pt_submit(void *ready, void **next_done, uint64_t *wait_ns) {
    if (next_done) *next_done = NULL;
    if (wait_ns) *wait_ns = 0;
    if (!P.running) return -1;
    mtx_lock(&P.mtx);
    const int cur = P.current;
    int next = -1;
    for (int k = 1; k < P.cfg.slots; k++) {
        const int slot = (cur + k) % P.cfg.slots;
        if (P.state[slot] == S_FREE) { next = slot; break; }
    }
    if (next < 0) {
        P.st.refused++;
        mtx_unlock(&P.mtx);
        return -2; /* caller retains both the composing slot and ready token */
    }
    memset(&P.timing[cur], 0, sizeof P.timing[cur]);
    P.ready[cur] = ready;
    P.state[cur] = S_QUEUED;
    P.queue[(P.q_head + P.q_len) % PT_MAX_SLOTS] = cur;
    P.q_len++;
    P.st.submits++;
    if (P.q_len > P.st.queued_high) P.st.queued_high = P.q_len;
    cnd_bcast(&P.cv);
    P.state[next] = S_COMPOSING;
    P.current = next;
    if (next_done) *next_done = P.done[next];
    else if (P.done[next] && P.cfg.dispose) P.cfg.dispose(P.cfg.user, P.done[next]);
    P.done[next] = NULL;
    mtx_unlock(&P.mtx);
    return next;
}

uint64_t pt_clock_ns(void) { return now_ns(); }

void pt_set_epoch(uint64_t epoch) {
    if (!P.running) return;
    mtx_lock(&P.mtx);
    P.epoch = epoch;
    cnd_bcast(&P.cv);
    mtx_unlock(&P.mtx);
}

int pt_try_submit_at(void *ready, const PtTiming *timing, void **next_done) {
    int next = -1;
    if (next_done) *next_done = NULL;
    if (!P.running) return -1;
    mtx_lock(&P.mtx);
    const uint64_t now = now_ns();
    if (timing && timing->epoch && timing->expires_ns > timing->not_before_ns &&
        !P.stop && timing->epoch == P.epoch && now < timing->expires_ns) {
        for (int k = 1; k < P.cfg.slots; ++k) {
            const int slot = (P.current + k) % P.cfg.slots;
            if (P.state[slot] == S_FREE) { next = slot; break; }
        }
    }
    if (next < 0) {
        P.st.timed_refused++;
        mtx_unlock(&P.mtx);
        return -2;
    }
    const int cur = P.current;
    P.timing[cur] = *timing;
    P.ready[cur] = ready;
    P.state[cur] = S_QUEUED;
    /* Reservations can publish a future base image before an earlier
     * worker finishes. Keep timed outputs in timestamp order, without moving
     * them across legacy untimed submissions or another session's epoch. */
    int insert=P.q_len;
    while (insert>0) {
        const int prior=P.queue[(P.q_head+insert-1)%PT_MAX_SLOTS];
        if (P.timing[prior].epoch!=timing->epoch ||
            P.timing[prior].not_before_ns<=timing->not_before_ns) break;
        P.queue[(P.q_head+insert)%PT_MAX_SLOTS]=prior;
        --insert;
    }
    P.queue[(P.q_head+insert)%PT_MAX_SLOTS]=cur;
    P.q_len++;
    P.st.submits++;
    if (P.q_len > P.st.queued_high) P.st.queued_high = P.q_len;
    P.state[next] = S_COMPOSING;
    P.current = next;
    if (next_done) *next_done = P.done[next];
    else if (P.done[next] && P.cfg.dispose) P.cfg.dispose(P.cfg.user, P.done[next]);
    P.done[next] = NULL;
    cnd_bcast(&P.cv);
    mtx_unlock(&P.mtx);
    return next;
}

void pt_drain(void) {
    if (!P.running) return;
    mtx_lock(&P.mtx);
    for (;;) {
        int busy = P.q_len > 0;
        for (int i = 0; i < P.cfg.slots && !busy; i++)
            busy = P.state[i] == S_PRESENTING;
        if (!busy) break;
        cnd_wait(&P.cv, &P.mtx);
    }
    mtx_unlock(&P.mtx);
}

int pt_present_timing(unsigned slot, PtTiming *out) {
    if (!s_pt_inited || !out || slot >= PT_MAX_SLOTS) return 0;
    mtx_lock(&P.mtx);
    int ok = P.running && P.state[slot] == S_PRESENTING;
    if (ok) *out = P.timing[slot];
    mtx_unlock(&P.mtx);
    return ok;
}

void pt_get_stats(PtStats *out) {
    if (!out) return;
    if (!s_pt_inited) { memset(out, 0, sizeof *out); return; }
    mtx_lock(&P.mtx);
    *out = P.st;
    mtx_unlock(&P.mtx);
}
