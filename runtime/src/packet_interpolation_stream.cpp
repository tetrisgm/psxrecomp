#include "packet_interpolation_stream.h"
#include "packet_interpolation.h"
#include <cstdio>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <unordered_set>
#include <array>
#include <memory>

struct TransformRecord {PGXPProjection projection;PGXPAffineTransform affine;bool valid;};
// Fixed producer storage: one bounded append per GTE point, no allocation or
// history scan. Packet sealing shares immutable blocks; the render consumer
// performs all history traversal. Sixteen blocks cover nine history ticks plus
// the mailbox and retained presenter leases without waiting for the GPU.
struct TransformBlock {
    std::array<TransformRecord,32768> values;
    size_t count=0;
};
struct PacketTransformTable {
    std::array<std::shared_ptr<TransformBlock>,9> blocks;
};
namespace {
std::array<std::shared_ptr<TransformBlock>,16> transform_pool=[] {
    std::array<std::shared_ptr<TransformBlock>,16> result;
    for(auto& block:result) block=std::make_shared<TransformBlock>();
    return result;
}();
std::array<std::shared_ptr<TransformBlock>,9> producer_history;
std::shared_ptr<TransformBlock> producer_block=transform_pool[0];
bool producer_failed=false;
void acquire_transform_block() {
    for(auto& block:transform_pool) if(block.use_count()==1) {
        producer_block=block;producer_block->count=0;return;
    }
    // Rewind may retain blocks longer than the live mailbox. Allocate one
    // uninitialized, fixed-capacity block at the packet boundary, never in
    // the per-vertex append. Packet accounting bounds retained history.
    try {producer_block.reset(new TransformBlock);} catch(...) {producer_failed=true;}
}

struct Stream {
    PacketInterpolation *pair=packet_interpolation_create();
    std::vector<PacketInterpolationPrimitive> older,current,sampled;
    std::vector<PacketInterpolationPrimitive> union_geometry,retiring_endpoints;
    std::vector<size_t> retiring_indices;
    std::vector<PacketInterpolationSource> old_sources,sources;
    uint64_t old_epoch=0,old_sequence=0,epoch=0,sequence=0;
    std::vector<PacketInterpolationTransform> transforms,old_transforms;
    struct Raw { PGXPProjection projection; uint64_t sequence; PGXPAffineTransform affine{};bool valid=false; };
    std::vector<Raw> history;
    std::unordered_map<uint64_t,std::vector<uint64_t>> invocations;
    std::unordered_map<uint64_t,std::unordered_map<uint64_t,std::vector<std::array<int16_t,3>>>> ordinals;
    std::unordered_map<uint64_t,std::vector<uint64_t>> model_invocations;
    std::unordered_set<uint32_t> staging_addresses;
    bool source_mode=false;
    uint64_t missing=0;
    bool failed=false;
    ~Stream() { packet_interpolation_destroy(pair); }
} stream;
std::mutex stats_mutex;
PacketInterpolationStats published{};
uint64_t published_sequence=0,published_missing=0,published_count=0;
bool published_failed=false,published_source_mode=false;
size_t retiring_unresolved=0,retiring_count=0;
size_t published_retiring_unresolved=0,published_retiring_count=0;
std::atomic<unsigned> output_rate{0},display_rate{60};
std::atomic<bool> capture_requested{false};
std::vector<PacketInterpolationPrimitive> captured[2];
std::vector<PacketInterpolationSource> captured_sources[2];
uint64_t captured_sequence[2]{};
std::atomic<PacketInterpolationRecorder> recorder{nullptr};
uint64_t hash(uint64_t seed,uint32_t value) {
    for(unsigned i=0;i<4;++i) seed=(seed^((value>>(i*8))&255))*1099511628211ULL;
    return seed;
}
FramePrimitiveIdentity model_identity(const PGXPProjection& v,bool record) {
    const uint64_t base=hash(hash(14695981039346656037ULL,v.source_matrix),v.caller);
    auto& instances=stream.model_invocations[base];
    auto at=std::find(instances.begin(),instances.end(),v.transform);
    if(at==instances.end()) {
        if(!record) return {};
        instances.push_back(v.transform);at=instances.end()-1;
    }
    return {base,uint64_t(at-instances.begin())+1};
}
FramePrimitiveIdentity source_identity(const PGXPProjection& v,bool record) {
    uint64_t base=14695981039346656037ULL;
    base=hash(base,v.source_vertex);base=hash(base,v.source_matrix);base=hash(base,v.caller);
    if(!v.source_vertex) return {};
    if(!record && !stream.invocations.count(base)) return {};
    auto& instances=stream.invocations[base];
    auto at=std::find(instances.begin(),instances.end(),v.transform);
    if(at==instances.end()) {
        if(!record) return {};
        instances.push_back(v.transform);at=instances.end()-1;
    }
    // Some games stream multiple vertices through one RAM staging address.
    // Their ordinal is transformation order within the invocation, not their
    // changing model coordinate value and not visible primitive order.
    auto& vertices=stream.ordinals[base][v.transform];
    const std::array<int16_t,3> model{v.model[0],v.model[1],v.model[2]};
    auto vertex=std::find(vertices.begin(),vertices.end(),model);
    if(vertex==vertices.end()) {
        if(!record) return {};
        vertices.push_back(model);vertex=vertices.end()-1;
    }
    if(vertices.size()>1) stream.staging_addresses.insert(v.source_vertex);
    // A staging address can emit a changing number/order of subdivision
    // vertices. Ordinal pairing tears adjacent faces when that order shifts.
    // Real vertex addresses retain identity through animation; shared staging
    // addresses use the model-space point and reconstruct missing points from
    // the preceding whole invocation's matrix.
    if(!record && stream.staging_addresses.count(v.source_vertex)) {
        for(int16_t coordinate:model) base=hash(base,uint16_t(coordinate));
    }
    return {base,uint64_t(at-instances.begin())+1};
}
}
extern "C" void packet_interpolation_transform(const PGXPProjection *p,const PGXPAffineTransform *affine) {
    if(!p) {producer_history={};producer_block.reset();producer_failed=false;acquire_transform_block();return;}
    if(!p->interpolation_valid || producer_failed) return;
    if(!producer_block) {producer_failed=true;return;}
    if(!producer_block || producer_block->count==producer_block->values.size()) {producer_failed=true;return;}
    producer_block->values[producer_block->count++]={*p,affine?*affine:PGXPAffineTransform{},affine!=nullptr};
}
extern "C" PacketTransformTable *packet_interpolation_transform_seal(void) {
    PacketTransformTable *result=nullptr;
    for(size_t i=0;i+1<producer_history.size();++i) producer_history[i]=std::move(producer_history[i+1]);
    producer_history.back()=std::move(producer_block);
    try {if(!producer_failed) result=new PacketTransformTable{producer_history};}
    catch(...) {}
    producer_failed=false;acquire_transform_block();
    return result;
}
extern "C" void packet_interpolation_transform_free(PacketTransformTable *p) {delete p;}
extern "C" size_t packet_interpolation_transform_bytes(const PacketTransformTable *p) {
    if(!p) return 0;
    size_t bytes=sizeof(*p);
    for(const auto& block:p->blocks) if(block) bytes+=block->count*sizeof(TransformRecord);
    return bytes;
}
extern "C" void packet_interpolation_stream_table(const PacketTransformTable *p) {
    stream.source_mode=true;stream.history.clear();
    if(!p) {stream.failed=true;return;}
    try {for(const auto& block:p->blocks) if(block) for(size_t i=0;i<block->count;++i) {
        const auto& v=block->values[i];
        stream.history.push_back({v.projection,stream.sequence,v.affine,v.valid});
    }} catch(...) {stream.failed=true;}
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
    return enabled || output_rate.load(std::memory_order_relaxed)!=0;
}
extern "C" void packet_interpolation_stream_begin(uint64_t epoch,uint64_t sequence) {
    stream.epoch=epoch;stream.sequence=sequence;stream.current.clear();stream.sources.clear();
    stream.missing=0;stream.failed=false;

}
extern "C" void packet_interpolation_stream_append(const PacketInterpolationSource *s) {
    if(!s || stream.failed) return;
    if(s->transform_only) {
        stream.source_mode=true;
        const auto& v=s->projection[0];
        if(!v.interpolation_valid) return;
        try {
            stream.history.push_back({v,stream.sequence});
            PacketInterpolationTransform transform{};
            transform.source=source_identity(v,true);
            transform.model=model_identity(v,true);
            for(unsigned d=0;d<3;++d) {
                transform.view[d]=v.view[d];
                for(unsigned k=0;k<3;++k) transform.affine[4*d+k]=0.0;
                transform.affine[4*d+3]=0;
            }
            transform.affine_valid=0;
            transform.h=v.h;transform.ofx=v.ofx;transform.ofy=v.ofy;transform.x_scale=v.x_scale;
            stream.transforms.push_back(transform);
        } catch(...) {stream.failed=true;}
        return;
    }
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

        for(unsigned d=0;d<3;++d) {out.view[d]=v.view[d];out.model_position[d]=v.model[d];}
        out.h=v.h;out.ofx=v.ofx;out.ofy=v.ofy;out.x_scale=v.x_scale;
        out.screen[0]=s->x[i];out.screen[1]=s->y[i];
    }
    p.identity={a,b};
    try { stream.current.push_back(p);stream.sources.push_back(*s); }
    catch(...) { stream.failed=true;stream.current.clear(); }
}
static void stream_end_impl(void) {
    /* A VBlank packet without polygon records is not a new geometry sample.
     * R4's 30 Hz draw produces these between draws. Keep the previous sample
     * for the audit; this is not a policy for presenting blank/cleared frames. */
    if(stream.current.empty() && !stream.failed) return;
    if(stream.source_mode) {
        // OT submission may follow transformation across a VBlank. Select the
        // complete transform groups referenced by this draw, including their
        // culled vertices, from the bounded pre-cull history.
        std::unordered_set<uint64_t> groups;
        for(const auto& source:stream.sources) for(const auto& p:source.projection)
            if(p.interpolation_valid) groups.insert(p.transform);
        stream.transforms.clear();stream.invocations.clear();stream.model_invocations.clear();stream.ordinals.clear();
        for(const auto& raw:stream.history) if(groups.count(raw.projection.transform)) {
            (void)source_identity(raw.projection,true);
            (void)model_identity(raw.projection,true);
        }
        for(const auto& raw:stream.history) if(groups.count(raw.projection.transform)) {
            const auto& v=raw.projection;
            PacketInterpolationTransform transform{};
            transform.source=source_identity(v,false);
            transform.model=model_identity(v,false);
            for(unsigned d=0;d<3;++d) {
                transform.view[d]=v.view[d];
                for(unsigned k=0;k<3;++k) transform.affine[4*d+k]=double(raw.affine.rotation[3*d+k])/4096.0;
                transform.affine[4*d+3]=raw.affine.translation[d];
            }
            transform.affine_valid=raw.valid;
            transform.h=v.h;transform.ofx=v.ofx;transform.ofy=v.ofy;transform.x_scale=v.x_scale;
            stream.transforms.push_back(transform);
        }
        for(size_t i=0;i<stream.current.size();++i) {
            uint64_t identity=hash(14695981039346656037ULL,stream.sources[i].primitive_type);
            uint64_t model=7809847782465536322ULL;
            for(unsigned j=0;j<3;++j) {
                auto& vertex=stream.current[i].vertex[j];
                vertex.source=source_identity(stream.sources[i].projection[j],false);
                vertex.model=model_identity(stream.sources[i].projection[j],false);
                identity=hash(hash(identity,uint32_t(vertex.source.object)),uint32_t(vertex.source.object>>32));
                identity=hash(hash(identity,uint32_t(vertex.source.primitive)),uint32_t(vertex.source.primitive>>32));
                model=hash(hash(model,uint32_t(vertex.model.object)),uint32_t(vertex.model.object>>32));
            }
            // Texture animation, OT bucket and alternating packet addresses
            // do not change the identity of a polygon's source vertices.
            stream.current[i].identity={model,identity};
        }
        stream.history.erase(std::remove_if(stream.history.begin(),stream.history.end(),[](const Stream::Raw& r) {
            return stream.sequence>r.sequence && stream.sequence-r.sequence>8;
        }),stream.history.end());
    }
    // Keep geometry culled at N until it actually leaves the interpolated
    // frustum. Both old-only and current faces use one canonical source table.
    retiring_unresolved=retiring_count=0;
    stream.union_geometry=stream.current;
    stream.retiring_endpoints=stream.older;
    stream.retiring_indices.assign(stream.older.size(),SIZE_MAX);
    std::unordered_map<uint64_t,std::unordered_set<uint64_t>> present;
    for(const auto& p:stream.current) present[p.identity.object].insert(p.identity.primitive);
    std::unordered_map<uint64_t,std::unordered_map<uint64_t,const PacketInterpolationTransform*>> models,points;
    for(const auto& v:stream.transforms) {
        models[v.model.object][v.model.primitive]=&v;
        points[v.source.object][v.source.primitive]=&v;
    }
    if(stream.old_epoch==stream.epoch) for(size_t i=0;i<stream.older.size();++i) {
        auto p=stream.older[i];
        if(!p.spatial || present[p.identity.object].count(p.identity.primitive)) continue;
        bool reconstructed=true;
        for(unsigned j=0;j<p.vertices;++j) {
            auto& v=p.vertex[j];
            const auto* point=points[v.source.object][v.source.primitive];
            const auto* model=models[v.model.object][v.model.primitive];
            if(!point && (!model || !model->affine_valid)) {reconstructed=false;break;}
            const auto& transform=*(point?point:model);
            for(unsigned d=0;d<3;++d) {
                v.view[d]=point?point->view[d]:model->affine[d*4+3];
                if(!point) for(unsigned k=0;k<3;++k) v.view[d]+=model->affine[d*4+k]*v.model_position[k];
            }
            v.h=transform.h;v.ofx=transform.ofx;v.ofy=transform.ofy;v.x_scale=transform.x_scale;
            if(v.view[2]>0) {
                v.screen[0]=v.ofx+v.h*v.x_scale*v.view[0]/v.view[2];
                v.screen[1]=v.ofy+v.h*v.view[1]/v.view[2];
            }

        }
        if(reconstructed) {
            stream.retiring_indices[i]=stream.union_geometry.size();
            stream.union_geometry.push_back(p);++retiring_count;
        } else ++retiring_unresolved;
    }
    PacketInterpolationPacket a{stream.old_epoch,stream.old_sequence,stream.older.data(),stream.older.size(),0};
    PacketInterpolationPacket b{stream.epoch,stream.sequence,stream.union_geometry.data(),stream.union_geometry.size(),0};
    a.transforms=stream.old_transforms.data();a.transform_count=stream.old_transforms.size();
    b.transforms=stream.transforms.data();b.transform_count=stream.transforms.size();
    a.source_mode=b.source_mode=stream.source_mode;
    if(!stream.failed && !packet_interpolation_pair(stream.pair,&a,&b,0.5)) stream.failed=true;
    {
        std::lock_guard<std::mutex> lock(stats_mutex);
        published=stream.failed?PacketInterpolationStats{}:packet_interpolation_stats(stream.pair);
        published_source_mode=stream.source_mode;
        published_sequence=stream.sequence;published_missing=stream.missing;
        published_count=stream.union_geometry.size();published_failed=stream.failed;
        published_retiring_count=retiring_count;published_retiring_unresolved=retiring_unresolved;
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
    stream.old_transforms.swap(stream.transforms);
    stream.transforms.clear();stream.invocations.clear();stream.model_invocations.clear();stream.ordinals.clear();stream.source_mode=false;
    stream.older.swap(stream.current);stream.old_sources.swap(stream.sources);
    stream.old_epoch=stream.epoch;stream.old_sequence=stream.sequence;
}
extern "C" void packet_interpolation_stream_end(void) {
    try {stream_end_impl();}
    catch(...) {
        packet_interpolation_stream_reset();
        std::lock_guard<std::mutex> lock(stats_mutex);
        published_failed=true;
    }
}
extern "C" void packet_interpolation_stream_reset(void) {
    stream.union_geometry.clear();stream.retiring_indices.clear();stream.retiring_endpoints.clear();
    stream.staging_addresses.clear();
    stream.transforms.clear();stream.old_transforms.clear();stream.invocations.clear();stream.model_invocations.clear();stream.ordinals.clear();stream.history.clear();
    stream.old_sources.clear();stream.sources.clear();
    stream.older.clear();stream.current.clear();stream.old_epoch=stream.old_sequence=0;
    std::lock_guard<std::mutex> lock(stats_mutex);
    published_source_mode=false;published={};published_sequence=published_missing=published_count=0;published_failed=false;
    published_retiring_count=published_retiring_unresolved=0;
    for(unsigned i=0;i<2;++i) {
        captured[i].clear();captured_sources[i].clear();captured_sequence[i]=0;
    }
    capture_requested.store(false);
}
extern "C" int packet_interpolation_stream_json(char *out,size_t capacity) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    return std::snprintf(out,capacity,
        "{\"status\":\"ok\",\"mode\":\"%s\",\"retiring\":%llu,\"retiring_unresolved\":%llu,\"sequence\":%llu,\"primitives\":%llu,"
        "\"matched\":%llu,\"unmatched\":%llu,\"disappeared\":%llu,\"ambiguous\":%llu,"
        "\"resolved_occurrence\":%llu,\"resolved_assignment\":%llu,\"appeared_candidates\":%llu,\"invalid\":%llu,\"partial_provenance\":%llu,\"duplicate_equal\":%llu,\"source_matched\":%llu,\"new_source\":%llu,\"source_unmatched\":%llu,\"source_ambiguous\":%llu,\"source_moving\":%llu,\"verified_appearance\":%llu,\"unexplained\":%llu,\"model_reconstructed\":%llu,\"sampled_primitives\":%llu,\"held_primitives\":%llu,\"snapped\":%d,\"failed\":%s}",
        published_source_mode?"vertex_source_audit":"candidate_audit",
        (unsigned long long)published_retiring_count,(unsigned long long)published_retiring_unresolved,
        (unsigned long long)published_sequence,(unsigned long long)published_count,
        (unsigned long long)published.matched,(unsigned long long)published.unmatched,
        (unsigned long long)published.disappeared,(unsigned long long)published.ambiguous,
        (unsigned long long)published.resolved_occurrence,(unsigned long long)published.resolved_assignment,
        (unsigned long long)published.appeared_candidates,(unsigned long long)published.invalid,(unsigned long long)published_missing,
        (unsigned long long)published.duplicate_equal,
        (unsigned long long)published.source_matched,(unsigned long long)published.new_source,
        (unsigned long long)published.source_unmatched,(unsigned long long)published.source_ambiguous,(unsigned long long)published.source_moving,(unsigned long long)published.verified_appearance,(unsigned long long)published.unexplained,(unsigned long long)published.model_reconstructed,(unsigned long long)published.sampled_primitives,(unsigned long long)published.held_primitives,published.snapped,published_failed?"true":"false");
}

