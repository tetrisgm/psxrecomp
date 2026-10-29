#include "frame_mailbox.h"
#include "frame_packet.h"
#include "frame_owner.h"
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <thread>
#include <cstdio>
#include <cstring>
#include <vector>
#include <chrono>

struct Packet { uint64_t sequence=0; std::array<uint64_t,1024> words{}; };
static void write(Packet &p,uint64_t sequence) {
    p.sequence=sequence;
    for(size_t i=0;i<p.words.size();++i) p.words[i]=sequence^(i*UINT64_C(0x123456789));
}
static void verify(const Packet &p) {
    for(size_t i=0;i<p.words.size();++i) assert(p.words[i]==(p.sequence^(i*UINT64_C(0x123456789))));
}
struct OwnerTest {
    std::thread::id producer=std::this_thread::get_id();
    std::atomic<bool> entered{false},unblock{false};
    std::atomic<unsigned> acquire{0},release{0};
    std::atomic<uint64_t> last{0};
    std::atomic<unsigned> ticks{0};
    FrameOwner *owner=nullptr;
};
static uint64_t owner_clock() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
static uint64_t tick(void *user,uint64_t now) {
    auto &state=*static_cast<OwnerTest *>(user);
    assert(frame_owner_is_thread());
    return ++state.ticks==1?now+1000000:0;
}
static void context(void *user,int current) {
    auto &state=*static_cast<OwnerTest *>(user);
    if(current) ++state.acquire;else ++state.release;
}
static void execute(void *user,const FramePacketView *packet) {
    auto &state=*static_cast<OwnerTest *>(user);
    assert(std::this_thread::get_id()!=state.producer);
    assert(frame_owner_is_thread());
    if(packet->sequence==1) {
        state.entered.store(true,std::memory_order_release);
        while(!state.unblock.load(std::memory_order_acquire)) std::this_thread::yield();
    }
    assert(packet->initial_bytes==8);
    assert(*static_cast<const uint64_t *>(packet->initial)==packet->sequence);
    if(packet->sequence==10000) frame_owner_tick_at(state.owner,owner_clock()+1000000);
    state.last.store(packet->sequence,std::memory_order_release);
}
int main() {
    OwnerTest state;
    const FrameOwnerConfig config{8,4096,execute,context,&state,tick,owner_clock};
    FrameOwner *owner=frame_owner_start(&config);assert(owner);
    state.owner=owner;
    assert(!frame_owner_is_thread());
    uint64_t number=1;
    assert(frame_owner_begin(owner,number,&number,sizeof number));
    assert(frame_owner_publish(owner));
    while(!state.entered.load(std::memory_order_acquire)) std::this_thread::yield();
    for(number=2;number<=10000;++number) {
        assert(frame_owner_begin(owner,number,&number,sizeof number));
        assert(frame_owner_publish(owner));
    }
    /* The graphics callback is deliberately stuck, but publication finishes
     * and context ownership never returns to the producer during execution. */
    assert(frame_owner_replaced(owner)==9998);
    assert(state.acquire==1 && state.release==1);
    state.unblock.store(true,std::memory_order_release);
    while(frame_owner_completed(owner)<2) std::this_thread::yield();
    assert(state.last.load(std::memory_order_acquire)==10000);
    /* Timed presentation work continues while the guest publishes nothing. */
    while(state.ticks.load(std::memory_order_acquire)<2) std::this_thread::yield();
    frame_owner_stop(owner);
    assert(state.acquire==2 && state.release==2);
    /* The C renderer's owned packet carries its seed independently of all
     * skipped packets. Neither caller buffers nor later appends may mutate
     * the consumer's current packet. Overflow rejects a whole packet. */
    FramePacketQueue *queue=frame_packet_create(1024*1024,1024*1024);
    assert(queue);
    std::vector<uint16_t> native(1024*512,0x1234);
    assert(frame_packet_begin(queue,1,native.data(),native.size()*2));
    auto *words=static_cast<uint32_t *>(frame_packet_append(queue,1,7,12));
    assert(words && uintptr_t(words)%RT_ALIGN==0);
    words[0]=11;words[1]=22;words[2]=33;
    assert(frame_packet_publish(queue));
    FramePacketView view;
    assert(frame_packet_acquire(queue,&view));
    const auto held_view=view;
    for(uint64_t n=2;n<=64;++n) {
        native[0]=uint16_t(n);
        assert(frame_packet_begin(queue,n,native.data(),native.size()*2));
        auto *p=static_cast<uint64_t *>(frame_packet_append(queue,2,0,8));
        assert(p);*p=n;
        assert(frame_packet_publish(queue));
    }
    assert(static_cast<const uint16_t *>(held_view.initial)[0]==0x1234);
    const auto *cmd=static_cast<const RtCmd *>(held_view.records);
    assert(cmd->op==1 && cmd->flags==7 && cmd->payload==12 && cmd->size==32);
    assert(static_cast<const uint32_t *>(static_cast<const void *>(cmd+1))[2]==33);
    assert(frame_packet_replaced(queue)==62);
    /* Failing a later packet must not erase the last complete pending one. */
    assert(frame_packet_begin(queue,65,native.data(),native.size()*2));
    assert(!frame_packet_append(queue,2,0,1024*1024));
    assert(!frame_packet_publish(queue));
    frame_packet_release(queue);
    assert(frame_packet_acquire(queue,&view));
    assert(view.sequence==64 && static_cast<const uint16_t *>(view.initial)[0]==64);
    assert(frame_packet_current(queue,&view));
    cmd=static_cast<const RtCmd *>(view.records);
    assert(*reinterpret_cast<const uint64_t *>(cmd+1)==64);
    frame_packet_invalidate(queue);
    assert(!frame_packet_current(queue,&view));
    /* Invalidation cannot free the image still being read by the renderer. */
    assert(*reinterpret_cast<const uint64_t *>(cmd+1)==64);
    frame_packet_release(queue);
    assert(!frame_packet_acquire(queue,&view));
    assert(frame_packet_begin(queue,66,nullptr,0));
    assert(frame_packet_publish(queue));
    frame_packet_invalidate(queue);
    assert(!frame_packet_acquire(queue,&view));
    assert(frame_packet_begin(queue,67,nullptr,0));
    frame_packet_invalidate(queue);
    assert(!frame_packet_publish(queue));
    assert(frame_packet_begin(queue,68,nullptr,0));
    assert(frame_packet_publish(queue));
    assert(frame_packet_acquire(queue,&view) && frame_packet_current(queue,&view));
    frame_packet_release(queue);
    frame_packet_destroy(queue);
    PsxFrameMailbox<Packet> mailbox;
    assert(!mailbox.acquire());
    write(mailbox.writing(),1);mailbox.publish();
    const Packet *held=mailbox.acquire();assert(held && held->sequence==1);
    /* A completely stopped consumer cannot block or mutate its held image. */
    for(uint64_t n=2;n<=10000;++n) {write(mailbox.writing(),n);mailbox.publish();}
    verify(*held);assert(held->sequence==1 && mailbox.dropped()==9998);
    mailbox.release();
    held=mailbox.acquire();assert(held && held->sequence==10000);verify(*held);mailbox.release();
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for(uint64_t n=10001;n<=100000;++n) {write(mailbox.writing(),n);mailbox.publish();}
        done.store(true,std::memory_order_release);
    });
    uint64_t last=10000;
    while(last<100000) {
        if(const Packet *p=mailbox.acquire()) {
            verify(*p);assert(p->sequence>last);last=p->sequence;
            std::this_thread::yield();verify(*p);mailbox.release();
        } else {
            /* An empty queue can race with the final publication; retry. */
            std::this_thread::yield();
        }
    }
    producer.join();assert(done.load(std::memory_order_acquire));
    puts("latest-frame mailbox: stalled consumer, bounded reuse and immutable concurrent packets passed");
}
