#include "rewind_worker.h"
#include <atomic>
#include <cassert>
#include <thread>
struct Work {std::atomic<bool> entered{false},release{false};unsigned result=0;};
static void run(void *context) {
    auto &w=*static_cast<Work*>(context);w.entered.store(true);
    while(!w.release.load())std::this_thread::yield();
    w.result++;
}
int main() {
    Work work;auto worker=rewind_worker_create(run,&work);assert(worker);
    assert(rewind_worker_submit(worker));
    while(!work.entered.load())std::this_thread::yield();
    assert(!rewind_worker_ready(worker));assert(!rewind_worker_submit(worker));
    work.release.store(true);rewind_worker_wait(worker);assert(work.result==1);
    for(unsigned i=0;i<1000;i++) {
        assert(rewind_worker_submit(worker));rewind_worker_wait(worker);
        assert(work.result==i+2);
    }
    assert(rewind_worker_submit(worker));rewind_worker_destroy(worker);
    assert(work.result==1002);
}
