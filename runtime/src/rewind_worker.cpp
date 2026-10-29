#include "rewind_worker.h"
#include <atomic>
#include "worker_event.h"
#include <thread>

struct RewindWorker {
    WorkerEvent wake,finished;
    std::atomic<bool> ready{true};
    std::atomic<bool> requested{false};
    std::atomic<bool> stop{false};
    void (*run)(void *);
    void *context;
    std::thread thread;
    RewindWorker(void (*fn)(void *),void *ctx):run(fn),context(ctx) {}
    void loop() {
        for(;;) {
            wake.wait();
            if(!requested.exchange(false,std::memory_order_acquire)) {
                if(stop.load(std::memory_order_acquire))break;
                continue;
            }
            run(context);
            ready.store(true,std::memory_order_release);
            finished.notify();
        }
    }
};
extern "C" RewindWorker *rewind_worker_create(void (*run)(void *),void *context) {
    if(!run)return nullptr;
    RewindWorker *w=nullptr;
    try {
        w=new RewindWorker(run,context);
        w->thread=std::thread(&RewindWorker::loop,w);return w;
    } catch(...) { delete w;return nullptr; }
}
extern "C" int rewind_worker_submit(RewindWorker *w) {
    if(!w || !w->ready.load(std::memory_order_acquire))return 0;
    /* Single producer, one outstanding job. The retained event avoids both
     * queue-mutex contention and timed-poll latency on the Windows host. */
    w->ready.store(false,std::memory_order_release);
    w->requested.store(true,std::memory_order_release);w->wake.notify();return 1;
}
extern "C" int rewind_worker_ready(RewindWorker *w) {
    return w && w->ready.load(std::memory_order_acquire);
}
extern "C" void rewind_worker_wait(RewindWorker *w) {
    if(!w)return;
    while(!w->ready.load(std::memory_order_acquire))w->finished.wait();
}
extern "C" void rewind_worker_destroy(RewindWorker *w) {
    if(!w)return;
    rewind_worker_wait(w);
    w->stop.store(true,std::memory_order_release);w->wake.notify();
    w->thread.join();delete w;
}
