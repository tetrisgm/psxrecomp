#include "frame_packet.h"
#include "frame_mailbox.h"
#include <cstring>
#include <vector>
#include <memory>
#include <deque>
#include <chrono>

namespace {
thread_local bool timing_enabled=false;
thread_local FramePacketProducerTiming timing={};
struct GrowthTiming {
    std::chrono::steady_clock::time_point start;
    bool enabled;
    GrowthTiming():enabled(timing_enabled) { if(enabled)start=std::chrono::steady_clock::now(); }
    ~GrowthTiming() { if(enabled) {
        timing.growth_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-start).count();
        ++timing.growth_count;
    } }
};
/* Match RtCmd's 16-byte payload alignment, including after vector growth. */
struct alignas(RT_ALIGN) Block {
    unsigned char bytes[RT_ALIGN];
    /* Every published seed/payload byte is supplied by the producer and
     * record padding is cleared explicitly. History retains old storage,
     * so vector growth can occur every tick: value-initializing each new
     * block would write the complete stream once before writing it again. */
    Block() noexcept {}
};
struct Storage {
    size_t attachment_bytes=0;
    size_t initial_bytes=0,record_bytes=0;
    std::vector<Block> initial,records;
    void *attachment=nullptr;
    void (*dispose)(void*)=nullptr;
    void clear_attachment() {
        if(dispose) {
            const auto start=timing_enabled?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
            dispose(attachment);
            if(timing_enabled)timing.cleanup_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now()-start).count();
        }
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
void grow(std::vector<Block>& data,size_t needed) {
    if(data.size()>=needed)return;
    if(data.capacity()>=needed) { data.resize(needed);return; }
    GrowthTiming watch;
    const size_t previous=data.capacity();
    data.resize(needed);
    if(timing_enabled)timing.growth_bytes+=(data.capacity()-previous)*RT_ALIGN;
}
}
extern "C" void frame_packet_producer_timing(int enable,FramePacketProducerTiming *out) {
    timing_enabled=enable!=0;
    if(out)*out=timing;
}
struct FramePacketQueue {
    PsxFrameMailbox<Packet> mailbox;
    size_t initial_limit,record_limit;
    bool open=false,valid=false;
    size_t history_limit=0,history_bytes=0,history_depth=0;
    std::deque<Packet> history;
    uint32_t command_sequence=0;
    std::atomic<uint64_t> epoch{1};
    FramePacketQueue(size_t initial,size_t records):initial_limit(initial),record_limit(records) {}
};
extern "C" FramePacketQueue *frame_packet_create(size_t initial,size_t records) {
    if(initial>SIZE_MAX-RT_ALIGN || records>UINT32_MAX-RT_ALIGN) return nullptr;
    try { return new FramePacketQueue(initial,records); } catch(...) { return nullptr; }
}
extern "C" void frame_packet_destroy(FramePacketQueue *q) { delete q; }
extern "C" int frame_packet_reserve_initial(FramePacketQueue *q,uint64_t sequence,size_t bytes,void **initial) {
    if(!initial) return 0;
    *initial=nullptr;
    if(!q || q->open) return 0;
    q->open=true;q->valid=false;q->command_sequence=0;
    auto &slot=q->mailbox.writing();
    try { if(!slot.data || !slot.data.unique()) { GrowthTiming watch;slot.data=std::make_shared<Storage>(); } }
    catch(...) { return 0; }
    auto &p=*slot.data;
    p.clear_attachment();p.attachment_bytes=0;
    if(bytes>q->initial_limit) return 0;
    try { grow(p.initial,blocks(bytes)); } catch(...) { return 0; }
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
    try { grow(p.records,needed); } catch(...) { q->valid=false;return nullptr; }
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
            q->history.pop_front();
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
    try { grow(p.records,needed); } catch(...) { return 0; }
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
    if(q->mailbox.writing().data && q->mailbox.writing().data.unique()) q->mailbox.writing().data->clear_attachment();
}
extern "C" int frame_packet_acquire(FramePacketQueue *q,FramePacketView *view) {
    if(!q || !view) return 0;
    const auto *p=q->mailbox.acquire();
    if(!p) { *view={};return 0; }
    const auto &d=*p->data;
    *view={p->sequence,p->epoch,d.initial.data(),d.initial_bytes,d.records.data(),d.record_bytes,d.attachment};
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
