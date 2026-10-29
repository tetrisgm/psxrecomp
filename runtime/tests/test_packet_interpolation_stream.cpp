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
    // Reset removes the pair rather than interpolating across a rewind.
    packet_interpolation_stream_reset();
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
}
