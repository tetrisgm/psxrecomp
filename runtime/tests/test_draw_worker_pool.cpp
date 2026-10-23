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
    std::atomic<int> result{0};
};
struct Payload { State *state; unsigned value; };
static uint64_t clock_ns(void *user) { return ((State *)user)->now.load(); }
static int execute(void *user, unsigned slot, void *data) {
    auto *s = (State *)user;
    ++s->started;
    while (!s->go.load()) std::this_thread::yield();
    ((Payload *)data)->value = slot + 42;
    return s->result.load();
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
struct OrderedState {
    std::atomic<uint64_t> now{100};
    std::atomic<unsigned> started{0}, later_done{0};
    std::atomic<bool> release_early{false};
};
static uint64_t ordered_clock(void *p) { return ((OrderedState *)p)->now.load(); }
static int ordered_execute(void *p, unsigned, void *payload) {
    auto *s=(OrderedState *)p;
    ++s->started;
    if (*(int *)payload==0)
        while (!s->release_early.load()) std::this_thread::yield();
    else ++s->later_done;
    return 0;
}
static void test_completion_order() {
    // Later finishes first. Hold it without blocking main, until the earlier
    // result arrives, its deadline passes, or its epoch is cancelled.
    for (int mode=0;mode<3;++mode) {
        OrderedState s; int first=0,second=1;
        auto *p=draw_worker_create(2,1,ordered_clock,ordered_execute,&s);
        assert(p);
        DrawWorkerJob a{1,1,0,200,400,&first,nullptr};
        DrawWorkerJob b{1,1,1,300,400,&second,nullptr};
        assert(draw_worker_submit(p,&a) && draw_worker_submit(p,&b));
        until([&]{return s.started==2 && s.later_done==1;});
        DrawWorkerResult r{};
        auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(20);
        do { assert(!draw_worker_poll(p,&r)); std::this_thread::yield(); }
        while (std::chrono::steady_clock::now()<end);
        if (mode==0) {
            s.release_early=true;
            until([&]{return draw_worker_poll(p,&r);});
            assert(r.job.index==0 && !r.status);
            assert(draw_worker_release(p,&r));
            until([&]{return draw_worker_poll(p,&r);});
            assert(r.job.index==1 && !r.status);
        } else {
            if (mode==1) s.now=200; // exact earlier deadline releases later job
            else draw_worker_set_epoch(p,2);
            until([&]{return draw_worker_poll(p,&r);});
            assert(r.job.index==1);
            assert(r.status==(mode==1?DRAW_WORKER_COMPLETE:DRAW_WORKER_CANCELLED));
            s.release_early=true;
        }
        assert(draw_worker_release(p,&r));
        draw_worker_destroy(p);
    }
}
int main() {
    test_completion_order();
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

    // Admission expiry/cancellation must not hide a callback failure.
    for (int cancel = 0; cancel < 2; ++cancel) {
        s.now = 100; s.go = false; s.result = -3;
        unsigned before = s.started.load();
        auto failing = job(s, 0, 2 + cancel);
        assert(draw_worker_submit(pool, &failing));
        until([&] { return s.started == before + 1; });
        if (cancel) draw_worker_set_epoch(pool, 4);
        else s.now = 300;
        s.go = true;
        until([&] { return draw_worker_poll(pool, &r); });
        assert(r.executed && r.execution_status == -3);
        assert(r.status == (cancel ? DRAW_WORKER_CANCELLED : DRAW_WORKER_EXPIRED));
        assert(!draw_worker_current(pool, &r, 7));
        assert(draw_worker_release(pool, &r));
        if (!cancel) draw_worker_set_epoch(pool, 3);
    }
    s.result = 0;
    // Teardown owns unclaimed results and cancels queued work without leaks.
    s.now = 100;
    for (unsigned i = 0; i < 4; ++i) {
        auto j = job(s, i, 4);
        assert(draw_worker_submit(pool, &j));
    }
    draw_worker_destroy(pool);
    assert(s.disposed == 14);
    std::puts("PASS: four draw workers, backpressure, epochs, expiry, ownership");
}