extern "C" void packet_interpolation_capture_request(void) {
    capture_requested.store(true);
}
extern "C" int packet_interpolation_capture_json(char *out,size_t capacity,int older,size_t start) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    if(!out || !capacity) return -1;
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

extern "C" void packet_interpolation_set_rate(unsigned hz) {output_rate.store(hz);}
extern "C" unsigned packet_interpolation_rate(void) {unsigned rate=output_rate.load();return rate==UINT32_MAX?display_rate.load():rate;}
extern "C" void packet_interpolation_display_rate(double hz) {if(hz>=1 && hz<=1000) display_rate.store(unsigned(hz+0.5));}
extern "C" size_t packet_interpolation_stream_count(void) {return stream.older.size();}
extern "C" int packet_interpolation_stream_sample(double t) {
    try {stream.sampled.resize(stream.union_geometry.size());}
    catch(...) {return 0;}
    return packet_interpolation_sample(stream.pair,t,stream.sampled.data(),stream.sampled.size());
}
extern "C" const PacketInterpolationPrimitive *packet_interpolation_stream_vertex(size_t i) {
    return i<stream.sampled.size()?&stream.sampled[i]:nullptr;
}
extern "C" const PacketInterpolationPrimitive *packet_interpolation_stream_endpoint(size_t i) {
    return i<stream.older.size()?&stream.older[i]:nullptr;
}
extern "C" const PacketInterpolationPrimitive *packet_interpolation_stream_retiring(size_t i) {
    return i<stream.retiring_indices.size() && stream.retiring_indices[i]<stream.sampled.size()?
        &stream.sampled[stream.retiring_indices[i]]:nullptr;
}
extern "C" const PacketInterpolationPrimitive *packet_interpolation_stream_retiring_endpoint(size_t i) {
    return i<stream.retiring_endpoints.size()?&stream.retiring_endpoints[i]:nullptr;
}

extern "C" int packet_interpolation_stream_snapped(void) {return packet_interpolation_stats(stream.pair).snapped;}
