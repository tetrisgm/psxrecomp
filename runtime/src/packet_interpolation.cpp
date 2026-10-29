#include "packet_interpolation.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace {
struct Key {
    uint64_t object, primitive;
    bool operator==(const Key& b) const { return object==b.object && primitive==b.primitive; }
};
struct Hash {
    size_t operator()(const Key& k) const {
        return size_t(k.object ^ (k.primitive + 0x9e3779b97f4a7c15ULL +
                                   (k.object<<6) + (k.object>>2)));
    }
};
Key key(const PacketInterpolationPrimitive& p) { return {p.identity.object,p.identity.primitive}; }
bool valid(const PacketInterpolationPrimitive& p) {
    if(p.vertices<3 || p.vertices>4 || (!p.identity.object && !p.identity.primitive)) return false;
    for(unsigned i=0;i<p.vertices;++i) {
        const auto& v=p.vertex[i];
        for(double c:v.view) if(!std::isfinite(c)) return false;
        if(v.view[2]<=0 || !std::isfinite(v.h) || v.h<=0 ||
           !std::isfinite(v.ofx) || !std::isfinite(v.ofy) ||
           !std::isfinite(v.x_scale) || v.x_scale<=0 ||
           !std::isfinite(v.screen[0]) || !std::isfinite(v.screen[1])) return false;
    }
    return true;
}
bool same_geometry(const PacketInterpolationPrimitive& a,const PacketInterpolationPrimitive& b) {
    if(a.vertices!=b.vertices) return false;
    for(unsigned i=0;i<a.vertices && i<4;++i) {
        const auto& av=a.vertex[i];const auto& bv=b.vertex[i];
        for(unsigned d=0;d<3;++d) if(av.view[d]!=bv.view[d]) return false;
        if(av.h!=bv.h || av.ofx!=bv.ofx || av.ofy!=bv.ofy || av.x_scale!=bv.x_scale) return false;
    }
    return true;
}
}
struct PacketInterpolation {
    std::vector<PacketInterpolationPrimitive> a,b;
    std::vector<size_t> matches;
    PacketInterpolationStats stats{};
};
extern "C" PacketInterpolation *packet_interpolation_create(void) {
    try { return new PacketInterpolation; } catch(...) { return nullptr; }
}
extern "C" void packet_interpolation_destroy(PacketInterpolation *c) { delete c; }
extern "C" int packet_interpolation_pair(PacketInterpolation *c,
    const PacketInterpolationPacket *a,const PacketInterpolationPacket *b,double max_motion) {
    if(!c) return 0;
    *c=PacketInterpolation{};
    if(!a || !b || (a->count && !a->primitives) || (b->count && !b->primitives) ||
       !std::isfinite(max_motion)) return 0;
    try {
        if(a->count) c->a.assign(a->primitives,a->primitives+a->count);
        if(b->count) c->b.assign(b->primitives,b->primitives+b->count);
        const size_t missing=std::numeric_limits<size_t>::max();
        c->matches.assign(b->count,missing);
        std::unordered_map<Key,size_t,Hash> old_index,new_count,new_first;
        for(size_t i=0;i<a->count;++i) if(c->a[i].spatial) {
            auto pair=old_index.emplace(key(c->a[i]),i);
            if(!pair.second) pair.first->second=missing;
        }
        for(size_t i=0;i<c->b.size();++i) if(c->b[i].spatial) {
            const auto& p=c->b[i];++new_count[key(p)];
            auto inserted=new_first.emplace(key(p),i);
            if(!inserted.second && same_geometry(c->b[inserted.first->second],p)) ++c->stats.duplicate_equal;
        }
        c->stats.snapped=a->epoch!=b->epoch || b->sequence<=a->sequence ||
                         a->discontinuity || b->discontinuity;
        for(size_t i=0;i<b->count;++i) {
            const auto& n=c->b[i];
            if(!n.spatial) continue;
            auto old=old_index.find(key(n));
            if(new_count[key(n)]!=1 || (old!=old_index.end() && old->second==missing)) {
                ++c->stats.ambiguous; ++c->stats.unmatched; continue;
            }
            if(!valid(n)) { ++c->stats.invalid; ++c->stats.unmatched; continue; }
            if(old==old_index.end()) { ++c->stats.unmatched; continue; }
            const auto& o=c->a[old->second];
            if(!valid(o) || o.vertices!=n.vertices) {
                ++c->stats.invalid; ++c->stats.unmatched; continue;
            }
            c->matches[i]=old->second;
            ++c->stats.matched;
            if(max_motion>0) for(unsigned v=0;v<n.vertices;++v) {
                double distance=0;
                for(unsigned d=0;d<3;++d) {
                    double delta=n.vertex[v].view[d]-o.vertex[v].view[d];
                    distance+=delta*delta;
                }
                if(std::sqrt(distance)/n.vertex[v].view[2]>max_motion) c->stats.snapped=1;
            }
        }
        for(const auto& p:c->a) if(p.spatial && !new_count.count(key(p))) ++c->stats.disappeared;
        return 1;
    } catch(...) { *c=PacketInterpolation{}; return 0; }
}
extern "C" int packet_interpolation_sample(const PacketInterpolation *c,double t,
    PacketInterpolationPrimitive *out,size_t capacity) {
    if(!c || capacity<c->b.size() || (!out && !c->b.empty()) || !std::isfinite(t)) return 0;
    t=std::clamp(t,0.0,1.0);
    for(size_t i=0;i<c->b.size();++i) {
        out[i]=c->b[i];
        if(c->stats.snapped || t==1 || c->matches[i]==std::numeric_limits<size_t>::max()) continue;
        const auto& a=c->a[c->matches[i]];
        const auto& b=c->b[i];
        for(unsigned v=0;v<b.vertices;++v) {
            auto& p=out[i].vertex[v];
            const auto& av=a.vertex[v]; const auto& bv=b.vertex[v];
            for(unsigned d=0;d<3;++d) p.view[d]=av.view[d]+t*(bv.view[d]-av.view[d]);
            p.h=av.h+t*(bv.h-av.h); p.ofx=av.ofx+t*(bv.ofx-av.ofx);
            p.ofy=av.ofy+t*(bv.ofy-av.ofy); p.x_scale=av.x_scale+t*(bv.x_scale-av.x_scale);
            /* Interpolate the renderer's residual too: draw-origin changes
             * across PS1 double buffers must not leak into the motion. The
             * caller supplies display-relative screen endpoints. */
            const double ax=av.ofx+av.h*av.x_scale*av.view[0]/av.view[2];
            const double ay=av.ofy+av.h*av.view[1]/av.view[2];
            const double bx=bv.ofx+bv.h*bv.x_scale*bv.view[0]/bv.view[2];
            const double by=bv.ofy+bv.h*bv.view[1]/bv.view[2];
            p.screen[0]=p.ofx+p.h*p.x_scale*p.view[0]/p.view[2]+
                (1-t)*(av.screen[0]-ax)+t*(bv.screen[0]-bx);
            p.screen[1]=p.ofy+p.h*p.view[1]/p.view[2]+
                (1-t)*(av.screen[1]-ay)+t*(bv.screen[1]-by);
        }
    }
    return 1;
}
extern "C" PacketInterpolationStats packet_interpolation_stats(const PacketInterpolation *c) {
    return c?c->stats:PacketInterpolationStats{};
}
extern "C" uint64_t packet_interpolation_deadline(uint64_t start,uint64_t index,uint32_t num,uint32_t den) {
    if(!num || !den) return UINT64_MAX;
    const uint64_t scale=uint64_t(den)*1000000000ULL;
    const uint64_t whole=index/num, remainder=index%num;
    if(whole>(UINT64_MAX-start)/scale) return UINT64_MAX;
    const uint64_t base=start+whole*scale;
    // Splitting scale avoids overflow even for the largest 32-bit rates.
    const uint64_t tail=remainder*(scale/num)+(remainder*(scale%num))/num;
    return tail>UINT64_MAX-base?UINT64_MAX:base+tail;
}
