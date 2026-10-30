#include "packet_presenter.h"
#include "packet_delivery_clock.h"
#include <cstdlib>
#include <array>
#include "packet_interpolation_stream.h"
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdio>

namespace {
FramePacketLease *current=nullptr;
FramePacketLease *previous=nullptr;
#ifndef PSX_NO_DEBUG_TOOLS
std::atomic<int> requested_sample{-1};
std::atomic<unsigned> sampled_requests{0};
#endif
uint64_t start=0,period=0,index=0,last_sequence=0,last_epoch=0,last_guest=0,display_origin=0;

unsigned rate=0;
std::atomic<uint64_t> submitted{0},generated{0},endpoints{0};
std::atomic<uint64_t> delivery_late{0},delivery_overflow{0},delivery_resyncs{0};
std::atomic<double> delay_ms{0},interval_ms{0},motion_sum_ms{0},motion_sum_sq_ms{0};
std::atomic<uint64_t> motion_steps{0};
uint64_t last_motion=0;
}
extern "C" void packet_presenter_trace(unsigned event,uint64_t begin,uint64_t end) {
#ifndef PSX_NO_DEBUG_TOOLS
    static FILE *file=[] {const char *p=std::getenv("PSX_PRESENT_SLOT_TRACE");
        FILE *f=p&&*p?std::fopen(p,"wb"):nullptr;
        if(f){std::setvbuf(f,nullptr,_IOFBF,256*1024);std::fprintf(f,"event,begin_ns,end_ns,sequence,period_ns,start_ns,due_ns,snapped\n");}return f;}();
    if(file)std::fprintf(file,"%u,%llu,%llu,%llu,%llu,%llu,%llu,%d\n",event,
        (unsigned long long)begin,(unsigned long long)end,(unsigned long long)last_sequence,
        (unsigned long long)period,(unsigned long long)start,
        (unsigned long long)(rate?packet_interpolation_deadline(display_origin,index,rate,1):0),
        packet_interpolation_stream_snapped());
#else
    (void)event;(void)begin;(void)end;
#endif
}
extern "C" void packet_presenter_reset(void) {
    frame_packet_lease_release(current);current=nullptr;
    frame_packet_lease_release(previous);previous=nullptr;
#ifndef PSX_NO_DEBUG_TOOLS
    requested_sample.store(-1);
#endif
    start=period=index=last_sequence=last_epoch=last_guest=display_origin=last_motion=0;rate=0;
}
extern "C" int packet_presenter_submit(const FramePacketView *packet,uint64_t now,
    double guest_hz,PacketPresenterDraw draw) {
    const unsigned hz=packet_interpolation_rate();
    if(!hz || !packet || !draw || !std::isfinite(guest_hz) || guest_hz<=0) {
        packet_presenter_reset();return 0;
    }
    auto *next=frame_packet_retain(packet);
    if(!next) {packet_presenter_reset();return 0;}
    const uint64_t frames=packet->epoch==last_epoch && packet->sequence>last_sequence?
        packet->sequence-last_sequence:0;
    period=frames && frames<=8 && packet->guest_time_ns>last_guest?
        packet->guest_time_ns-last_guest:0;
    // Pair duration is measured in the guest domain. Arrivals never restart
    // either the mapping or the output grid. The delivery buffer already
    // prepared this pair at its fixed mapped deadline.
    const bool continuous=period && current && rate==hz;
    frame_packet_lease_release(previous);previous=current;current=next;
    last_sequence=packet->sequence;last_epoch=packet->epoch;last_guest=packet->guest_time_ns;
    start=now;rate=hz;interval_ms.store(period/1e6);
    if(!continuous) {
        display_origin=now;index=1;
        draw(frame_packet_lease_view(current),1.0);
    }
    ++submitted;
    return 1;
}
extern "C" uint64_t packet_presenter_tick(uint64_t now,PacketPresenterDraw draw) {
    packet_presenter_trace(0,now,now);
#ifndef PSX_NO_DEBUG_TOOLS
    const int requested=requested_sample.exchange(-1);
    if(requested>=0 && current && draw) {
        if(requested==0 && previous) draw(frame_packet_lease_view(previous),-1.0);
        else draw(frame_packet_lease_view(current),requested/1000.0);
        ++sampled_requests;period=0;return 0;
    }
#endif
    if(!current || !period || !draw || !packet_interpolation_rate()) return 0;
    const uint64_t due=packet_interpolation_deadline(display_origin,index,rate,1);
    if(now<due) return due;
    double t=now<=start?0.0:std::min(1.0,double(now-start)/double(period));
    // Cadence changes or replaced packets must never move displayed guest
    // time backward. Hold the last sampled time until this pair catches up.
    if(last_motion>last_guest-period)
        t=std::max(t,std::min(1.0,double(last_motion-(last_guest-period))/double(period)));
    if(t==0 && previous && !packet_interpolation_stream_snapped())
        draw(frame_packet_lease_view(previous),-1.0);
    else draw(frame_packet_lease_view(current),t);
    const uint64_t motion=packet_interpolation_stream_snapped()?last_guest:
        last_guest-period+uint64_t(t*period);
    if(last_motion && motion>=last_motion) {
        const double step=(motion-last_motion)/1e6;
        motion_sum_ms.store(motion_sum_ms.load()+step);
        motion_sum_sq_ms.store(motion_sum_sq_ms.load()+step*step);
        ++motion_steps;
    }
    last_motion=motion;
    if(t<1) ++generated;else ++endpoints;
    // Skip missed display deadlines: presentation never builds a backlog.
    index=std::max(index+1,uint64_t(double(now-display_origin)*rate/1e9)+1);
    if(packet_interpolation_deadline(display_origin,index,rate,1)<=now)++index;
    if(t==1) {period=0;return 0;}
    return packet_interpolation_deadline(display_origin,index,rate,1);
}

