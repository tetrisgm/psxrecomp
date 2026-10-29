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
#include <cstdlib>
#include <new>

static thread_local bool count_allocations=false;
static thread_local size_t allocations=0;
void* operator new(size_t n) {
    if(count_allocations)++allocations;
    if(void* p=std::malloc(n?n:1))return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}


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
    std::atomic<unsigned> requests{0},stale_requests{0};
    std::atomic<unsigned> controls{0};
    std::atomic<unsigned> resets{0};
    bool arm_old_tick=false;
    uint64_t tick_after=0;
    FrameOwner *owner=nullptr;
};
static uint64_t owner_clock() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
static uint64_t tick(void *user,uint64_t now) {
    auto &state=*static_cast<OwnerTest *>(user);
    assert(frame_owner_is_thread());
    if(state.tick_after) assert(state.last.load(std::memory_order_acquire)>=state.tick_after);
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
        if(state.arm_old_tick) frame_owner_tick_at(state.owner,1);
    }
    assert(packet->initial_bytes==8);
    assert(*static_cast<const uint64_t *>(packet->initial)==packet->sequence);
    if(packet->sequence==10000) frame_owner_tick_at(state.owner,owner_clock()+1000000);
    state.last.store(packet->sequence,std::memory_order_release);
}
static void reset_timeline(void *user,uint64_t epoch) {
    auto &state=*static_cast<OwnerTest *>(user);
    assert(frame_owner_is_thread() && epoch==2);
    state.resets.fetch_add(1,std::memory_order_release);
}
static void request_done(void *user,const void *payload,size_t bytes,int stale) {
    auto &state=*static_cast<OwnerTest *>(user);
    assert(frame_owner_is_thread());
    assert(bytes==sizeof(uint64_t));
    const auto expected=*static_cast<const uint64_t *>(payload);
    if(stale) ++state.stale_requests;
    else assert(state.last.load(std::memory_order_acquire)>=expected);
    state.requests.fetch_add(1,std::memory_order_release);
}
static void control_done(void *user,const void *payload,size_t bytes,int stale) {
    auto &state=*static_cast<OwnerTest *>(user);
    assert(frame_owner_is_thread() && !stale && bytes==sizeof(uint64_t));
    assert(state.last.load(std::memory_order_acquire)==*static_cast<const uint64_t *>(payload));
    state.controls.fetch_add(1,std::memory_order_release);
}
static void test_attachments() {
    struct Owned { int* destroyed; uint64_t value; };
    const auto dispose=[](void* data) {
        auto* owned=static_cast<Owned*>(data); ++*owned->destroyed; delete owned;
    };
    int destroyed=0;
    auto* queue=frame_packet_create(sizeof(Owned),16);
    assert(queue);
    assert(frame_packet_begin(queue,1,nullptr,0));
    auto* rejected=new Owned{&destroyed,0};
    assert(!frame_packet_attach(queue,rejected,sizeof(Owned)+1,dispose));
    delete rejected; // rejected attachment still belongs to caller
    assert(!frame_packet_publish(queue));
    assert(frame_packet_begin(queue,1,nullptr,0));
    assert(frame_packet_attach(queue,new Owned{&destroyed,1},sizeof(Owned),dispose));
    assert(frame_packet_publish(queue));
    FramePacketView held{};
    assert(frame_packet_acquire(queue,&held));
    for(uint64_t n=2;n<=100;++n) {
        assert(frame_packet_begin(queue,n,nullptr,0));
        assert(frame_packet_attach(queue,new Owned{&destroyed,n},sizeof(Owned),dispose));
        assert(frame_packet_publish(queue));
        assert(static_cast<const Owned*>(held.attachment)->value==1);
    }
    assert(destroyed==97); // three bounded slots, including the held lease
    frame_packet_invalidate(queue);
    assert(static_cast<const Owned*>(held.attachment)->value==1);
    frame_packet_release(queue);
    frame_packet_destroy(queue);
    assert(destroyed==100);
}
static void test_optional_batches() {
    auto* queue=frame_packet_create(0,96);assert(queue);
    assert(frame_packet_begin(queue,1,nullptr,0));
    auto* first=static_cast<uint64_t*>(frame_packet_append(queue,1,0,8));
    assert(first);*first=11;
    alignas(RT_ALIGN) unsigned char batch[64]{};
    auto* a=reinterpret_cast<RtCmd*>(batch);
    auto* b=reinterpret_cast<RtCmd*>(batch+32);
    *a={32,2,0,8,100};*b={0,3,0,8,101};
    *reinterpret_cast<uint64_t*>(a+1)=22;
    *reinterpret_cast<uint64_t*>(b+1)=33;
    assert(!frame_packet_append_records(queue,batch,sizeof batch)); // malformed suffix rejects prefix too
    b->size=32;
    assert(frame_packet_append_records(queue,batch,sizeof batch));
    assert(!frame_packet_append_records(queue,batch,32)); // optional overflow preserves main frame
    std::memset(batch,0,sizeof batch);
    assert(frame_packet_publish(queue));
    assert(frame_packet_begin(queue,2,nullptr,0));
    assert(frame_packet_append(queue,4,0,8));
    frame_packet_cancel(queue);
    assert(!frame_packet_publish(queue));
    FramePacketView packet{};
    assert(frame_packet_acquire(queue,&packet) && packet.sequence==1 && packet.record_bytes==96);
    for(unsigned n=0;n<3;++n) {
        const auto* record=reinterpret_cast<const RtCmd*>(static_cast<const unsigned char*>(packet.records)+n*32);
        assert(record->op==n+1 && record->seq==n && *reinterpret_cast<const uint64_t*>(record+1)==11*(n+1));
    }
    frame_packet_release(queue);frame_packet_destroy(queue);
}

