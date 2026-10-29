#include "guest_wait_trace.h"
#include "frame_packet.h"
#include "frame_mailbox.h"
#include <cstring>
#include <vector>
#include <memory>
#include <deque>

namespace {
std::atomic<uint64_t> allocation_events{0};
/* Match RtCmd's 16-byte payload alignment, including after vector growth. */
struct alignas(RT_ALIGN) Block {
    unsigned char bytes[RT_ALIGN];
    /* Payload is written before publication; vector growth must not zero it. */
    Block() noexcept {}
};
/* Producer-owned arena. Metadata and backing bytes are allocated only when
 * creating/configuring the queue. Frame growth splits/coalesces free spans;
 * it never enters the host allocator. Leases keep the backing arena alive. */
struct Arena {
    struct Span {size_t at=0,count=0;int next=-1;};
    std::vector<Block> backing;
    std::vector<Span> spans;
    int head=0,spare=1;
    Arena(size_t bytes,size_t slots):backing(bytes/RT_ALIGN+(bytes%RT_ALIGN!=0)),spans(slots*4+32) {
        spans[0]={0,backing.size(),-1};
        for(size_t i=1;i<spans.size();++i)spans[i].next=i+1<spans.size()?int(i+1):-1;
        allocation_events+=2;
    }
    Block* take(size_t count) {
        if(!count)return nullptr;
        int previous=-1;
        for(int i=head;i!=-1;previous=i,i=spans[i].next) {
            auto& s=spans[i];if(s.count<count)continue;
            Block* result=backing.data()+s.at;s.at+=count;s.count-=count;
            if(!s.count) {if(previous<0)head=s.next;else spans[previous].next=s.next;s.next=spare;spare=i;}
            return result;
        }
        return nullptr;
    }
    void put(Block* data,size_t count) {
        if(!data || !count)return;
        const size_t at=size_t(data-backing.data());
        int previous=-1,next=head;
        while(next!=-1 && spans[next].at<at) {previous=next;next=spans[next].next;}
        if(previous!=-1 && spans[previous].at+spans[previous].count==at) {
            spans[previous].count+=count;
        } else {
            if(spare==-1)std::terminate(); /* At most two allocations per slot. */
            int node=spare;spare=spans[node].next;spans[node]={at,count,next};
            if(previous<0)head=node;else spans[previous].next=node;
            previous=node;
        }
        if(next!=-1 && spans[previous].at+spans[previous].count==spans[next].at) {
            spans[previous].count+=spans[next].count;spans[previous].next=spans[next].next;
            spans[next].next=spare;spare=next;
        }
    }
};
struct Buffer {
    std::shared_ptr<Arena> arena,target;
    Block* pointer=nullptr;
    size_t length=0,room=0;
    Block* data() const {return pointer;}
    size_t size() const {return length;}
    size_t capacity() const {return room;}
    bool resize(size_t n) {
        if(n>room) {
            size_t capacity=std::max(n,room*2);
            Block* next=target->take(capacity);
            if(!next && capacity!=n) {capacity=n;next=target->take(n);}
            if(!next)return false;
            if(length)std::memcpy(next,pointer,length*RT_ALIGN);
            if(pointer)arena->put(pointer,room);
            pointer=next;room=capacity;arena=target;
        }
        length=n;return true;
    }
    void clear() {
        if(pointer)arena->put(pointer,room);
        pointer=nullptr;length=room=0;arena.reset();
    }
    /* No free-list mutation on destruction: a queue owns all slot envelopes
     * until shutdown; later lease destruction only drops arena references. */
};
struct Storage {
    size_t attachment_bytes=0;
    size_t initial_bytes=0,record_bytes=0;
    Buffer initial,records;
    void *attachment=nullptr;
    void (*dispose)(void*)=nullptr;
    void clear_attachment() {
        if(dispose) dispose(attachment);
        attachment=nullptr;dispose=nullptr;
    }
    ~Storage() { clear_attachment(); }
    size_t cost() const { return sizeof(Storage)+(initial.capacity()+records.capacity())*RT_ALIGN+attachment_bytes; }
};
struct Packet {
    uint64_t sequence=0,epoch=0;
    std::shared_ptr<Storage> data;
};
size_t blocks(size_t bytes) { return bytes/RT_ALIGN+(bytes%RT_ALIGN!=0); }
}
struct FramePacketQueue {
    PsxFrameMailbox<Packet> mailbox;
    size_t initial_limit,record_limit;
    bool open=false,valid=false;
    size_t history_limit=0,history_bytes=0,history_depth=0;
    std::vector<Packet> history;
    std::vector<std::shared_ptr<Storage>> storage_pool;
    std::shared_ptr<Arena> arena;
    std::vector<std::shared_ptr<Arena>> arenas;
    uint32_t command_sequence=0;
    std::atomic<uint64_t> epoch{1};
    void reclaim_spares() {
        for(auto& item:storage_pool)if(item.use_count()==1) {
            item->clear_attachment();item->attachment_bytes=0;
            item->initial.clear();item->records.clear();
        }
    }
    void prepare(size_t slots,size_t history) {
        if(initial_limit>SIZE_MAX-record_limit || initial_limit+record_limit>(SIZE_MAX-history)/4)throw std::bad_alloc();
        const size_t bytes=history+4*(initial_limit+record_limit);
        if(!arena || bytes/RT_ALIGN>arena->backing.size() || slots*4+32>arena->spans.size()) {
            arena=std::make_shared<Arena>(bytes,slots);arenas.push_back(arena);
        }
        storage_pool.reserve(slots);
        while(storage_pool.size()<slots) {++allocation_events;storage_pool.push_back(std::make_shared<Storage>());}
    }
    FramePacketQueue(size_t initial,size_t records):initial_limit(initial),record_limit(records) {prepare(16,0);}
};
extern "C" FramePacketQueue *frame_packet_create(size_t initial,size_t records) {
    if(initial>SIZE_MAX-RT_ALIGN || records>UINT32_MAX-RT_ALIGN) return nullptr;
    try { return new FramePacketQueue(initial,records); } catch(...) { return nullptr; }
}
extern "C" void frame_packet_destroy(FramePacketQueue *q) { delete q; }
extern "C" int frame_packet_reserve_initial(FramePacketQueue *q,uint64_t sequence,size_t bytes,void **initial) {
    guest_wait_trace::Scope trace(guest_wait_trace::Reserve);
    if(!initial) return 0;
    *initial=nullptr;
    if(!q || q->open) return 0;
    q->open=true;q->valid=false;q->command_sequence=0;
    auto &slot=q->mailbox.writing();
    try {
        if(!slot.data || slot.data.use_count()!=2) {
            slot.data.reset();
            for(auto& candidate:q->storage_pool) if(candidate.unique()) {slot.data=candidate;break;}
            if(!slot.data)return 0;
        }
    }
    catch(...) { return 0; }
    auto &p=*slot.data;
    p.initial.target=p.records.target=q->arena;
    p.clear_attachment();p.attachment_bytes=0;
    if(bytes>q->initial_limit) return 0;
    if(!p.initial.resize(blocks(bytes))) {
        q->reclaim_spares();if(!p.initial.resize(blocks(bytes)))return 0;
    }
    slot.sequence=sequence;slot.epoch=q->epoch.load(std::memory_order_relaxed);
    p.initial_bytes=bytes;p.record_bytes=0;
    q->valid=true;
    *initial=p.initial.data();
    return 1;
}
extern "C" int frame_packet_begin(FramePacketQueue *q,uint64_t sequence,const void *initial,size_t bytes) {
    void* destination=nullptr;
    if(!frame_packet_reserve_initial(q,sequence,bytes,&destination)) return 0;
    if(bytes && !initial) { q->valid=false;return 0; }
    if(bytes) std::memcpy(destination,initial,bytes);
    return 1;
}
extern "C" int frame_packet_attach(FramePacketQueue *q,void *attachment,size_t bytes,
                                     void (*dispose)(void*)) {
    if(!q || !q->open || !q->valid) return 0;
    auto &p=*q->mailbox.writing().data;
    if(!attachment || !dispose || p.attachment || bytes>q->initial_limit-p.initial_bytes) {
        q->valid=false;return 0;
    }
    p.attachment=attachment;p.dispose=dispose;p.attachment_bytes=bytes;
    return 1;
}
extern "C" void *frame_packet_append(FramePacketQueue *q,uint16_t op,uint16_t flags,uint32_t bytes) {
    if(!q || !q->open || !q->valid) return nullptr;
    auto &p=*q->mailbox.writing().data;
    const uint64_t size=(uint64_t(sizeof(RtCmd))+bytes+RT_ALIGN-1)&~uint64_t(RT_ALIGN-1);
    if(!op || op==UINT16_MAX || size>q->record_limit-p.record_bytes) {
        q->valid=false;return nullptr;
    }
    const size_t offset=p.record_bytes/RT_ALIGN;
    const size_t needed=offset+size/RT_ALIGN;
    if(p.records.size()<needed && !p.records.resize(needed)) {
        q->reclaim_spares();if(!p.records.resize(needed)) {q->valid=false;return nullptr;}
    }
    auto *cmd=reinterpret_cast<RtCmd *>(p.records.data()+offset);
    *cmd=RtCmd{uint32_t(size),op,flags,bytes,q->command_sequence++};
    /* Retain allocated storage between frames; only alignment padding needs
     * clearing. The producer fills the payload, so clearing it first would
     * add another full draw-stream write on every guest frame. */
    std::memset(reinterpret_cast<unsigned char*>(cmd+1)+bytes,0,size-sizeof(RtCmd)-bytes);
    p.record_bytes+=size;
    return cmd+1;
}
extern "C" int frame_packet_publish(FramePacketQueue *q) {
    if(!q || !q->open) return 0;
    q->open=false;
    if(!q->valid) return 0;
    if(q->history_limit) {
        const auto &packet=q->mailbox.writing();
        const size_t cost=packet.data->cost()+sizeof(Packet);
        while(!q->history.empty() && (cost>q->history_limit-q->history_bytes || q->history.size()>=q->history_depth)) {
            q->history_bytes-=q->history.front().data->cost()+sizeof(Packet);
            q->history.erase(q->history.begin());
        }
        if(cost<=q->history_limit) {
            try { q->history.push_back(packet);q->history_bytes+=cost; } catch(...) {}
        }
    }
    q->mailbox.publish();q->valid=false;
    return 1;
}
extern "C" int frame_packet_append_records(FramePacketQueue *q,const void *records,size_t bytes) {
    if(!q || !q->open || !q->valid || (bytes && !records)) return 0;
    auto& p=*q->mailbox.writing().data;
    if(bytes>q->record_limit-p.record_bytes || bytes%RT_ALIGN) return 0;
    const auto* source=static_cast<const unsigned char*>(records);
    for(size_t at=0;at<bytes;) {
        if(bytes-at<sizeof(RtCmd)) return 0;
        RtCmd command;std::memcpy(&command,source+at,sizeof command);
        if(command.size<sizeof command || command.size%RT_ALIGN || command.size>bytes-at ||
           command.payload>command.size-sizeof command || !command.op || command.op==UINT16_MAX) return 0;
        at+=command.size;
    }
    const size_t offset=p.record_bytes/RT_ALIGN,needed=offset+bytes/RT_ALIGN;
    if(p.records.size()<needed && !p.records.resize(needed)) {
        q->reclaim_spares();if(!p.records.resize(needed))return 0;
    }
    if(bytes) std::memcpy(p.records.data()+offset,source,bytes);
    for(size_t at=offset;at<needed;) {
        auto* command=reinterpret_cast<RtCmd*>(p.records.data()+at);
        command->seq=q->command_sequence++;at+=command->size/RT_ALIGN;
    }
    p.record_bytes+=bytes;
    return 1;
}
extern "C" void frame_packet_cancel(FramePacketQueue *q) {
    if(!q || !q->open) return;
    q->valid=false;q->open=false;
    if(q->mailbox.writing().data && q->mailbox.writing().data.use_count()==2) q->mailbox.writing().data->clear_attachment();
}
extern "C" int frame_packet_acquire(FramePacketQueue *q,FramePacketView *view) {
    if(!q || !view) return 0;
    const auto *p=q->mailbox.acquire();
    if(!p) { *view={};return 0; }
    const auto &d=*p->data;
    *view={p->sequence,p->epoch,d.initial.data(),d.initial_bytes,d.records.data(),d.record_bytes,d.attachment,&p->data};
    return 1;
}
extern "C" void frame_packet_release(FramePacketQueue *q) { q->mailbox.release(); }
extern "C" uint64_t frame_packet_replaced(const FramePacketQueue *q) { return q->mailbox.dropped(); }
extern "C" int frame_packet_pending(const FramePacketQueue *q) { return q->mailbox.has_pending(); }
extern "C" void frame_packet_invalidate(FramePacketQueue *q) {
    q->epoch.fetch_add(1,std::memory_order_release);
    q->valid=false;q->open=false;
    q->mailbox.discard_pending();
}
extern "C" int frame_packet_current(const FramePacketQueue *q,const FramePacketView *view) {
    return view && view->epoch==q->epoch.load(std::memory_order_acquire);
}

