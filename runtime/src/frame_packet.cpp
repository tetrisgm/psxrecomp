#include "frame_packet.h"
#include "frame_mailbox.h"
#include <cstring>
#include <vector>

namespace {
/* Match RtCmd's 16-byte payload alignment, including after vector growth. */
struct alignas(RT_ALIGN) Block { unsigned char bytes[RT_ALIGN]; };
struct Packet {
    uint64_t sequence=0,epoch=0;
    size_t initial_bytes=0,record_bytes=0;
    std::vector<Block> initial,records;
};
size_t blocks(size_t bytes) { return bytes/RT_ALIGN+(bytes%RT_ALIGN!=0); }
}
struct FramePacketQueue {
    PsxFrameMailbox<Packet> mailbox;
    size_t initial_limit,record_limit;
    bool open=false,valid=false;
    uint32_t command_sequence=0;
    std::atomic<uint64_t> epoch{1};
    FramePacketQueue(size_t initial,size_t records):initial_limit(initial),record_limit(records) {}
};
extern "C" FramePacketQueue *frame_packet_create(size_t initial,size_t records) {
    if(initial>SIZE_MAX-RT_ALIGN || records>UINT32_MAX-RT_ALIGN) return nullptr;
    try { return new FramePacketQueue(initial,records); } catch(...) { return nullptr; }
}
extern "C" void frame_packet_destroy(FramePacketQueue *q) { delete q; }
extern "C" int frame_packet_begin(FramePacketQueue *q,uint64_t sequence,const void *initial,size_t bytes) {
    if(!q || q->open) return 0;
    q->open=true;q->valid=false;q->command_sequence=0;
    if(bytes>q->initial_limit || (bytes && !initial)) return 0;
    auto &p=q->mailbox.writing();
    try { p.initial.resize(blocks(bytes)); } catch(...) { return 0; }
    if(bytes) std::memcpy(p.initial.data(),initial,bytes);
    p.sequence=sequence;p.epoch=q->epoch.load(std::memory_order_relaxed);
    p.initial_bytes=bytes;p.record_bytes=0;
    p.records.clear();q->valid=true;
    return 1;
}
extern "C" void *frame_packet_append(FramePacketQueue *q,uint16_t op,uint16_t flags,uint32_t bytes) {
    if(!q || !q->open || !q->valid) return nullptr;
    auto &p=q->mailbox.writing();
    const uint64_t size=(uint64_t(sizeof(RtCmd))+bytes+RT_ALIGN-1)&~uint64_t(RT_ALIGN-1);
    if(!op || op==UINT16_MAX || size>q->record_limit-p.record_bytes) {
        q->valid=false;return nullptr;
    }
    const size_t offset=p.records.size();
    try { p.records.resize(offset+size/RT_ALIGN); } catch(...) { q->valid=false;return nullptr; }
    auto *cmd=reinterpret_cast<RtCmd *>(p.records.data()+offset);
    *cmd=RtCmd{uint32_t(size),op,flags,bytes,q->command_sequence++};
    p.record_bytes+=size;
    return cmd+1;
}
extern "C" int frame_packet_publish(FramePacketQueue *q) {
    if(!q || !q->open) return 0;
    q->open=false;
    if(!q->valid) return 0;
    q->mailbox.publish();q->valid=false;
    return 1;
}
extern "C" int frame_packet_acquire(FramePacketQueue *q,FramePacketView *view) {
    if(!q || !view) return 0;
    const auto *p=q->mailbox.acquire();
    if(!p) { *view={};return 0; }
    *view={p->sequence,p->epoch,p->initial.data(),p->initial_bytes,p->records.data(),p->record_bytes};
    return 1;
}
extern "C" void frame_packet_release(FramePacketQueue *q) { q->mailbox.release(); }
extern "C" uint64_t frame_packet_replaced(const FramePacketQueue *q) { return q->mailbox.dropped(); }
extern "C" void frame_packet_invalidate(FramePacketQueue *q) {
    q->epoch.fetch_add(1,std::memory_order_release);
    q->valid=false;q->open=false;
    q->mailbox.discard_pending();
}
extern "C" int frame_packet_current(const FramePacketQueue *q,const FramePacketView *view) {
    return view && view->epoch==q->epoch.load(std::memory_order_acquire);
}
