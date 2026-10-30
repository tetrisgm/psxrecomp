#include "packet_presenter.h"
#include "packet_delivery_clock.h"
#include "packet_interpolation_stream.h"
#include "packet_metadata.h"
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <vector>
static std::vector<double> phases;
static unsigned freed=0;
static void dispose(void *p) {delete static_cast<int*>(p);++freed;}
static void draw(const FramePacketView *p,double t) {
    assert(*static_cast<const int*>(p->attachment)==int(p->sequence));
    phases.push_back(t);
}
static void submit(FramePacketQueue *q,uint64_t sequence,uint64_t now) {
    assert(frame_packet_begin(q,sequence,nullptr,0));
    assert(frame_packet_attach(q,new int(int(sequence)),sizeof(int),dispose));
    frame_packet_stamp(q,sequence*16666667ull);
    assert(frame_packet_publish(q));
    FramePacketView view{};assert(frame_packet_acquire(q,&view));
    assert(packet_presenter_submit(&view,now,60,draw));
    frame_packet_release(q);
}
static std::vector<double> values;
static void sample(const FramePacketView *p,double t) {
    values.push_back(t<0?double(p->guest_time_ns):double(p->guest_time_ns)-16666667.0*(1.0-t));
}
static void prepare(const FramePacketView *p,uint64_t due) {
    assert(packet_presenter_submit(p,due,60,sample));
}
static std::vector<double> buffered_run(uint64_t jitter_ns) {
    packet_presenter_reset();packet_presenter_buffer_reset();packet_interpolation_set_rate(60);
    values.clear();auto *q=frame_packet_create(128,128);
    uint64_t sequence=1,previous_arrival=0,now=1000000000;
    uint64_t next_delivery=UINT64_MAX,next_sample=UINT64_MAX;
    while(now<4000000000) {
        uint64_t arrival=sequence<=120?1000000000+(sequence-1)*16666667:UINT64_MAX;
        if(jitter_ns && sequence<=120 && sequence%17==0)arrival+=jitter_ns;
        arrival=std::max(arrival,previous_arrival);
        now=std::min(arrival,std::min(next_delivery,next_sample));
        if(now==UINT64_MAX)break;
        if(arrival==now) {
            assert(frame_packet_begin(q,sequence,nullptr,0));
            frame_packet_stamp(q,sequence*16666667);
            assert(frame_packet_publish(q));FramePacketView view{};
            assert(frame_packet_acquire(q,&view));
            assert(packet_presenter_buffer(&view,now,60,prepare));
            if(sequence==1)assert(values.size()==1); // first restored image never waits
            frame_packet_release(q);previous_arrival=arrival;++sequence;
        }
        next_delivery=packet_presenter_deliver(now,prepare);
        next_sample=packet_presenter_tick(now,sample);
        if(!next_delivery)next_delivery=UINT64_MAX;
        if(!next_sample)next_sample=UINT64_MAX;
    }
    frame_packet_destroy(q);packet_presenter_buffer_reset();packet_presenter_reset();
    return values;
}
static double guest_value;
static int capture(void *out) {*static_cast<double*>(out)=guest_value;return 1;}
static void blend(const void *a,const void *b,double t,void *out) {
    const double x=*static_cast<const double*>(a),y=*static_cast<const double*>(b);
    *static_cast<double*>(out)=x+t*(y-x);
}
int main() {
#ifdef _WIN32
    _putenv_s("PSX_PRESENT_DELAY_MS","33.333334");
#else
    setenv("PSX_PRESENT_DELAY_MS","33.333334",1);
#endif
    const auto steady=buffered_run(0),jittered=buffered_run(25000000);
    assert(steady.size()>100 && steady==jittered);
    const auto held=buffered_run(70000000);
    assert(held.size()>80 && std::is_sorted(held.begin(),held.end()));

    for(unsigned rate:{60u,90u,120u,144u}) {
        packet_presenter_reset();packet_interpolation_set_rate(rate);phases.clear();
        auto *q=frame_packet_create(128,128);assert(q);
        submit(q,1,100);assert(phases.back()==1);
        submit(q,3,33333433);assert(phases.size()==1);
        // The queue may die while the presenter still holds its immutable lease.
        frame_packet_destroy(q);
        uint64_t due=packet_presenter_tick(33333433,draw);
        assert(due>33333433);
        while(due) {due=packet_presenter_tick(due,draw);assert(phases.size()<10);}
        assert(phases.back()==1);
        for(size_t i=2;i<phases.size();++i) assert(phases[i]>phases[i-1]);
        packet_presenter_reset();
    }
    assert(freed==8);
    // Fixed delay absorbs 25 ms arrival spikes without moving ANY deadline.
    // 33.3 ms is selected here because the default 16.7 ms cannot conceal
    // a 25 ms producer outage. Beyond-buffer outages hold, never extrapolate
    // unmatched geometry or modify the guest clock.
    PacketDeliveryClock clock;
    const uint64_t base=1000000000,step=16666667,delay=2*step;
    uint64_t prior=0;
    for(uint64_t i=0;i<600;++i) {
        const uint64_t jitter=i%17==0 && i?25000000:0;
        // Preserve publication order when a stall delays the next frame too.
        const uint64_t arrival=std::max(base+i*step+jitter,prior);
        prior=arrival;
        const uint64_t due=clock.map(i*step,1,arrival,delay);
        assert(due==base+i*step+delay);
        assert(due>=arrival);
    }
    assert(clock.resyncs==0);
    // A short late endpoint keeps the same mapping; a long stall reanchors.
    assert(clock.map(600*step,1,base+600*step+delay+25000000,delay)==base+600*step+delay);
    const uint64_t late=base+601*step+500000000;
    assert(clock.map(601*step,1,late,delay)==late+delay);
    assert(clock.resyncs==1);
    assert(clock.map(0,2,late+step,delay)==late+step+delay);
    assert(clock.resyncs==2);
    assert(clock.map(0,2,late+2*step,UINT64_MAX)==late+2*step+PacketDeliveryClock::max_delay_ns);

    assert(packet_metadata_register(1,sizeof(double),capture,blend));
    assert(!packet_metadata_register(1,sizeof(double),capture,blend));
    guest_value=10;auto *a=packet_metadata_capture();assert(a);
    guest_value=30;auto *b=packet_metadata_capture();assert(b);
    packet_metadata_pair(a,1);packet_metadata_pair(b,1);
    packet_metadata_free(a);packet_metadata_free(b);guest_value=999;
    double result=0;assert(!packet_metadata_read(1,&result,sizeof result));
    packet_metadata_sample(0.25);assert(packet_metadata_read(1,&result,sizeof result));
    assert(result==15);packet_metadata_end();assert(!packet_metadata_read(1,&result,sizeof result));
    guest_value=50;a=packet_metadata_capture();packet_metadata_pair(a,2);
    packet_metadata_free(a);packet_metadata_sample(0);assert(packet_metadata_read(1,&result,sizeof result));
    assert(result==50);packet_metadata_reset();
}