static void rewind_packets() {
    auto *q=frame_packet_create(1024,1024);assert(q);
    frame_packet_history_configure(q,3,8192);
    int disposed=0;
    struct Attachment { int *disposed;int value; };
    auto dispose=[](void *v) { auto *a=static_cast<Attachment*>(v);++*a->disposed;delete a; };
    for(unsigned i=1;i<=5;i++) {
        assert(frame_packet_begin(q,i,&i,sizeof i));
        assert(frame_packet_attach(q,new Attachment{&disposed,int(i)},sizeof(Attachment),dispose));
        auto *value=static_cast<unsigned*>(frame_packet_append(q,1,0,sizeof i));assert(value);*value=i*10;
        assert(frame_packet_publish(q));
    }
    assert(!frame_packet_history_has(q,2) && frame_packet_history_has(q,3));
    FramePacketView held{};assert(frame_packet_acquire(q,&held));
    const auto *held_attachment=static_cast<const Attachment*>(held.attachment);
    frame_packet_invalidate(q);
    assert(!frame_packet_current(q,&held) && held_attachment->value==5);
    assert(frame_packet_history_replay(q,3,6));
    frame_packet_history_branch(q,3);
    assert(!frame_packet_history_has(q,4) && held_attachment->value==5);
    frame_packet_release(q);
    FramePacketView reverse{};assert(frame_packet_acquire(q,&reverse));
    assert(reverse.sequence==6 && frame_packet_current(q,&reverse));
    assert(*static_cast<const unsigned*>(reverse.initial)==3);
    assert(static_cast<const Attachment*>(reverse.attachment)->value==3);
    auto *record=static_cast<const RtCmd*>(reverse.records);
    assert(*reinterpret_cast<const unsigned*>(record+1)==30);
    frame_packet_history_configure(q,1,1); // byte overflow never publishes a partial packet
    assert(!frame_packet_history_has(q,3));
    assert(static_cast<const Attachment*>(reverse.attachment)->value==3);
    frame_packet_release(q);
    assert(frame_packet_begin(q,7,nullptr,0));assert(frame_packet_publish(q));
    assert(frame_packet_history_bytes(q)==0 && !frame_packet_history_has(q,7));
    frame_packet_destroy(q);assert(disposed==5);
}

