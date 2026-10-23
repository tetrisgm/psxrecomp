#include "draw_worker_pool.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>
#include <cstdio>

struct State {
    std::atomic<uint64_t> now{100};
    std::atomic<unsigned> started{0}, disposed{0};
    std::atomic<bool> go{false};
};
struct Payload { State *state; unsigned value; };
static uint64_t clock_ns(void *user) { return ((State *)user)->now.load(); }
static int execute(void *user, unsigned slot, void *data) {
    auto *s = (State *)user;
    ++s->started;
    while (!s->go.load()) std::this_thread::yield();
    ((Payload *)data)->value = slot + 42;
    return 0;
}
static void dispose(void *data) {
    auto *p = (Payload *)data;
    ++p->state->disposed;
    delete p;
}
template<class P> static void until(P predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < end);
        std::this_thread::yield();
    }
}
static DrawWorkerJob job(State &s, unsigned i, uint64_t epoch = 1) {
    return {epoch, 7, i, 200 + i, 300, new Payload{&s, 0}, dispose};
}
int main() {
    State s;
    assert(!draw_worker_create(0, 1, clock_ns, execute, &s));
    auto *pool = draw_worker_create(4, 1, clock_ns, execute, &s);
    assert(pool);
    for (unsigned i = 0; i < 4; ++i) {
        auto j = job(s, i);
        assert(draw_worker_submit(pool, &j));
    }
    until([&] { return s.started == 4; });
    auto extra = job(s, 4);
    assert(!draw_worker_submit(pool, &extra));
    assert(s.disposed == 0); // rejected payload still belongs to caller
    dispose(extra.payload);
    DrawWorkerResult r{};
    assert(!draw_worker_poll(pool, &r)); // main never waits for callbacks
    s.go = true;
    unsigned collected = 0;
    until([&] {
        if (draw_worker_poll(pool, &r)) {
            assert(!r.status && draw_worker_current(pool, &r, 7));
            assert(!draw_worker_current(pool, &r, 8));
            assert(((Payload *)r.job.payload)->value == r.slot + 42);
            assert(draw_worker_release(pool, &r));
            assert(!draw_worker_release(pool, &r));
            ++collected;
        }
        return collected == 4;
    });
    assert(s.disposed == 5);

    // Running old-generation work finishes privately and cannot be consumed.
    s.go = false;
    auto old = job(s, 0);
    assert(draw_worker_submit(pool, &old));
    until([&] { return s.started == 5; });
    draw_worker_set_epoch(pool, 2);
    s.go = true;
    until([&] { return draw_worker_poll(pool, &r); });
    assert(r.status == DRAW_WORKER_CANCELLED);
    assert(!draw_worker_current(pool, &r, 7));
    assert(draw_worker_release(pool, &r));

    // A result that expires after claiming is also refused at consumption.
    auto fresh = job(s, 0, 2);
    assert(draw_worker_submit(pool, &fresh));
    until([&] { return draw_worker_poll(pool, &r); });
    assert(draw_worker_current(pool, &r, 7));
    s.now = 300;
    assert(!draw_worker_current(pool, &r, 7));
    assert(draw_worker_release(pool, &r));
    auto expired = job(s, 0, 2);
    assert(!draw_worker_submit(pool, &expired));
    dispose(expired.payload);

    // Teardown owns unclaimed results and cancels queued work without leaks.
    s.now = 100;
    for (unsigned i = 0; i < 4; ++i) {
        auto j = job(s, i, 2);
        assert(draw_worker_submit(pool, &j));
    }
    draw_worker_destroy(pool);
    assert(s.disposed == 12);
    std::puts("PASS: four draw workers, backpressure, epochs, expiry, ownership");
}