/* History is producer-owned. Shared storage keeps leases immutable across
 * eviction/reset; replay publishes a new envelope in the current epoch. */
extern "C" void frame_packet_history_configure(FramePacketQueue *q,size_t depth,size_t bytes) {
    if(!q)return;
    q->history.clear();q->history_bytes=0;q->history_depth=depth;q->history_limit=depth?bytes:0;
    try {q->history.reserve(depth);q->prepare(std::max<size_t>(16,depth+16),depth?bytes:0);}
    catch(...) {q->history_depth=0;q->history_limit=0;}
}
extern "C" size_t frame_packet_history_bytes(const FramePacketQueue *q) { return q?q->history_bytes:0; }
extern "C" int frame_packet_history_has(const FramePacketQueue *q,uint64_t sequence) {
    if(!q)return 0;
    for(const auto &p:q->history) if(p.sequence==sequence)return 1;
    return 0;
}
extern "C" int frame_packet_history_replay(FramePacketQueue *q,uint64_t saved,uint64_t sequence) {
    if(!q || q->open)return 0;
    for(size_t i=0;i<q->history.size();i++) if(q->history[i].sequence==saved) {
        auto &slot=q->mailbox.writing();slot=q->history[i];
        slot.sequence=sequence;slot.epoch=q->epoch.load(std::memory_order_acquire);
        q->mailbox.publish();return 1;
    }
    return 0;
}
extern "C" void frame_packet_history_branch(FramePacketQueue *q,uint64_t sequence) {
    if(!q)return;
    while(!q->history.empty() && q->history.back().sequence>sequence) {
        q->history_bytes-=q->history.back().data->cost()+sizeof(Packet);q->history.pop_back();
    }
}

struct FramePacketLease { std::shared_ptr<Storage> data; FramePacketView view; };
extern "C" FramePacketLease *frame_packet_retain(const FramePacketView *view) {
    if(!view || !view->lease) return nullptr;
    try {
        auto *lease=new FramePacketLease{*static_cast<const std::shared_ptr<Storage>*>(view->lease),*view};
        lease->view.lease=&lease->data;return lease;
    } catch(...) {return nullptr;}
}
extern "C" const FramePacketView *frame_packet_lease_view(const FramePacketLease *lease) {return lease?&lease->view:nullptr;}
extern "C" void frame_packet_lease_release(FramePacketLease *lease) {delete lease;}

extern "C" uint64_t frame_packet_allocation_events(void) {return allocation_events.load(std::memory_order_relaxed);}