static void pooled_frame_allocations() {
    auto* q=frame_packet_create(256,4096);assert(q);
    frame_packet_history_configure(q,8,1024*1024);
    std::array<unsigned char,256> seed{};
    for(unsigned n=0;n<2000;++n) {
        if(n==100) {allocations=0;count_allocations=true;}
        assert(frame_packet_begin(q,n,seed.data(),seed.size()));
        auto* bytes=frame_packet_append(q,1,0,2048);assert(bytes);
        std::memset(bytes,int(n&255),2048);
        assert(frame_packet_publish(q));
        FramePacketView view{};assert(frame_packet_acquire(q,&view));
        assert(view.sequence==n);frame_packet_release(q);
    }
    count_allocations=false;assert(allocations==0);
    frame_packet_destroy(q);
}
int main() {
    pooled_frame_allocations();
    rewind_packets();
    test_attachments();
    test_optional_batches();
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
    uint64_t requested=10000;
    for(unsigned n=0;n<62;++n)
        assert(frame_owner_request(owner,requested,request_done,&state,&requested,sizeof requested));
    uint64_t previous=1;
    assert(frame_owner_control(owner,2,control_done,&state,&previous,sizeof previous));
    assert(frame_owner_control(owner,10001,control_done,&state,&requested,sizeof requested));
    assert(!frame_owner_request(owner,requested,request_done,&state,&requested,sizeof requested));
    requested=UINT64_MAX; // accepted requests own their bytes
    state.unblock.store(true,std::memory_order_release);
    while(frame_owner_completed(owner)<2) std::this_thread::yield();
    assert(state.last.load(std::memory_order_acquire)==10000);
    while(state.requests.load(std::memory_order_acquire)<62 ||
          state.controls.load(std::memory_order_acquire)<2) std::this_thread::yield();
    assert(state.stale_requests==0);
    /* Timed presentation work continues while the guest publishes nothing. */
    while(state.ticks.load(std::memory_order_acquire)<2) std::this_thread::yield();
    frame_owner_stop(owner);
    assert(state.acquire==2 && state.release==2);
    {
        OwnerTest reset;
        reset.arm_old_tick=true;
        const FrameOwnerConfig reset_config{8,4096,execute,context,&reset,tick,owner_clock,reset_timeline};
        reset.owner=frame_owner_start(&reset_config);assert(reset.owner);
        uint64_t seq=1;
        assert(frame_owner_begin(reset.owner,seq,&seq,sizeof seq));
        assert(frame_owner_publish(reset.owner));
        while(!reset.entered.load(std::memory_order_acquire)) std::this_thread::yield();
        seq=2;
        assert(frame_owner_begin(reset.owner,seq,&seq,sizeof seq));
        assert(frame_owner_publish(reset.owner));
        assert(frame_owner_request(reset.owner,seq,request_done,&reset,&seq,sizeof seq));
        frame_owner_invalidate(reset.owner);
        reset.unblock.store(true,std::memory_order_release);
        while(reset.requests.load(std::memory_order_acquire)<1) std::this_thread::yield();
        assert(reset.stale_requests==1 && reset.last==1);
        // Idle requests still complete without another guest frame.
        seq=0;
        assert(frame_owner_request(reset.owner,seq,request_done,&reset,&seq,sizeof seq));
        while(reset.requests.load(std::memory_order_acquire)<2) std::this_thread::yield();
        assert(reset.stale_requests==1 && reset.resets==1 && reset.ticks==0);
        frame_owner_stop(reset.owner);
    }
    /* The C renderer's owned packet carries its seed independently of all
     * skipped packets. Neither caller buffers nor later appends may mutate
     * the consumer's current packet. Overflow rejects a whole packet. */
    {
        OwnerTest priority;
        priority.arm_old_tick=true;priority.tick_after=2;
        const FrameOwnerConfig priority_config{8,4096,execute,context,&priority,tick,owner_clock};
        priority.owner=frame_owner_start(&priority_config);assert(priority.owner);
        uint64_t seq=1;
        assert(frame_owner_begin(priority.owner,seq,&seq,sizeof seq));
        assert(frame_owner_publish(priority.owner));
        while(!priority.entered.load(std::memory_order_acquire)) std::this_thread::yield();
        seq=2;
        assert(frame_owner_begin(priority.owner,seq,&seq,sizeof seq));
        assert(frame_owner_publish(priority.owner));
        priority.unblock.store(true,std::memory_order_release);
        while(priority.ticks.load(std::memory_order_acquire)<2) std::this_thread::yield();
        frame_owner_stop(priority.owner);
    }
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
