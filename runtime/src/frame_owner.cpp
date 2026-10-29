#include "frame_owner.h"
#include "host_scheduling.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstdlib>

static thread_local FrameOwner *current_owner=nullptr;

struct FrameOwner {
    FrameOwnerConfig config;
    FramePacketQueue *queue=nullptr;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable ready;
    uint64_t published=0; // wake generation, protected only by mutex
    bool stop=false;
    std::atomic<uint64_t> completed{0};
    uint64_t deadline=0; // consumer only
    ~FrameOwner() { frame_packet_destroy(queue); }
    void wake() {
        { std::lock_guard<std::mutex> lock(mutex); ++published; }
        ready.notify_one();
    }
    void run() {
        psx_host_schedule_thread(PsxHostThreadRole::Render);
        current_owner=this;
        config.context(config.user,1);
        uint64_t seen=0;
        for(;;) {
            bool stopping;
            {
                std::unique_lock<std::mutex> lock(mutex);
                while(!stop && published==seen) {
                    if(deadline) {
                        const uint64_t now=config.clock_ns();
                        if(now>=deadline) break;
                        ready.wait_for(lock,std::chrono::nanoseconds(deadline-now));
                    } else ready.wait(lock);
                }
                seen=published;stopping=stop;
            }
            if(!stopping && deadline) {
                const uint64_t now=config.clock_ns();
                if(now>=deadline) {
                    deadline=0;
                    const uint64_t next=config.tick(config.user,now);
                    if(next && (!deadline || next<deadline)) deadline=next;
                }
            }
            FramePacketView packet{};
            if(frame_packet_acquire(queue,&packet)) {
                if(frame_packet_current(queue,&packet)) config.execute(config.user,&packet);
                completed.fetch_add(1,std::memory_order_release);
                frame_packet_release(queue);
            }
            if(stopping) break;
        }
        config.context(config.user,0);
        current_owner=nullptr;
    }
};
extern "C" FrameOwner *frame_owner_start(const FrameOwnerConfig *config) {
    if(!config || !config->execute || !config->context ||
       (config->tick && !config->clock_ns)) return nullptr;
    try {
        auto owner=std::make_unique<FrameOwner>();
        owner->config=*config;
        owner->queue=frame_packet_create(config->initial_limit,config->record_limit);
        if(!owner->queue) return nullptr;
        config->context(config->user,0);
        try { owner->thread=std::thread(&FrameOwner::run,owner.get()); }
        catch(...) { config->context(config->user,1);return nullptr; }
        return owner.release();
    } catch(...) { return nullptr; }
}
extern "C" void frame_owner_stop(FrameOwner *owner) {
    if(!owner) return;
    { std::lock_guard<std::mutex> lock(owner->mutex);owner->stop=true; }
    owner->ready.notify_one();
    owner->thread.join();
    owner->config.context(owner->config.user,1);
    delete owner;
}
extern "C" int frame_owner_begin(FrameOwner *owner,uint64_t sequence,const void *initial,size_t bytes) {
    return frame_packet_begin(owner->queue,sequence,initial,bytes);
}
extern "C" int frame_owner_attach(FrameOwner *owner,void *attachment,size_t bytes,void (*dispose)(void*)) {
    return frame_packet_attach(owner->queue,attachment,bytes,dispose);
}
extern "C" void *frame_owner_append(FrameOwner *owner,uint16_t op,uint16_t flags,uint32_t bytes) {
    return frame_packet_append(owner->queue,op,flags,bytes);
}
extern "C" int frame_owner_publish(FrameOwner *owner) {
    const int result=frame_packet_publish(owner->queue);
    if(result) owner->wake();
    return result;
}
extern "C" void frame_owner_invalidate(FrameOwner *owner) { frame_packet_invalidate(owner->queue); }
extern "C" int frame_owner_current(const FrameOwner *owner,const FramePacketView *packet) {
    return frame_packet_current(owner->queue,packet);
}
extern "C" uint64_t frame_owner_replaced(const FrameOwner *owner) { return frame_packet_replaced(owner->queue); }
extern "C" uint64_t frame_owner_completed(const FrameOwner *owner) { return owner->completed.load(std::memory_order_acquire); }
extern "C" int frame_owner_is_thread(void) { return current_owner!=nullptr; }
extern "C" void frame_owner_tick_at(FrameOwner *owner,uint64_t deadline) {
    if(current_owner!=owner || !owner->config.tick) std::abort();
    if(deadline && (!owner->deadline || deadline<owner->deadline)) owner->deadline=deadline;
}
