#include "draw_worker_pool.h"
#include <condition_variable>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

enum SlotState { FREE, QUEUED, RUNNING, READY, CLAIMED };
struct DrawSlot {
    SlotState state = FREE;
    DrawWorkerJob job{};
    uint64_t ticket = 0;
    int status = 0;
    bool executed = false;
    int execution_status = 0;
};
struct DrawWorkerPool {
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<DrawSlot> slots;
    std::vector<std::thread> threads;
    DrawWorkerClock clock;
    DrawWorkerExecute execute;
    void *user;
    uint64_t epoch, serial = 0;
    bool stop = false;
};

static int invalid(DrawWorkerPool *p, const DrawWorkerJob &j) {
    if (j.epoch != p->epoch) return DRAW_WORKER_CANCELLED;
    if (p->clock(p->user) >= j.expires) return DRAW_WORKER_EXPIRED;
    return 0;
}
static void run(DrawWorkerPool *p, unsigned i) {
    std::unique_lock<std::mutex> lock(p->mutex);
    DrawSlot &s = p->slots[i];
    for (;;) {
        p->wake.wait(lock, [&] { return p->stop || s.state == QUEUED; });
        if (p->stop) return;
        s.status = invalid(p, s.job);
        if (s.status) { s.state = READY; continue; }
        s.state = RUNNING;
        void *payload = s.job.payload;
        lock.unlock();
        const int status = p->execute(p->user, i, payload);
        lock.lock();
        s.executed = true;
        s.execution_status = status;
        s.status = invalid(p, s.job);
        if (!s.status) s.status = status;
        s.state = READY;
    }
}
extern "C" DrawWorkerPool *draw_worker_create(unsigned workers, uint64_t epoch,
    DrawWorkerClock clock, DrawWorkerExecute execute, void *user) {
    if (!workers || workers > 16 || !clock || !execute) return nullptr;
    auto *p = new (std::nothrow) DrawWorkerPool;
    if (!p) return nullptr;
    p->clock = clock; p->execute = execute; p->user = user; p->epoch = epoch;
    try {
        p->slots.resize(workers);
        p->threads.reserve(workers);
        for (unsigned i = 0; i < workers; ++i) p->threads.emplace_back(run, p, i);
    } catch (...) { draw_worker_destroy(p); return nullptr; }
    return p;
}
extern "C" int draw_worker_submit(DrawWorkerPool *p, const DrawWorkerJob *job) {
    if (!p || !job || !job->payload || job->deadline >= job->expires) return 0;
    std::lock_guard<std::mutex> lock(p->mutex);
    if (p->stop || invalid(p, *job)) return 0;
    for (auto &s : p->slots) if (s.state == FREE) {
        if (++p->serial == 0) ++p->serial;
        s.job = *job; s.ticket = p->serial; s.state = QUEUED;
        s.executed = false; s.execution_status = 0;
        p->wake.notify_all();
        return 1;
    }
    return 0;
}
extern "C" int draw_worker_poll(DrawWorkerPool *p, DrawWorkerResult *out) {
    if (!p || !out) return 0;
    std::lock_guard<std::mutex> lock(p->mutex);
    unsigned best = (unsigned)p->slots.size();
    for (unsigned i = 0; i < p->slots.size(); ++i) {
        const auto &s = p->slots[i];
        if (s.state == READY && (best == p->slots.size() ||
            s.job.deadline < p->slots[best].job.deadline)) best = i;
    }
    if (best == p->slots.size()) return 0;
    auto &s = p->slots[best];
    const int stale = invalid(p, s.job);
    /* Do not publish a later image while an earlier live job can still make
     * its deadline. Returning zero leaves main free to run and poll again;
     * an expired or cancelled predecessor never holds subsequent work. */
    if (!stale && !s.status) {
        const uint64_t now = p->clock(p->user);
        for (const auto &earlier : p->slots)
            if ((earlier.state == QUEUED || earlier.state == RUNNING) &&
                earlier.job.epoch == p->epoch &&
                earlier.job.deadline < s.job.deadline &&
                now < earlier.job.deadline) return 0;
    }
    if (stale) s.status = stale;
    s.state = CLAIMED;
    *out = {s.ticket, best, s.status, s.job, s.executed, s.execution_status};
    return 1;
}
extern "C" int draw_worker_current(DrawWorkerPool *p, const DrawWorkerResult *r,
                                      uint64_t generation) {
    if (!p || !r) return 0;
    std::lock_guard<std::mutex> lock(p->mutex);
    if (r->slot >= p->slots.size()) return 0;
    const auto &s = p->slots[r->slot];
    return s.state == CLAIMED && s.ticket == r->ticket && !s.status &&
        s.job.generation == generation && !invalid(p, s.job);
}
extern "C" int draw_worker_release(DrawWorkerPool *p, const DrawWorkerResult *r) {
    if (!p || !r) return 0;
    DrawWorkerJob job{};
    {
        std::lock_guard<std::mutex> lock(p->mutex);
        if (r->slot >= p->slots.size()) return 0;
        auto &s = p->slots[r->slot];
        if (s.state != CLAIMED || s.ticket != r->ticket) return 0;
        job = s.job; s.job = {}; s.state = FREE;
    }
    if (job.dispose) job.dispose(job.payload);
    return 1;
}
extern "C" void draw_worker_set_epoch(DrawWorkerPool *p, uint64_t epoch) {
    if (!p) return;
    std::lock_guard<std::mutex> lock(p->mutex);
    p->epoch = epoch;
    for (auto &s : p->slots) if (s.state == QUEUED && s.job.epoch != epoch) {
        s.status = DRAW_WORKER_CANCELLED; s.state = READY;
    }
}
extern "C" void draw_worker_destroy(DrawWorkerPool *p) {
    if (!p) return;
    {
        std::lock_guard<std::mutex> lock(p->mutex);
        p->stop = true;
        p->wake.notify_all();
    }
    for (auto &t : p->threads) if (t.joinable()) t.join();
    for (auto &s : p->slots)
        if (s.state != FREE && s.job.dispose) s.job.dispose(s.job.payload);
    delete p;
}
