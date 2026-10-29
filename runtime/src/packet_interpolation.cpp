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
    bool ready=false;
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
        using Group=std::vector<size_t>;
        std::unordered_map<Key,Group,Hash> old_groups,new_groups;
        std::vector<bool> used(a->count,false);
        for(size_t i=0;i<a->count;++i) if(c->a[i].spatial) old_groups[key(c->a[i])].push_back(i);
        for(size_t i=0;i<b->count;++i) if(c->b[i].spatial) new_groups[key(c->b[i])].push_back(i);
        c->stats.snapped=a->epoch!=b->epoch || b->sequence<=a->sequence ||
                         a->discontinuity || b->discontinuity;
        auto accept=[&](size_t ni,size_t oi) {
            const auto& n=c->b[ni]; const auto& o=c->a[oi];
            if(!valid(n) || !valid(o) || n.vertices!=o.vertices) return false;
            c->matches[ni]=oi; used[oi]=true; ++c->stats.matched;
            if(max_motion>0) for(unsigned v=0;v<n.vertices;++v) {
                double distance=0;
                for(unsigned d=0;d<3;++d) {
                    double delta=n.vertex[v].view[d]-o.vertex[v].view[d];
                    distance+=delta*delta;
                }
                if(std::sqrt(distance)/n.vertex[v].view[2]>max_motion) c->stats.snapped=1;
            }
            return true;
        };
        for(const auto& entry:new_groups) {
            const auto& ng=entry.second;
            auto previous=old_groups.find(entry.first);
            if(previous==old_groups.end()) continue;
            const auto& og=previous->second;
            for(size_t i=1;i<ng.size();++i)
                if(same_geometry(c->b[ng[0]],c->b[ng[i]])) ++c->stats.duplicate_equal;
            if(ng.size()==og.size()) {
                bool resolved=true;
                for(size_t k=0;k<ng.size();++k) resolved=accept(ng[k],og[k])&&resolved;
                if(resolved && ng.size()>1) ++c->stats.resolved_occurrence;
                continue;
            }
            // Rectangular Hungarian assignment with private dummy columns.
            // Dummy cost dominates all allowed edges: first maximize the
            // number of gated matches, then minimize centroid displacement.
            // Stable input ordering and strict comparisons break ties.
            const bool new_rows=ng.size()<og.size();
            const auto& row_group=new_rows?ng:og;
            const auto& col_group=new_rows?og:ng;
            const size_t rows=row_group.size(),cols=col_group.size()+rows;
            const double gate=max_motion>0?max_motion:0.5;
            const double penalty=double(rows+1),forbidden=penalty*double(rows+2);
            auto cost=[&](size_t r,size_t col) {
                if(col>=col_group.size()) return penalty;
                const auto& n=c->b[new_rows?row_group[r]:col_group[col]];
                const auto& o=c->a[new_rows?col_group[col]:row_group[r]];
                if(!valid(n)||!valid(o)||n.vertices!=o.vertices) return forbidden;
                double distance=0,depth=0;
                for(unsigned d=0;d<3;++d) {
                    double nc=0,oc=0;
                    for(unsigned v=0;v<n.vertices;++v) {nc+=n.vertex[v].view[d];oc+=o.vertex[v].view[d];}
                    double delta=(nc-oc)/n.vertices;distance+=delta*delta;
                    if(d==2) depth=std::min(nc,oc)/n.vertices;
                }
                double normalized=distance/(depth*depth*gate*gate);
                return normalized<=1?normalized:forbidden;
            };
            std::vector<double> u(rows+1),v(cols+1);
            std::vector<size_t> assignment(cols+1),way(cols+1);
            for(size_t row=1;row<=rows;++row) {
                assignment[0]=row;size_t col=0;
                std::vector<double> best(cols+1,forbidden*double(rows+cols+1));
                std::vector<bool> visited(cols+1,false);
                do {
                    visited[col]=true;size_t r=assignment[col],next=0;
                    double delta=forbidden*double(rows+cols+1);
                    for(size_t j=1;j<=cols;++j) if(!visited[j]) {
                        double candidate=cost(r-1,j-1)-u[r]-v[j];
                        if(candidate<best[j]) {best[j]=candidate;way[j]=col;}
                        if(best[j]<delta) {delta=best[j];next=j;}
                    }
                    for(size_t j=0;j<=cols;++j) {
                        if(visited[j]) {u[assignment[j]]+=delta;v[j]-=delta;}
                        else best[j]-=delta;
                    }
                    col=next;
                } while(assignment[col]);
                do {size_t prev=way[col];assignment[col]=assignment[prev];col=prev;} while(col);
            }
            bool resolved=false;
            for(size_t col=1;col<=col_group.size();++col)
                if(assignment[col] && cost(assignment[col]-1,col-1)<penalty) {
                    const size_t r=assignment[col]-1;
                    resolved=accept(new_rows?row_group[r]:col_group[col-1],
                                    new_rows?col_group[col-1]:row_group[r])||resolved;
                }
            if(resolved) ++c->stats.resolved_assignment;
        }
        for(size_t i=0;i<b->count;++i) if(c->b[i].spatial && c->matches[i]==missing) {
            ++c->stats.unmatched;
            if(!valid(c->b[i])) ++c->stats.invalid;
            else if(!old_groups.count(key(c->b[i]))) ++c->stats.appeared_candidates;
            else ++c->stats.ambiguous;
        }
        for(size_t i=0;i<a->count;++i) if(c->a[i].spatial && !used[i]) ++c->stats.disappeared;
        c->ready=true;
        return 1;
    } catch(...) { *c=PacketInterpolation{}; return 0; }
}
extern "C" int packet_interpolation_sample(const PacketInterpolation *c,double t,
    PacketInterpolationPrimitive *out,size_t capacity) {
    if(!c || !c->ready || capacity<c->b.size() || (!out && !c->b.empty()) || !std::isfinite(t)) return 0;
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