extern "C" int packet_presenter_json(char *out,size_t bytes) {
    unsigned requests=0;
#ifndef PSX_NO_DEBUG_TOOLS
    requests=sampled_requests.load();
#endif
    return std::snprintf(out,bytes,"{\"rate\":%u,\"submitted\":%llu,\"in_between\":%llu,\"endpoints\":%llu,\"sampled_requests\":%u,\"delay_ms\":%.6f,\"interval_ms\":%.6f,\"late_packets\":%llu,\"resyncs\":%llu,\"buffer_overflows\":%llu,\"motion_steps\":%llu,\"motion_sum_ms\":%.9f,\"motion_sum_sq_ms\":%.9f}",
        packet_interpolation_rate(),(unsigned long long)submitted.load(),
        (unsigned long long)generated.load(),(unsigned long long)endpoints.load(),requests,
        delay_ms.load(),interval_ms.load(),(unsigned long long)delivery_late.load(),
        (unsigned long long)delivery_resyncs.load(),(unsigned long long)delivery_overflow.load(),
        (unsigned long long)motion_steps.load(),motion_sum_ms.load(),motion_sum_sq_ms.load());
}
extern "C" int packet_presenter_request_sample(int thousandths) {
#ifndef PSX_NO_DEBUG_TOOLS
    if(thousandths<0 || thousandths>1000) return 0;
    requested_sample.store(thousandths);return 1;
#else
    (void)thousandths;return 0;
#endif
}

extern "C" int packet_presenter_active(void) {return current && packet_interpolation_rate()!=0;}
extern "C" const FramePacketView *packet_presenter_previous(void) {return frame_packet_lease_view(previous);}

namespace {
struct Delivery {FramePacketLease *packet=nullptr;uint64_t due=0;};
std::array<Delivery,8> delivery;
size_t delivery_count=0;
PacketDeliveryClock delivery_clock;
unsigned delivery_rate=0;
bool delivery_started=false;
uint64_t buffer_delay(double hz) {
    // Optional host presentation latency, bounded to 100 ms. Invalid values
    // use one VBlank; this never changes guest pacing or SPU production.
    static const double configured=[] {
        const char *value=std::getenv("PSX_PRESENT_DELAY_MS");
        if(!value || !*value)return -1.0;
        char *end=nullptr;const double ms=std::strtod(value,&end);
        return end && !*end && std::isfinite(ms) && ms>=0?std::min(ms,100.0):-1.0;
    }();
    return configured<0?uint64_t(std::min(1e9/hz,double(PacketDeliveryClock::max_delay_ns))):uint64_t(configured*1e6);
}
}
extern "C" void packet_presenter_buffer_reset(void) {
    for(size_t i=0;i<delivery_count;++i)frame_packet_lease_release(delivery[i].packet);
    delivery_count=0;delivery_clock.reset();delivery_rate=0;delivery_started=false;
}
extern "C" uint64_t packet_presenter_deliver(uint64_t now,PacketPresenterPrepare prepare) {
    while(delivery_count && delivery[0].due<=now) {
        const Delivery item=delivery[0];
        for(size_t i=1;i<delivery_count;++i)delivery[i-1]=delivery[i];
        --delivery_count;
        prepare(frame_packet_lease_view(item.packet),item.due);
        frame_packet_lease_release(item.packet);
    }
    return delivery_count?delivery[0].due:0;
}
extern "C" int packet_presenter_buffer(const FramePacketView *packet,uint64_t now,
    double guest_hz,PacketPresenterPrepare prepare) {
    if(!packet || !prepare || !std::isfinite(guest_hz) || guest_hz<=0)return 0;
#ifndef PSX_NO_DEBUG_TOOLS
    static const bool bypass=[] {const char *v=std::getenv("PSX_PRESENT_GUEST_CLOCK");return v && *v=='0';}();
    if(bypass)return 0;
#endif
    packet_presenter_trace(3,now,now);
    const unsigned hz=packet_interpolation_rate();
    if(!hz) {packet_presenter_buffer_reset();return 0;}
    if(delivery_rate!=hz) {packet_presenter_buffer_reset();packet_presenter_reset();delivery_rate=hz;}
    auto *lease=frame_packet_retain(packet);
    if(!lease) {packet_presenter_buffer_reset();packet_presenter_reset();return 0;}
    const uint64_t resyncs=delivery_clock.resyncs;
    const uint64_t delay=buffer_delay(guest_hz);delay_ms.store(delay/1e6);
    const uint64_t due=delivery_clock.map(packet->guest_time_ns,packet->epoch,now,delay);
    delivery_resyncs.store(delivery_clock.resyncs);
    if(resyncs!=delivery_clock.resyncs || delivery_count==delivery.size()) {
        if(delivery_count==delivery.size())++delivery_overflow;
        for(size_t i=0;i<delivery_count;++i)frame_packet_lease_release(delivery[i].packet);
        delivery_count=0;packet_presenter_reset();delivery_started=false;
    }
    if(!delivery_started) {
        // A restored epoch must show its first exact endpoint immediately.
        // Rewind can invalidate every VBlank, faster than the jitter delay;
        // buffering these first endpoints would otherwise freeze its image.
        delivery_started=true;prepare(frame_packet_lease_view(lease),due);
        frame_packet_lease_release(lease);return 1;
    }
    if(now>due)++delivery_late;
    delivery[delivery_count++]={lease,due};
    return 1;
}
