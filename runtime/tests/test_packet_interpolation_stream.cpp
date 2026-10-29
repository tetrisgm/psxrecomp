#include "packet_interpolation_stream.h"
#undef NDEBUG
#include <cassert>
#include <cstring>
#include <vector>
#include <cstdlib>
#include <new>
static thread_local bool watch_allocations=false;
static thread_local size_t producer_allocations=0;
void* operator new(size_t n) {
    if(watch_allocations) ++producer_allocations;
    if(auto* p=std::malloc(n?n:1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}

int main() {
    // Rewind leases outlive the live mailbox. Holding more than the warm pool
    // must not exhaust capture or silently stop presentation.
    std::vector<PacketTransformTable*> retained;
    retained.reserve(64);
    watch_allocations=true;
    packet_interpolation_transform(nullptr,nullptr);
    PGXPProjection point{};point.interpolation_valid=1;
    for(unsigned i=0;i<64;++i) {
        point.transform=i+1;packet_interpolation_transform(&point,nullptr);
        auto* table=packet_interpolation_transform_seal();assert(table);
        assert(packet_interpolation_transform_bytes(table)>0);retained.push_back(table);
    }
    for(auto* table:retained) packet_interpolation_transform_free(table);
    packet_interpolation_transform(nullptr,nullptr);

    watch_allocations=false;
    assert(producer_allocations==0); // append, seal, release and reset, including retained rewind history

    PacketInterpolationSource source{};
    source.valid_mask=7;
    for(unsigned i=0;i<3;++i) {
        auto& p=source.projection[i];
        p.view[0]=float(i);p.view[2]=100;
        p.h=290;p.ofx=160;p.ofy=120;p.x_scale=1;
        p.model[0]=int16_t(i);p.caller=0x1234;p.interpolation_valid=1;
    }
    char json[2048];
    packet_interpolation_stream_reset();
    packet_interpolation_stream_begin(1,1);
    packet_interpolation_stream_append(&source);
    packet_interpolation_stream_end();
    // A held-display VBlank must not erase the preceding geometry sample.
    packet_interpolation_stream_begin(1,2);packet_interpolation_stream_end();
    packet_interpolation_stream_begin(1,3);
    packet_interpolation_stream_append(&source);packet_interpolation_stream_end();
    packet_interpolation_stream_json(json,sizeof json);
    assert(std::strstr(json,"\"matched\":1,"));
    assert(std::strstr(json,"\"unmatched\":0,"));
    packet_interpolation_stream_begin(1,4);
    packet_interpolation_stream_append(&source);packet_interpolation_stream_append(&source);
    packet_interpolation_stream_end();packet_interpolation_stream_json(json,sizeof json);
    assert(std::strstr(json,"\"ambiguous\":1,"));
    assert(std::strstr(json,"\"resolved_assignment\":1,"));
    assert(std::strstr(json,"\"duplicate_equal\":1,"));
    // A frozen debug capture owns its pair across subsequent packet reuse.
    packet_interpolation_capture_request();
    packet_interpolation_stream_begin(1,5);
    packet_interpolation_stream_append(&source);packet_interpolation_stream_end();
    char detail[16384];
    assert(packet_interpolation_capture_json(detail,sizeof detail,1,0)>0);
    assert(std::strstr(detail,"\"count\":2,"));
    assert(packet_interpolation_capture_json(detail,sizeof detail,0,0)>0);
    assert(std::strstr(detail,"\"count\":1,"));
    assert(packet_interpolation_capture_json(nullptr,0,0,0)==-1);
    // Reset removes the pair rather than interpolating across a rewind.
    packet_interpolation_stream_reset();
    assert(packet_interpolation_capture_json(detail,sizeof detail,0,0)>0);
    assert(std::strstr(detail,"\"count\":0,"));
    packet_interpolation_stream_begin(2,1);
    packet_interpolation_stream_append(&source);packet_interpolation_stream_end();
    packet_interpolation_stream_json(json,sizeof json);
    assert(std::strstr(json,"\"matched\":0,"));
    assert(std::strstr(json,"\"snapped\":1,"));
    source.valid_mask=0;source.spatial_mask=7;
    for(auto& p:source.projection) p={};
    packet_interpolation_stream_begin(2,2);
    packet_interpolation_stream_append(&source);packet_interpolation_stream_end();
    packet_interpolation_stream_json(json,sizeof json);
    assert(std::strstr(json,"\"partial_provenance\":1,"));
    assert(std::strstr(json,"\"invalid\":1,"));
    // Transformation-only packets precede drawing with double-buffered OTs.
    packet_interpolation_stream_reset();
    source.valid_mask=7;source.spatial_mask=7;
    for(unsigned frame=1;frame<=2;++frame) {
        packet_interpolation_stream_begin(3,frame*2);
        for(unsigned slot=0;slot<3;++slot) {
            const unsigned i=frame==2?2-slot:slot; // reordered generated mesh
            auto& p=source.projection[i];p={};
            p.source_vertex=0x1000; // shared staging slot, distinguished by model point
            p.source_matrix=0x2000;p.caller=0x3000;
            p.interpolation_valid=1;p.transform=frame;
            p.view[0]=float(frame*10+i);p.view[2]=100;p.h=290;p.x_scale=1;
            p.model[0]=int16_t(i);
            PacketInterpolationSource transform{};
            transform.transform_only=1;transform.projection[0]=p;
            packet_interpolation_stream_append(&transform);
        }
        packet_interpolation_stream_end();
        packet_interpolation_stream_begin(3,frame*2+1);
        packet_interpolation_stream_append(&source);packet_interpolation_stream_end();
    }
    packet_interpolation_stream_json(json,sizeof json);
    assert(std::strstr(json,"\"source_matched\":3,"));
    assert(std::strstr(json,"\"source_moving\":3,"));
    assert(std::strstr(json,"\"source_unmatched\":0,"));

    // A held immutable attachment survives dropped transport packets and
    // producer advancement. The consumer never needs live PGXP or guest RAM.
    packet_interpolation_stream_reset();
    for(unsigned i=0;i<3;++i) packet_interpolation_transform(&source.projection[i],nullptr);
    auto *held=packet_interpolation_transform_seal();assert(held);
    for(unsigned i=0;i<12;++i) {
        auto *drop=packet_interpolation_transform_seal();assert(drop);
        packet_interpolation_transform_free(drop);
    }
    packet_interpolation_stream_begin(4,1);packet_interpolation_stream_table(held);
    packet_interpolation_stream_append(&source);packet_interpolation_stream_end();
    packet_interpolation_stream_begin(4,2);packet_interpolation_stream_table(held);
    packet_interpolation_stream_append(&source);packet_interpolation_stream_end();
    packet_interpolation_transform_free(held);
    packet_interpolation_stream_json(json,sizeof json);
    assert(std::strstr(json,"\"source_matched\":3,"));
    assert(std::strstr(json,"\"source_unmatched\":0,"));

    // One caller sees one point at a shared address, another emits many.
    // Classification belongs to the storage address, not each caller's count.
    packet_interpolation_stream_reset();packet_interpolation_transform(nullptr,nullptr);
    for(unsigned frame=1;frame<=2;++frame) {
        PGXPAffineTransform affine{};
        affine.rotation[0]=affine.rotation[4]=affine.rotation[8]=4096;
        affine.translation[0]=int32_t(frame*10);affine.translation[2]=100;
        for(unsigned i=0;i<3;++i) {
            auto& p=source.projection[i];p={};
            p.source_vertex=0x4000;p.source_matrix=0x5000;p.caller=i?0x6000:0x7000;
            p.interpolation_valid=1;p.transform=frame;
            p.model[0]=int16_t(i?i:frame*10);p.view[0]=float(p.model[0]+frame*10);
            p.view[2]=100;p.h=290;p.x_scale=1;
            packet_interpolation_transform(&p,&affine);
        }
        auto *table=packet_interpolation_transform_seal();assert(table);
        packet_interpolation_stream_begin(5,frame);packet_interpolation_stream_table(table);
        packet_interpolation_stream_append(&source);packet_interpolation_stream_end();
        packet_interpolation_transform_free(table);
    }
    packet_interpolation_stream_json(json,sizeof json);
    assert(std::strstr(json,"\"source_matched\":6,"));
    assert(packet_interpolation_stream_sample(0.5));
    const auto* departing=packet_interpolation_stream_retiring(0);
    assert(departing && departing->interpolated);
    const auto* current=packet_interpolation_stream_vertex(0);
    // The shared edge uses the same sampled source table in both topologies.
    for(unsigned v=1;v<3;++v)
        assert(std::memcmp(departing->vertex[v].view,current->vertex[v].view,sizeof current->vertex[v].view)==0);
    assert(std::strstr(json,"\"model_reconstructed\":1,"));
    assert(std::strstr(json,"\"unexplained\":0,"));

}
