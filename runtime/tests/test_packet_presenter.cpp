#include "packet_presenter.h"
#include "packet_interpolation_stream.h"
#include "packet_metadata.h"
#undef NDEBUG
#include <cassert>
#include <cmath>
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
    assert(frame_packet_publish(q));
    FramePacketView view{};assert(frame_packet_acquire(q,&view));
    assert(packet_presenter_submit(&view,now,60,draw));
    frame_packet_release(q);
}
static double guest_value;
static int capture(void *out) {*static_cast<double*>(out)=guest_value;return 1;}
static void blend(const void *a,const void *b,double t,void *out) {
    const double x=*static_cast<const double*>(a),y=*static_cast<const double*>(b);
    *static_cast<double*>(out)=x+t*(y-x);
}
int main() {
    for(unsigned rate:{60u,90u,120u,144u}) {
        packet_presenter_reset();packet_interpolation_set_rate(rate);phases.clear();
        auto *q=frame_packet_create(128,128);assert(q);
        submit(q,1,100);assert(phases.back()==1);
        submit(q,3,33333433);assert(phases.back()==-1);
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
