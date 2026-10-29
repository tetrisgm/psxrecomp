#ifndef PSX_FRAME_MAILBOX_H
#define PSX_FRAME_MAILBOX_H
#include <array>
#include <atomic>
#include <cstdlib>

/* SPSC latest-complete-frame exchange. Exactly one writable, one pending and
 * one consumed packet can exist. Publication never waits for the consumer;
 * an unclaimed pending packet is recycled. A claimed packet is immutable
 * until release. Payloads must be self-contained: dropping a packet must not
 * lose state required by a later packet. Lifecycle requires stopped threads. */
template<class Packet> class PsxFrameMailbox {
    static_assert(std::atomic<int>::is_always_lock_free &&
                  std::atomic<bool>::is_always_lock_free &&
                  std::atomic<unsigned long long>::is_always_lock_free,
                  "frame publication requires lock-free ownership counters");
    std::array<Packet,3> packets;
    std::array<std::atomic<bool>,3> owned{{false,false,false}};
    alignas(64) std::atomic<int> pending{-1};
    int producer=0;
    int consumer=-1;
    std::atomic<unsigned long long> replaced{0};
public:
    PsxFrameMailbox() { owned[0].store(true,std::memory_order_relaxed); }
    PsxFrameMailbox(const PsxFrameMailbox&)=delete;
    PsxFrameMailbox& operator=(const PsxFrameMailbox&)=delete;
    Packet &writing() { return packets[producer]; }
    void discard_pending() {
        const int old=pending.exchange(-1,std::memory_order_acq_rel);
        if(old>=0) {
            owned[old].store(false,std::memory_order_release);
            replaced.fetch_add(1,std::memory_order_relaxed);
        }
    }
    void publish() {
        const int old=pending.exchange(producer,std::memory_order_acq_rel);
        if(old>=0) {
            owned[old].store(false,std::memory_order_release);
            replaced.fetch_add(1,std::memory_order_relaxed);
        }
        for(int i=0;i<3;++i) {
            bool free=false;
            if(owned[i].compare_exchange_strong(free,true,std::memory_order_acquire)) {
                producer=i;
                return;
            }
        }
        /* With one consumer lease and one pending packet, one slot is free.
         * Failure means a producer/consumer violated the single-owner API. */
        std::abort();
    }
    const Packet *acquire() {
        if(consumer>=0) std::abort();
        consumer=pending.exchange(-1,std::memory_order_acquire);
        return consumer<0?nullptr:&packets[consumer];
    }
    void release() {
        if(consumer<0) std::abort();
        owned[consumer].store(false,std::memory_order_release);
        consumer=-1;
    }
    unsigned long long dropped() const { return replaced.load(std::memory_order_relaxed); }
};
#endif
