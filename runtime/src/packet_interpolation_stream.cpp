#include "packet_interpolation_stream.h"
#include "packet_interpolation.h"
#include <cstdio>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace {
struct Stream {
    PacketInterpolation *pair=packet_interpolation_create();
    std::vector<PacketInterpolationPrimitive> older,current;
    std::vector<PacketInterpolationSource> old_sources,sources;
    uint64_t old_epoch=0,old_sequence=0,epoch=0,sequence=0;
    uint64_t missing=0;
    bool failed=false;
    ~Stream() { packet_interpolation_destroy(pair); }
} stream;
std::mutex stats_mutex;
PacketInterpolationStats published{};
uint64_t published_sequence=0,published_missing=0,published_count=0;
bool published_failed=false;
std::atomic<bool> capture_requested{false};
std::vector<PacketInterpolationPrimitive> captured[2];
std::vector<PacketInterpolationSource> captured_sources[2];
uint64_t captured_sequence[2]{};
std::atomic<PacketInterpolationRecorder> recorder{nullptr};
uint64_t hash(uint64_t seed,uint32_t value) {
    for(unsigned i=0;i<4;++i) seed=(seed^((value>>(i*8))&255))*1099511628211ULL;
    return seed;
}
}
extern "C" void packet_interpolation_set_recorder(PacketInterpolationRecorder callback) {
    recorder.store(callback,std::memory_order_release);
}
extern "C" void packet_interpolation_record(const PacketInterpolationSource *source) {
    auto callback=recorder.load(std::memory_order_acquire);
    if(callback) callback(source);
}
extern "C" int packet_interpolation_stream_enabled(void) {
    static const bool enabled=[] {
        const char *value=std::getenv("PSX_PACKET_INTERPOLATION_AUDIT");
        return value && value[0]=='1';
    }();
    return enabled;
}
extern "C" void packet_interpolation_stream_begin(uint64_t epoch,uint64_t sequence) {
    stream.epoch=epoch;stream.sequence=sequence;stream.current.clear();stream.sources.clear();
    stream.missing=0;stream.failed=false;
}
extern "C" void packet_interpolation_stream_append(const PacketInterpolationSource *s) {
    if(!s || stream.failed) return;
    PacketInterpolationPrimitive p{};
    p.vertices=3;p.spatial=(s->valid_mask|s->spatial_mask)!=0;
    if(s->valid_mask!=7) {
        if(p.spatial) ++stream.missing;
        // Partial provenance stays in the census as invalid, never becomes HUD.
    }
    uint64_t a=14695981039346656037ULL,b=7809847782465536322ULL;
    a=hash(a,s->primitive_type);b=hash(b,s->texpage);b=hash(b,s->clut);
    for(unsigned i=0;i<3;++i) {
        const auto& v=s->projection[i];
        a=hash(a,v.source_vertex);a=hash(a,v.source_matrix);
        a=hash(a,v.caller);
        for(unsigned d=0;d<3;++d) { a=hash(a,uint16_t(v.model[d])); b=hash(b,uint16_t(v.model[d])); }
        b=hash(b,v.caller);
        auto& out=p.vertex[i];
        for(unsigned d=0;d<3;++d) out.view[d]=v.view[d];
        out.h=v.h;out.ofx=v.ofx;out.ofy=v.ofy;out.x_scale=v.x_scale;
        out.screen[0]=s->x[i];out.screen[1]=s->y[i];
    }
    p.identity={a,b};
    try { stream.current.push_back(p);stream.sources.push_back(*s); }
    catch(...) { stream.failed=true;stream.current.clear(); }
}
extern "C" void packet_interpolation_stream_end(void) {
    /* A VBlank packet without polygon records is not a new geometry sample.
     * R4's 30 Hz draw produces these between draws. Keep the previous sample
     * for the audit; this is not a policy for presenting blank/cleared frames. */
    if(stream.current.empty() && !stream.failed) return;
    PacketInterpolationPacket a{stream.old_epoch,stream.old_sequence,stream.older.data(),stream.older.size(),0};
    PacketInterpolationPacket b{stream.epoch,stream.sequence,stream.current.data(),stream.current.size(),0};
    if(!stream.failed && !packet_interpolation_pair(stream.pair,&a,&b,0.5)) stream.failed=true;
    {
        std::lock_guard<std::mutex> lock(stats_mutex);
        published=stream.failed?PacketInterpolationStats{}:packet_interpolation_stats(stream.pair);
        published_sequence=stream.sequence;published_missing=stream.missing;
        published_count=stream.current.size();published_failed=stream.failed;
        if(capture_requested.exchange(false)) {
            try {
                captured[0]=stream.older;captured[1]=stream.current;
                captured_sources[0]=stream.old_sources;captured_sources[1]=stream.sources;
                captured_sequence[0]=stream.old_sequence;captured_sequence[1]=stream.sequence;
            } catch(...) {
                captured[0].clear();captured[1].clear();
                captured_sources[0].clear();captured_sources[1].clear();
                captured_sequence[0]=captured_sequence[1]=0;published_failed=true;
            }
        }
    }
    stream.older.swap(stream.current);stream.old_sources.swap(stream.sources);
    stream.old_epoch=stream.epoch;stream.old_sequence=stream.sequence;
}
extern "C" void packet_interpolation_stream_reset(void) {
    stream.old_sources.clear();stream.sources.clear();
    stream.older.clear();stream.current.clear();stream.old_epoch=stream.old_sequence=0;
    std::lock_guard<std::mutex> lock(stats_mutex);
    published={};published_sequence=published_missing=published_count=0;published_failed=false;
}
extern "C" int packet_interpolation_stream_json(char *out,size_t capacity) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    return std::snprintf(out,capacity,
        "{\"status\":\"ok\",\"mode\":\"candidate_audit\",\"sequence\":%llu,\"primitives\":%llu,"
        "\"matched\":%llu,\"unmatched\":%llu,\"disappeared\":%llu,\"ambiguous\":%llu,"
        "\"resolved_occurrence\":%llu,\"resolved_assignment\":%llu,\"appeared_candidates\":%llu,\"invalid\":%llu,\"partial_provenance\":%llu,\"duplicate_equal\":%llu,\"snapped\":%d,\"failed\":%s}",
        (unsigned long long)published_sequence,(unsigned long long)published_count,
        (unsigned long long)published.matched,(unsigned long long)published.unmatched,
        (unsigned long long)published.disappeared,(unsigned long long)published.ambiguous,
        (unsigned long long)published.resolved_occurrence,(unsigned long long)published.resolved_assignment,
        (unsigned long long)published.appeared_candidates,(unsigned long long)published.invalid,(unsigned long long)published_missing,
        (unsigned long long)published.duplicate_equal,published.snapped,published_failed?"true":"false");
}

