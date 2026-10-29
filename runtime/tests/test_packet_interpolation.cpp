#include "packet_interpolation.h"
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

static PacketInterpolationPrimitive primitive(uint64_t id,double x,double z) {
    PacketInterpolationPrimitive p{};
    p.identity={42,id}; p.vertices=3; p.spatial=1;
    for(unsigned i=0;i<3;++i) {
        auto& v=p.vertex[i];
        v.view[0]=x+i; v.view[1]=i; v.view[2]=z;
        v.h=290; v.ofx=160; v.ofy=120; v.x_scale=1;
        v.screen[0]=160+290*v.view[0]/z;
        v.screen[1]=120+290*v.view[1]/z;
        v.colour=0x123456; v.uv[0]=i*10; v.uv[1]=i*20;
    }
    return p;
}
int main() {
    auto *ctx=packet_interpolation_create(); assert(ctx);
    assert(!packet_interpolation_sample(ctx,0.5,nullptr,0));
    std::vector<PacketInterpolationPrimitive> a,b,out(4);
    for(unsigned i=1;i<=4;++i) a.push_back(primitive(i,10*i,100));
    // Alternate packet allocations and reverse order: double-buffer addresses,
    // primitive order, textures and colours are not part of identity.
    for(unsigned i=4;i>=1;--i) {
        b.push_back(primitive(i,30*i,200));
        b.back().vertex[0].colour=0xabcdef; b.back().vertex[0].uv[0]=99;
    }
    PacketInterpolationPacket pa{1,10,a.data(),a.size(),0},pb{1,12,b.data(),b.size(),0};
    assert(packet_interpolation_pair(ctx,&pa,&pb,2));
    auto stats=packet_interpolation_stats(ctx);
    assert(stats.matched==4 && !stats.unmatched && !stats.ambiguous && !stats.snapped);
    assert(packet_interpolation_sample(ctx,0.5,out.data(),out.size()));
    assert(out[0].vertex[0].view[0]==80 && out[0].vertex[0].view[2]==150);
    assert(std::abs(out[0].vertex[0].screen[0]-(160+290*80.0/150))<1e-10);
    assert(std::abs(out[0].vertex[0].screen[0]-
        (a[3].vertex[0].screen[0]+b[0].vertex[0].screen[0])*0.5)>1);
    assert(out[0].vertex[0].colour==0xabcdef && out[0].vertex[0].uv[0]==99);
    assert(packet_interpolation_sample(ctx,1,out.data(),out.size()));
    assert(std::memcmp(out.data(),b.data(),b.size()*sizeof(b[0]))==0);
    // Copied input survives queue-slot reuse and sampling is repeatable.
    a[0].vertex[0].view[0]=9999; b[0].vertex[0].view[0]=9999;
    assert(packet_interpolation_sample(ctx,0.5,out.data(),out.size()));
    assert(out[0].vertex[0].view[0]==80);
    a[0]=primitive(1,10,100); b[0]=primitive(4,80,200);
    // HUD takes N, even with a matching identity and moving coordinates.
    b[0].spatial=0;
    assert(packet_interpolation_pair(ctx,&pa,&pb,2));
    assert(packet_interpolation_sample(ctx,0,out.data(),out.size()));
    assert(std::memcmp(&out[0],&b[0],sizeof(b[0]))==0);
    b[0].spatial=1;
    // True absence is counted separately from a duplicate/invalid identity.
    b[0].identity.primitive=99;
    assert(packet_interpolation_pair(ctx,&pa,&pb,2));
    stats=packet_interpolation_stats(ctx);
    assert(stats.matched==3 && stats.unmatched==1 && stats.disappeared==1);
    b[0].identity=b[1].identity;
    assert(packet_interpolation_pair(ctx,&pa,&pb,2));
    stats=packet_interpolation_stats(ctx);
    assert(stats.ambiguous==1 && stats.unmatched==1 && stats.matched==3);
    assert(stats.resolved_assignment==1);
    b[0]=primitive(4,80,200);
    a[0].identity=a[1].identity;
    assert(packet_interpolation_pair(ctx,&pa,&pb,2));
    assert(packet_interpolation_stats(ctx).resolved_assignment==1);
    a[0]=primitive(1,10,100);
    b[0].vertex[0].view[2]=0;
    assert(packet_interpolation_pair(ctx,&pa,&pb,2));
    assert(packet_interpolation_stats(ctx).invalid==1);
    b[0]=primitive(4,80,200);
    // One discontinuous primitive snaps the whole frame, as do rewind/cuts.
    assert(packet_interpolation_pair(ctx,&pa,&pb,0.01));
    assert(packet_interpolation_stats(ctx).snapped);
    assert(packet_interpolation_sample(ctx,0.5,out.data(),out.size()));
    assert(std::memcmp(out.data(),b.data(),b.size()*sizeof(b[0]))==0);
    pb.epoch=2;
    assert(packet_interpolation_pair(ctx,&pa,&pb,2)); assert(packet_interpolation_stats(ctx).snapped);
    pb.epoch=1; pb.discontinuity=1;
    assert(packet_interpolation_pair(ctx,&pa,&pb,2)); assert(packet_interpolation_stats(ctx).snapped);
    pb.discontinuity=0; pb.sequence=9;
    assert(packet_interpolation_pair(ctx,&pa,&pb,2)); assert(packet_interpolation_stats(ctx).snapped);
    assert(!packet_interpolation_sample(ctx,std::numeric_limits<double>::quiet_NaN(),out.data(),4));
    assert(!packet_interpolation_sample(ctx,0.5,out.data(),3));
    // Same mesh repeated twice: stable occurrence pairing, independent buffers.
    std::vector<PacketInterpolationPrimitive> repeated_a={primitive(7,0,100),primitive(7,20,100)};
    std::vector<PacketInterpolationPrimitive> repeated_b={primitive(7,2,100),primitive(7,22,100)};
    PacketInterpolationPacket ra{1,1,repeated_a.data(),repeated_a.size(),0};
    PacketInterpolationPacket rb{1,2,repeated_b.data(),repeated_b.size(),0};
    assert(packet_interpolation_pair(ctx,&ra,&rb,0.5));
    stats=packet_interpolation_stats(ctx);
    assert(stats.matched==2 && stats.unmatched==0 && stats.resolved_occurrence==1);
    assert(packet_interpolation_sample(ctx,0.5,out.data(),out.size()));
    assert(out[0].vertex[0].view[0]==1 && out[1].vertex[0].view[0]==21);
    // A vanished instance changes counts. Greedy first-fit would choose the
    // wrong survivor; minimum total cost must select the closer old instance.
    rb.count=1;rb.primitives=&repeated_b[1];
    assert(packet_interpolation_pair(ctx,&ra,&rb,0.5));
    stats=packet_interpolation_stats(ctx);
    assert(stats.matched==1 && stats.unmatched==0 && stats.disappeared==1);
    assert(stats.resolved_assignment==1);
    assert(packet_interpolation_sample(ctx,0.5,out.data(),out.size()));
    assert(out[0].vertex[0].view[0]==21);
    // Gating must leave a distant new instance unresolved, never force a match.
    repeated_b[1]=primitive(7,1000,100);
    assert(packet_interpolation_pair(ctx,&ra,&rb,0.5));
    stats=packet_interpolation_stats(ctx);
    assert(stats.matched==0 && stats.unmatched==1 && stats.ambiguous==1);
    // Exact endpoints after 24 hours at every requested refresh rate.
    for(uint32_t rate:{60u,90u,120u,144u}) {
        uint64_t last=100;
        for(uint64_t i=1;i<=rate*10;++i) {
            uint64_t next=packet_interpolation_deadline(100,i,rate,1);
            assert(next>last); last=next;
        }
        assert(last==10000000100ULL);
        assert(packet_interpolation_deadline(100,86400ULL*rate,rate,1)==86400000000100ULL);
    }
    assert(packet_interpolation_deadline(0,60000,60000,1001)==1001000000000ULL);
    assert(packet_interpolation_deadline(UINT64_MAX-1,1,60,1)==UINT64_MAX);
    assert(packet_interpolation_deadline(0,1,0,1)==UINT64_MAX);
    assert(!packet_interpolation_pair(ctx,nullptr,&pb,2));
    assert(!packet_interpolation_sample(ctx,0.5,out.data(),4));
    packet_interpolation_destroy(ctx);
}
