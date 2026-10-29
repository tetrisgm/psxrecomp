#include "packet_presenter.h"
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
uint64_t start=0,period=0,index=0,last_sequence=0,last_epoch=0;
unsigned rate=0;
std::atomic<uint64_t> submitted{0},generated{0},endpoints{0};
}
extern "C" void packet_presenter_reset(void) {
    frame_packet_lease_release(current);current=nullptr;
    frame_packet_lease_release(previous);previous=nullptr;
#ifndef PSX_NO_DEBUG_TOOLS
    requested_sample.store(-1);
#endif
    start=period=index=last_sequence=last_epoch=0;rate=0;
}
extern "C" int packet_presenter_submit(const FramePacketView *packet,uint64_t now,
    double guest_hz,PacketPresenterDraw draw) {
    const unsigned hz=packet_interpolation_rate();
    if(!hz || !packet || !draw || !std::isfinite(guest_hz) || guest_hz<=0) {
        packet_presenter_reset();return 0;
    }
    auto *next=frame_packet_retain(packet);
    if(!next) return 0;
    const uint64_t frames=packet->epoch==last_epoch && packet->sequence>last_sequence?
        packet->sequence-last_sequence:0;
    period=frames && frames<=8?uint64_t(double(frames)*1e9/guest_hz):0;
    // Exact left endpoint uses the real retained packet, including topology,
    // uploads and materials. Reprojecting N's topology at t=0 is not N-1.
    if(period && current && !packet_interpolation_stream_snapped())
        draw(frame_packet_lease_view(current),-1.0);
    else draw(frame_packet_lease_view(next),1.0);
    frame_packet_lease_release(previous);previous=current;current=next;
    last_sequence=packet->sequence;last_epoch=packet->epoch;
    start=now;index=1;rate=hz;
    ++submitted;
    return 1;
}
extern "C" uint64_t packet_presenter_tick(uint64_t now,PacketPresenterDraw draw) {
#ifndef PSX_NO_DEBUG_TOOLS
    const int requested=requested_sample.exchange(-1);
    if(requested>=0 && current && draw) {
        if(requested==0 && previous) draw(frame_packet_lease_view(previous),-1.0);
        else draw(frame_packet_lease_view(current),requested/1000.0);
        ++sampled_requests;period=0;return 0;
    }
#endif
    if(!current || !period || !draw || !packet_interpolation_rate()) return 0;
    const uint64_t due=packet_interpolation_deadline(start,index,rate,1);
    if(now<due) return due;
    const double t=std::min(1.0,double(now-start)/double(period));
    draw(frame_packet_lease_view(current),t);
    if(t<1) ++generated;else ++endpoints;
    if(t==1) {period=0;return 0;}
    // Skip missed display deadlines: presentation never builds a backlog.
    index=std::max(index+1,uint64_t(double(now-start)*rate/1e9)+1);
    return packet_interpolation_deadline(start,index,rate,1);
}

extern "C" int packet_presenter_json(char *out,size_t bytes) {
    unsigned requests=0;
#ifndef PSX_NO_DEBUG_TOOLS
    requests=sampled_requests.load();
#endif
    return std::snprintf(out,bytes,"{\"rate\":%u,\"submitted\":%llu,\"in_between\":%llu,\"endpoints\":%llu,\"sampled_requests\":%u}",
        packet_interpolation_rate(),(unsigned long long)submitted.load(),
        (unsigned long long)generated.load(),(unsigned long long)endpoints.load(),requests);
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
