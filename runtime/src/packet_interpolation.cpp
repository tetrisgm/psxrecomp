#include "packet_interpolation.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
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
    std::vector<PacketInterpolationPrimitive> source_previous;
    std::vector<unsigned> source_masks;
    // Render-owned source slots. Sampling writes each slot once, then every
    // face reads exactly those bits; material attributes remain per face.
    std::vector<size_t> source_slots;
    std::vector<std::pair<size_t,unsigned>> source_owners;
    mutable std::vector<PacketInterpolationVertex> source_samples;
    bool source_mode=false;
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
        if(b->source_mode) {
            if((a->transform_count && !a->transforms) || (b->transform_count && !b->transforms)) return 0;
            c->source_mode=true;
            c->source_previous=c->b;
            c->source_masks.assign(b->count,0);
            struct Entry { const PacketInterpolationTransform *value; bool ambiguous; };
            std::unordered_map<Key,Entry,Hash> table;
            std::unordered_map<Key,Entry,Hash> models;
            for(size_t i=0;i<a->transform_count;++i) {
                const auto& v=a->transforms[i];
                auto model=models.emplace(Key{v.model.object,v.model.primitive},Entry{&v,false});
                if(!model.second && v.affine_valid && model.first->second.value->affine_valid)
                    for(unsigned d=0;d<12;++d)
                        if(model.first->second.value->affine[d]!=v.affine[d]) model.first->second.ambiguous=true;
                auto result=table.emplace(Key{v.source.object,v.source.primitive},Entry{&v,false});
                if(!result.second) for(unsigned d=0;d<3;++d)
                    if(result.first->second.value->view[d]!=v.view[d]) result.first->second.ambiguous=true;
            }
            // A reused matrix slot's invocation ordinal may shift when a
            // preceding object disappears. Shared pre-cull source identities
            // establish the whole invocation's correspondence instead.
            std::unordered_map<Key,Entry,Hash> correspondence;
            for(size_t i=0;i<b->transform_count;++i) {
                const auto& v=b->transforms[i];
                auto old=table.find({v.source.object,v.source.primitive});
                if(old==table.end() || old->second.ambiguous) continue;
                auto result=correspondence.emplace(Key{v.model.object,v.model.primitive},old->second);
                if(!result.second) {
                    const auto& first=result.first->second.value->model;
                    const auto& next=old->second.value->model;
                    if(first.object!=next.object || first.primitive!=next.primitive)
                        result.first->second.ambiguous=true;
                }
            }
            c->stats.snapped=a->epoch!=b->epoch || b->sequence<=a->sequence || a->discontinuity || b->discontinuity;
            size_t local_cuts=0;
            for(size_t i=0;i<c->b.size();++i) {
                const auto& p=c->b[i]; if(!p.spatial) continue;
                if(p.vertices<3 || p.vertices>4) {++c->stats.invalid;continue;}
                for(unsigned j=0;j<p.vertices;++j) {
                    const auto& v=p.vertex[j];
                    bool finite=std::isfinite(v.h)&&v.h>=0&&std::isfinite(v.ofx)&&
                        std::isfinite(v.ofy)&&std::isfinite(v.x_scale)&&v.x_scale>0;
                    for(double x:v.view) finite=finite&&std::isfinite(x);
                    if(!finite || (!v.source.object && !v.source.primitive)) {
                        ++c->stats.invalid;++c->stats.source_unmatched;continue;
                    }
                    auto found=table.find({v.source.object,v.source.primitive});
                    if(found==table.end()) {
                        auto model=models.find({v.model.object,v.model.primitive});
                        const auto mapped=correspondence.find({v.model.object,v.model.primitive});
                        if(mapped!=correspondence.end() && mapped->second.ambiguous) {
                            ++c->stats.unexplained;++c->stats.source_unmatched;continue;
                        }
                        if(mapped!=correspondence.end() && !mapped->second.ambiguous) {
                            const auto& identity=mapped->second.value->model;
                            model=models.find({identity.object,identity.primitive});
                        }
                        if(model!=models.end() && !model->second.ambiguous && model->second.value->affine_valid) {
                            // A streamed/generated vertex was not emitted last time, but
                            // its model invocation was. Apply that invocation's exact
                            // GTE matrix to this model-space point before interpolating.
                            // No nearest-neighbour guess or false appearance exemption.
                            const auto& m=*model->second.value;
                            if(m.h>0 && m.x_scale>0) {
                                auto& previous=c->source_previous[i].vertex[j];
                                previous.h=m.h;previous.ofx=m.ofx;previous.ofy=m.ofy;previous.x_scale=m.x_scale;
                            }
                            double distance=0;
                            for(unsigned d=0;d<3;++d) {
                                double value=m.affine[4*d+3];
                                for(unsigned k=0;k<3;++k) value+=m.affine[4*d+k]*v.model_position[k];
                                c->source_previous[i].vertex[j].view[d]=value;
                                double delta=value-v.view[d];distance+=delta*delta;
                            }
                            c->source_masks[i]|=1u<<j;++c->stats.source_matched;++c->stats.model_reconstructed;
                            if(distance>1e-12) ++c->stats.source_moving;
                            if(max_motion>0 && std::sqrt(distance)>max_motion*std::max({1.0,std::abs(v.view[2]),v.h})) {
                                c->source_masks[i]&=~(1u<<j);++local_cuts;
                            }
                        } else {
                            ++c->stats.new_source;
                            if((v.model.object || v.model.primitive) && model==models.end()) ++c->stats.verified_appearance;
                            else {++c->stats.unexplained;++c->stats.source_unmatched;}
                        }
                        continue;
                    }
                    if(found->second.ambiguous) {++c->stats.source_ambiguous;++c->stats.source_unmatched;continue;}
                    const auto& old=*found->second.value;
                    if(old.h>0 && old.x_scale>0) {
                        auto& previous=c->source_previous[i].vertex[j];
                        previous.h=old.h;previous.ofx=old.ofx;previous.ofy=old.ofy;previous.x_scale=old.x_scale;
                    }
                    double distance=0;
                    for(unsigned d=0;d<3;++d) {
                        if(!std::isfinite(old.view[d])) finite=false;
                        double delta=v.view[d]-old.view[d];distance+=delta*delta;
                        c->source_previous[i].vertex[j].view[d]=old.view[d];
                    }
                    if(!finite) {++c->stats.invalid;++c->stats.source_unmatched;continue;}
                    c->source_masks[i]|=1u<<j;++c->stats.source_matched;
                    if(distance>1e-12) ++c->stats.source_moving;
                    if(max_motion>0 && std::sqrt(distance)>max_motion*std::max({1.0,std::abs(v.view[2]),v.h})) {
                                c->source_masks[i]&=~(1u<<j);++local_cuts;
                            }
                }
                if(c->source_masks[i]==(1u<<p.vertices)-1) ++c->stats.matched;
            }
            // Local discontinuities (respawns, changing procedural geometry)
            // take N. A scene-wide discontinuity snaps metadata and all geometry.
            if(local_cuts && local_cuts*2>c->stats.source_matched) c->stats.snapped=1;
            std::unordered_map<Key,size_t,Hash> slots;
            c->source_slots.assign(c->b.size()*4,missing);
            std::vector<size_t> parent(c->b.size());
            std::unordered_map<Key,size_t,Hash> edges;
            for(size_t i=0;i<parent.size();++i) parent[i]=i;
            auto root=[&](size_t i) {while(parent[i]!=i) {parent[i]=parent[parent[i]];i=parent[i];} return i;};
            for(size_t i=0;i<c->b.size();++i) {
                const auto& p=c->b[i];
                // Integer screen-edge adjacency includes GTE-less neighbours.
                // Do not move one side of such an edge while holding the other.
                auto screen_key=[](const PacketInterpolationVertex& v) {
                    return (uint64_t(uint32_t(int32_t(v.screen[0])))<<32)|uint32_t(int32_t(v.screen[1]));
                };
                for(unsigned j=0;j<p.vertices && j<4;++j) {
                    uint64_t a=screen_key(p.vertex[j]),b=screen_key(p.vertex[(j+1)%p.vertices]);
                    if(a==b) continue;
                    auto edge=edges.emplace(Key{std::min(a,b),std::max(a,b)},i);
                    if(!edge.second) parent[root(i)]=root(edge.first->second);
                }
                if(!p.spatial) continue;
                for(unsigned j=0;j<p.vertices && j<4;++j) {
                    const auto& v=p.vertex[j];
                    if(!v.source.object && !v.source.primitive) continue;
                    auto entry=slots.emplace(Key{v.source.object,v.source.primitive},c->source_owners.size());
                    if(entry.second) c->source_owners.emplace_back(i,j);
                    else parent[root(i)]=root(c->source_owners[entry.first->second].first);
                    c->source_slots[i*4+j]=entry.first->second;
                }
            }
            // A cut or unsupported point holds the entire connected mesh.
            // Mixing N and N-1 within an edge is never a valid fallback.
            std::vector<bool> hold(c->b.size(),false);
            for(size_t i=0;i<c->b.size();++i)
                if(!c->b[i].spatial || c->source_masks[i]!=(1u<<c->b[i].vertices)-1) hold[root(i)]=true;
            for(size_t i=0;i<c->b.size();++i) if(hold[root(i)]) c->source_masks[i]=0;
            c->source_samples.resize(c->source_owners.size());
            c->stats.unmatched=c->stats.source_unmatched;
            c->stats.ambiguous=c->stats.source_ambiguous;
            c->ready=true;return 1;
        }
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
    if(c->source_mode && t!=1 && !c->stats.snapped) {
        for(size_t slot=0;slot<c->source_owners.size();++slot) {
            const auto [i,v]=c->source_owners[slot];
            auto& p=c->source_samples[slot];p=c->b[i].vertex[v];
            if(c->source_masks[i]&(1u<<v)) {
                const auto& old=c->source_previous[i].vertex[v];
                for(unsigned d=0;d<3;++d) p.view[d]=old.view[d]+t*(p.view[d]-old.view[d]);
                p.h=old.h+t*(p.h-old.h);p.ofx=old.ofx+t*(p.ofx-old.ofx);
                p.ofy=old.ofy+t*(p.ofy-old.ofy);p.x_scale=old.x_scale+t*(p.x_scale-old.x_scale);
            }
            p.homogeneous[0]=p.ofx*p.view[2]+p.h*p.x_scale*p.view[0];
            p.homogeneous[1]=p.ofy*p.view[2]+p.h*p.view[1];p.homogeneous[2]=p.view[2];
            if(p.view[2]>0) {p.screen[0]=p.homogeneous[0]/p.view[2];p.screen[1]=p.homogeneous[1]/p.view[2];}
        }
    }
    for(size_t i=0;i<c->b.size();++i) {
        out[i]=c->b[i];
        if(c->source_mode) {
            if(!out[i].spatial) continue;
            if(t==1 || c->stats.snapped || !c->source_masks[i]) continue;
            out[i].interpolated=1;
            for(unsigned v=0;v<out[i].vertices && v<4;++v) {
                auto& p=out[i].vertex[v];
                const size_t slot=c->source_slots[i*4+v];
                if(slot!=std::numeric_limits<size_t>::max()) {
                    const auto& value=c->source_samples[slot];
                    for(unsigned d=0;d<3;++d) {p.view[d]=value.view[d];p.homogeneous[d]=value.homogeneous[d];}
                    p.h=value.h;p.ofx=value.ofx;p.ofy=value.ofy;p.x_scale=value.x_scale;
                    p.screen[0]=value.screen[0];p.screen[1]=value.screen[1];
                    continue;
                }
                if(!c->stats.snapped && t!=1 && (c->source_masks[i]&(1u<<v))) {
                    for(unsigned d=0;d<3;++d) p.view[d]=c->source_previous[i].vertex[v].view[d]+t*(p.view[d]-c->source_previous[i].vertex[v].view[d]);
                    const auto& old=c->source_previous[i].vertex[v];
                    p.h=old.h+t*(p.h-old.h);p.ofx=old.ofx+t*(p.ofx-old.ofx);
                    p.ofy=old.ofy+t*(p.ofy-old.ofy);p.x_scale=old.x_scale+t*(p.x_scale-old.x_scale);
                }
                p.homogeneous[0]=p.ofx*p.view[2]+p.h*p.x_scale*p.view[0];
                p.homogeneous[1]=p.ofy*p.view[2]+p.h*p.view[1];
                p.homogeneous[2]=p.view[2];
                // Nonpositive depth is valid geometry. The renderer clips
                // homogeneous coordinates before performing the divide.
                if(p.view[2]>0 && t!=1 && !c->stats.snapped && (c->source_masks[i]&(1u<<v))) {
                    p.screen[0]=p.homogeneous[0]/p.view[2];
                    p.screen[1]=p.homogeneous[1]/p.view[2];
                }
            }
            continue;
        }
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
extern "C" size_t packet_interpolation_clip(const PacketInterpolationPrimitive *p,
    double near_depth,PacketInterpolationVertex *out,size_t capacity) {
    if(!p || !out || p->vertices<3 || p->vertices>4 || capacity<p->vertices+2 ||
       !std::isfinite(near_depth) || near_depth<=0) return 0;
    for(unsigned i=0;i<p->vertices;++i) {
        const auto& v=p->vertex[i];
        for(double x:v.view) if(!std::isfinite(x)) return 0;
        if(!std::isfinite(v.h) || !std::isfinite(v.x_scale) ||
           !std::isfinite(v.ofx) || !std::isfinite(v.ofy)) return 0;
    }
    size_t count=0;
    for(unsigned i=0;i<p->vertices;++i) {
        const auto& a=p->vertex[(i+p->vertices-1)%p->vertices];
        const auto& b=p->vertex[i];
        bool ai=a.view[2]>=near_depth,bi=b.view[2]>=near_depth;
        if(ai!=bi) {
            double t=(near_depth-a.view[2])/(b.view[2]-a.view[2]);
            auto v=b;v.source={};
            for(unsigned d=0;d<3;++d) v.view[d]=a.view[d]+t*(b.view[d]-a.view[d]);
            v.view[2]=near_depth;
            v.h=a.h+t*(b.h-a.h);v.ofx=a.ofx+t*(b.ofx-a.ofx);
            v.ofy=a.ofy+t*(b.ofy-a.ofy);v.x_scale=a.x_scale+t*(b.x_scale-a.x_scale);
            for(unsigned d=0;d<2;++d) v.uv[d]=a.uv[d]+t*(b.uv[d]-a.uv[d]);
            v.colour=0;
            for(unsigned shift=0;shift<32;shift+=8) {
                double ac=(a.colour>>shift)&255,bc=(b.colour>>shift)&255;
                v.colour|=uint32_t(std::lround(ac+t*(bc-ac)))<<shift;
            }
            out[count++]=v;
        }
        if(bi) out[count++]=b;
    }
    for(size_t i=0;i<count;++i) {
        auto& v=out[i];
        v.homogeneous[0]=v.ofx*v.view[2]+v.h*v.x_scale*v.view[0];
        v.homogeneous[1]=v.ofy*v.view[2]+v.h*v.view[1];v.homogeneous[2]=v.view[2];
        v.screen[0]=v.homogeneous[0]/v.view[2];v.screen[1]=v.homogeneous[1]/v.view[2];
    }
    return count;
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
