#include "packet_interpolation_stream.h"
#undef NDEBUG
#include <cassert>
#include <cstring>

int main() {
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
        for(unsigned i=0;i<3;++i) {
            auto& p=source.projection[i];p={};
            p.source_vertex=0x1000+i*8;p.source_matrix=0x2000;p.caller=0x3000;
            p.interpolation_valid=1;p.transform=frame;
            p.view[0]=float(frame*10+i);p.view[2]=100;p.h=290;p.x_scale=1;
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
    assert(std::strstr(json,"\"source_unmatched\":0,"));

    // A held immutable attachment survives dropped transport packets and
    // producer advancement. The consumer never needs live PGXP or guest RAM.
    packet_interpolation_stream_reset();
    for(unsigned i=0;i<3;++i) packet_interpolation_transform(&source.projection[i]);
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

}
