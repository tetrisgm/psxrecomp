#include "packet_presenter.h"
#include "packet_interpolation_stream.h"
#include <algorithm>
#include <cmath>

namespace {
FramePacketLease *current=nullptr;
uint64_t start=0,period=0,index=0,last_sequence=0,last_epoch=0;
unsigned rate=0;
}
extern "C" void packet_presenter_reset(void) {
    frame_packet_lease_release(current);current=nullptr;
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
    frame_packet_lease_release(current);current=next;
    last_sequence=packet->sequence;last_epoch=packet->epoch;
    start=now;index=1;rate=hz;
    draw(frame_packet_lease_view(current),period?0.0:1.0);
    return 1;
}
extern "C" uint64_t packet_presenter_tick(uint64_t now,PacketPresenterDraw draw) {
    if(!current || !period || !draw || !packet_interpolation_rate()) return 0;
    const uint64_t due=packet_interpolation_deadline(start,index,rate,1);
    if(now<due) return due;
    const double t=std::min(1.0,double(now-start)/double(period));
    draw(frame_packet_lease_view(current),t);
    if(t==1) {period=0;return 0;}
    // Skip missed display deadlines: presentation never builds a backlog.
    index=uint64_t(double(now-start)*rate/1e9)+1;
    return packet_interpolation_deadline(start,index,rate,1);
}