extern "C" void packet_interpolation_capture_request(void) {
    capture_requested.store(true);
}
extern "C" int packet_interpolation_capture_json(char *out,size_t capacity,int older,size_t start) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    const unsigned side=older?0:1;
    const auto& list=captured[side];const auto& sources=captured_sources[side];
    size_t used=0;
    auto append=[&](const char *fmt,auto... args) {
        if(used>=capacity) return;
        int n=std::snprintf(out+used,capacity-used,fmt,args...);
        if(n>0) used+=size_t(n);
    };
    append("{\"sequence\":%llu,\"count\":%llu,\"pending\":%s,\"primitives\":[",
        (unsigned long long)captured_sequence[side],(unsigned long long)list.size(),
        capture_requested.load()?"true":"false");
    for(size_t i=start;i<list.size() && i<start+8 && i<sources.size();++i) {
        const auto& p=list[i]; const auto& s=sources[i];
        append("%s{\"index\":%llu,\"key\":\"%016llx%016llx\",\"type\":%u,\"texpage\":%u,\"clut\":%u,\"vertices\":[",
            i==start?"":",",(unsigned long long)i,(unsigned long long)p.identity.object,
            (unsigned long long)p.identity.primitive,s.primitive_type,s.texpage,s.clut);
        for(unsigned k=0;k<3;++k) {
            const auto& v=s.projection[k];
            append("%s{\"caller\":%u,\"source\":%u,\"matrix\":%u,\"transform\":\"%016llx\",\"model\":[%d,%d,%d],\"view\":[%.9g,%.9g,%.9g]}",
                k?",":"",v.caller,v.source_vertex,v.source_matrix,(unsigned long long)v.transform,
                v.model[0],v.model[1],v.model[2],v.view[0],v.view[1],v.view[2]);
        }
        append("%s","]}");
    }
    append("%s","]}");return used<capacity?(int)used:-1;
}
