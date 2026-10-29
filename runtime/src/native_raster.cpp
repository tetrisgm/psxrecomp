#include "native_raster.h"
#include "gpu_sw_renderer.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
constexpr size_t vram_words = 1024 * 512;
constexpr size_t queue_budget = 4 * 1024 * 1024;
enum class Kind { Draw, Packet, Read, Snapshot, Restore, Observe, Visit };
struct Job {
    Kind kind = Kind::Draw;
    NativeRasterCommand command{};
    NativeRasterState state{};
    std::array<uint32_t,12> packet{};
    std::vector<uint16_t> upload;
    uint16_t *destination = nullptr;
    uint64_t *dirty = nullptr;
    std::promise<void> *completion = nullptr;
    SwNativeTextureState *texture_destination = nullptr;
    std::unique_ptr<SwNativeTextureState> texture_source;
    NativeRasterObserver observer = nullptr;
    NativeRasterVisit visit = nullptr;
    void *user = nullptr;
    size_t bytes() const { return sizeof(Job) + upload.size() * sizeof(uint16_t)
                                  +(texture_source?sizeof(SwNativeTextureState):0); }
};
int coordinate(uint32_t word) { return int(word&1023)-int(word&1024); }
uint16_t color555(uint32_t rgb) {
    return uint16_t(((rgb>>3)&31)|((rgb>>6)&992)|((rgb>>9)&31744));
}
size_t packet_size(unsigned op) {
    if(op==2) return 3;
    if(op>=0x20 && op<0x40) {
        const size_t n=(op&8)?4:3;
        return 1+n*(1+!!(op&4))+((op&16)?n-1:0);
    }
    if(op>=0x40 && op<0x60 && !(op&8)) return (op&16)?4:3;
    if(op>=0x60 && op<0x80) return 2+!!(op&4)+((op&24)==0);
    if(op>=0x80 && op<0xa0) return 4;
    return 0;
}
void packet(const Job &job) {
    const auto &s=job.state;
    const auto *p=job.packet.data();
    const unsigned op=p[0]>>24;
    sw_set_draw_area(s.left,s.top,s.right,s.bottom);
    sw_set_mask_bits(s.set_mask,s.check_mask);
    sw_set_texture_window(s.window);
    sw_native_set_skip_field(s.skip_field);
    unsigned page=s.page,clut=0;
    if(op==2) {
        sw_fill_rect(p[1]&0x3f0,(p[1]>>16)&511,((p[2]&1023)+15)&~15,
                     (p[2]>>16)&511,color555(p[0]));
    } else if(op>=0x80) {
        sw_native_copy(p[1]&1023,(p[1]>>16)&511,p[2]&1023,(p[2]>>16)&511,
                     ((p[3]-1)&1023)+1,(((p[3]>>16)-1)&511)+1);
    } else if(op<0x40) {
        SwNativeVertex v[4]{};
        unsigned n=(op&8)?4:3,at=1;
        for(unsigned i=0;i<n;++i) {
            v[i].rgb=(i && (op&16))?p[at++]:p[0];
            uint32_t xy=p[at++];
            v[i].x=coordinate(xy)+s.offset_x;
            v[i].y=coordinate(xy>>16)+s.offset_y;
            if(op&4) {
                uint32_t uv=p[at++];
                v[i].u=uv&255;v[i].v=(uv>>8)&255;
                if(i==0) clut=uv>>16;
                if(i==1) page=(page&~511u)|((uv>>16)&511);
            }
        }
        sw_set_semi_transparency(!!(op&2),(page>>5)&3);
        unsigned flags=((op&4)?1:0)|((op&1)?2:0)|((op&16)?4:0)|((page&512)?8:0);
        sw_draw_native_triangle(v,(clut&63)*16,(clut>>6)&511,page,flags);
        if(n==4) {
            SwNativeVertex second[3]={v[1],v[2],v[3]};
            sw_draw_native_triangle(second,(clut&63)*16,(clut>>6)&511,page,flags);
        }
    } else if(op<0x60) {
        unsigned last=(op&16)?3:2;
        SwNativeVertex v[2]={{coordinate(p[1])+s.offset_x,coordinate(p[1]>>16)+s.offset_y,p[0],0,0},
                            {coordinate(p[last])+s.offset_x,coordinate(p[last]>>16)+s.offset_y,
                             (op&16)?p[2]:p[0],0,0}};
        sw_set_semi_transparency(!!(op&2),(page>>5)&3);
        sw_draw_native_line(v,((op&16)?4:0)|((page&512)?8:0));
    } else {
        int w,h,u=0,v=0;
        if(op&4) { u=p[2]&255;v=(p[2]>>8)&255;clut=p[2]>>16; }
        switch(op&24) {
        case 8:w=h=1;break;
        case 16:w=h=8;break;
        case 24:w=h=16;break;
        default:{uint32_t wh=p[(op&4)?3:2];w=wh&1023;h=(wh>>16)&511;break;}
        }
        sw_set_semi_transparency(!!(op&2),(page>>5)&3);
        sw_draw_native_rect(coordinate(uint32_t(coordinate(p[1])+s.offset_x)),
            coordinate(uint32_t(coordinate(p[1]>>16)+s.offset_y)),
            w,h,p[0],u,v,(clut&63)*16,(clut>>6)&511,page,
            ((op&4)?1:0)|((op&1)?2:0)|((page&4096)?16:0)|((page&8192)?32:0));
    }
}
void draw(const Job &j) {
    const auto *a = j.command.args;
    switch (j.command.op) {
    case NR_AREA: sw_set_draw_area(a[0],a[1],a[2],a[3]); break;
    case NR_OFFSET: sw_set_draw_offset(a[0],a[1]); break;
    case NR_SEMI: sw_set_semi_transparency(a[0],a[1]); break;
    case NR_MASK: sw_set_mask_bits(a[0],a[1]); break;
    case NR_WINDOW: sw_set_texture_window(a[0]); break;
    case NR_MODULATION: sw_set_color_modulation(a[0],a[1],a[2],a[3]); break;
    case NR_FILL: sw_fill_rect(a[0],a[1],a[2],a[3],a[4]); break;
    case NR_COPY: sw_native_copy(a[0],a[1],a[2],a[3],a[4],a[5]); break;
    case NR_FLAT_TRIANGLE: sw_draw_flat_triangle(a[0],a[1],a[2],a[3],a[4],a[5],a[6]); break;
    case NR_GOURAUD_TRIANGLE: sw_draw_gouraud_triangle(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8]); break;
    case NR_TEXTURED_TRIANGLE: sw_draw_textured_triangle(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11],a[12],a[13],a[14]); break;
    case NR_SHADED_TEXTURED_TRIANGLE: sw_draw_shaded_textured_triangle(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11],a[12],a[13],a[14],a[15],a[16],a[17],a[18]); break;
    case NR_FLAT_RECT: sw_draw_flat_rect(a[0],a[1],a[2],a[3],a[4]); break;
    case NR_TEXTURED_RECT: sw_draw_textured_rect(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8]); break;
    case NR_TEXTURED_RECT_SCALED: sw_draw_textured_rect_scaled(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10]); break;
    case NR_LINE: sw_draw_line(a[0],a[1],a[2],a[3],a[4]); break;
    case NR_SHADED_LINE: sw_draw_shaded_line(a[0],a[1],a[2],a[3],a[4],a[5]); break;
    case NR_WRITE: sw_native_cache_clear(0); sw_vram_write(a[0],a[1],a[2]); break;
    case NR_UPLOAD: sw_native_cache_clear(0); sw_vram_transfer_in(a[0],a[1],a[2],a[3],j.upload.data()); break;
    case NR_CPU_UPLOAD:
        if(a[4]==0) sw_native_cache_clear(0);
        for (size_t i = 0; i < j.upload.size(); ++i) {
            const size_t offset = size_t(a[4]) + i;
            const int x = (a[0] + int(offset % size_t(a[2]))) & 1023;
            const int y = (a[1] + int(offset / size_t(a[2]))) & 511;
            if (a[6] && (sw_vram_read(x,y) & 0x8000)) continue;
            sw_vram_write(x,y,j.upload[i] | (a[5] ? 0x8000 : 0));
        }
        break;
    case NR_NATIVE_TRIANGLE: {
        SwNativeVertex vertices[3];
        for (int i=0;i<3;++i)
            vertices[i]={a[i*5],a[i*5+1],uint32_t(a[i*5+2]),a[i*5+3],a[i*5+4]};
        sw_draw_native_triangle(vertices,a[15],a[16],a[17],unsigned(a[18]));
        break;
    }
    case NR_NATIVE_LINE: {
        SwNativeVertex vertices[2]={{a[0],a[1],uint32_t(a[2]),0,0},
                                    {a[3],a[4],uint32_t(a[5]),0,0}};
        sw_draw_native_line(vertices,unsigned(a[6]));
        break;
    }
    case NR_NATIVE_RECT:
        sw_draw_native_rect(a[0],a[1],a[2],a[3],uint32_t(a[4]),a[5],a[6],
                            a[7],a[8],a[9],unsigned(a[10]));
        break;
    case NR_CACHE_CLEAR: sw_native_cache_clear(a[0]); break;
    }
}
}

struct NativeRaster {
    std::array<uint16_t, vram_words> vram{};
    SwRendererContext *context = nullptr;
    std::mutex mutex;
    std::condition_variable available, space;
    std::deque<Job> queue;
    size_t bytes = 0;
    bool stop = false;
    std::atomic<uint64_t> completed{0};
    std::thread thread;
    /* Producer-only read cache. Any queued mutation invalidates it before
     * publication; reads can therefore reuse a completed row without another
     * worker wake, including GPUREAD's halfword-at-a-time interface. */
    std::array<uint16_t,1024> read_row{};
    int read_row_y = -1;
    NativeRasterObserver observer = nullptr;
    void *observer_user = nullptr;

    void push(Job job) {
        if(job.kind==Kind::Draw || job.kind==Kind::Packet || job.kind==Kind::Restore)
            read_row_y=-1;
        const size_t cost = job.bytes();
        std::unique_lock<std::mutex> lock(mutex);
        space.wait(lock, [&] { return bytes + cost <= queue_budget; });
        queue.push_back(std::move(job));
        bytes += cost;
        lock.unlock();
        available.notify_one();
    }
    void run() {
        sw_context_bind(context);
        sw_renderer_init(vram.data());
        sw_set_faithful_authority(1);
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                available.wait(lock, [&] { return stop || !queue.empty(); });
                if (queue.empty()) break;
                const size_t cost = queue.front().bytes();
                job = std::move(queue.front());
                queue.pop_front();
                bytes -= cost;
            }
            space.notify_one();
            const auto *a = job.command.args;
            const bool mutation = job.kind==Kind::Draw || job.kind==Kind::Packet || job.kind==Kind::Restore;
            if(mutation) sw_context_begin_write_observation(observer!=nullptr);
            NativeRasterEvent event{};
            if (observer && mutation) {
                event.before=1;event.restore=job.kind==Kind::Restore;
                if(job.kind==Kind::Draw) event.command=&job.command;
                if(job.kind==Kind::Packet) {
                    event.state=&job.state;event.packet=job.packet.data();
                    event.packet_words=packet_size(job.packet[0]>>24);
                }
                event.upload=job.upload.data();event.upload_words=job.upload.size();
                event.vram=vram.data();
                observer(observer_user,&event);
            }
            switch (job.kind) {
            case Kind::Draw: draw(job); break;
            case Kind::Packet: packet(job); break;
            case Kind::Read: sw_vram_transfer_out(a[0],a[1],a[2],a[3],job.destination); break;
            case Kind::Snapshot:
                std::memcpy(job.destination,vram.data(),sizeof(vram));
                if(job.dirty) sw_context_take_dirty(job.dirty);
                if(job.texture_destination) sw_native_texture_save(job.texture_destination);
                break;
            case Kind::Restore:
                sw_vram_transfer_in(0,0,1024,512,job.upload.data());
                sw_native_cache_clear(1);
                if(job.texture_source) sw_native_texture_load(job.texture_source.get());
                break;
            case Kind::Observe: observer=job.observer;observer_user=job.user;break;
            case Kind::Visit: job.visit(job.user,vram.data());break;
            }
            if(observer && mutation) {
                event.before=0;
                event.has_writes=sw_context_written_bounds(event.written_bounds);
                observer(observer_user,&event);
            }
            completed.fetch_add(1, std::memory_order_release);
            if (job.completion) job.completion->set_value();
        }
        sw_context_bind(nullptr);
    }
};

extern "C" NativeRaster *native_raster_create(const uint16_t *initial) {
    try {
        auto r = std::make_unique<NativeRaster>();
        r->context = sw_context_create();
        if (!r->context) return nullptr;
        if (initial) std::memcpy(r->vram.data(),initial,sizeof(r->vram));
        try { r->thread = std::thread(&NativeRaster::run,r.get()); }
        catch (...) { sw_context_destroy(r->context); throw; }
        return r.release();
    } catch (...) { return nullptr; }
}
extern "C" void native_raster_destroy(NativeRaster *r) {
    if (!r) return;
    {
        std::lock_guard<std::mutex> lock(r->mutex);
        r->stop = true;
    }
    r->available.notify_one();
    r->thread.join();
    sw_context_destroy(r->context);
    delete r;
}
extern "C" int native_raster_submit(NativeRaster *r, const NativeRasterCommand *c,
                                     const uint16_t *upload, size_t words) {
    if (!r || !c || c->op < NR_AREA || c->op > NR_CACHE_CLEAR) return 0;
    if (c->op == NR_UPLOAD || c->op == NR_CPU_UPLOAD) {
        if (c->args[2] <= 0 || c->args[2] > 1024 || c->args[3] <= 0 || c->args[3] > 512 ||
            !upload || !words) return 0;
        const size_t total = size_t(c->args[2]) * size_t(c->args[3]);
        if (c->op == NR_UPLOAD) { if (words != total) return 0; }
        else if (c->args[4] < 0 || size_t(c->args[4]) >= total ||
                 words > total - size_t(c->args[4])) return 0;
    } else if (words) return 0;
    try {
        Job job;
        job.command = *c;
        if (words) job.upload.assign(upload,upload+words);
        r->push(std::move(job));
        return 1;
    } catch (...) { return 0; }
}
extern "C" int native_raster_gp0(NativeRaster *r,const NativeRasterState *state,
                                  const uint32_t *words,size_t count) {
    if(!r || !state || !words || !count || count!=packet_size(words[0]>>24)) return 0;
    try {
        Job job;
        job.kind=Kind::Packet;job.state=*state;
        std::memcpy(job.packet.data(),words,count*sizeof(uint32_t));
        r->push(std::move(job));
        return 1;
    } catch(...) { return 0; }
}
static void sync_job(NativeRaster *r, Job job) {
    std::promise<void> done;
    auto result = done.get_future();
    job.completion = &done;
    r->push(std::move(job));
    result.get();
}
extern "C" void native_raster_set_observer(NativeRaster *r,NativeRasterObserver observer,void *user) {
    Job job;job.kind=Kind::Observe;job.observer=observer;job.user=user;
    sync_job(r,std::move(job));
}
extern "C" void native_raster_visit(NativeRaster *r,NativeRasterVisit visit,void *user) {
    if(!visit) return;
    Job job;job.kind=Kind::Visit;job.visit=visit;job.user=user;
    sync_job(r,std::move(job));
}
extern "C" void native_raster_read(NativeRaster *r, int x,int y,int w,int h,uint16_t *out) {
    if(h==1 && w>0 && w<=32) {
        y&=511;
        if(r->read_row_y!=y) {
            Job row;
            row.kind=Kind::Read;
            row.command.args[1]=y;row.command.args[2]=1024;row.command.args[3]=1;
            row.destination=r->read_row.data();
            sync_job(r,std::move(row));
            r->read_row_y=y;
        }
        for(int i=0;i<w;++i) out[i]=r->read_row[(x+i)&1023];
        return;
    }
    Job job;
    job.kind = Kind::Read;
    job.command.args[0]=x; job.command.args[1]=y; job.command.args[2]=w; job.command.args[3]=h;
    job.destination=out;
    sync_job(r,std::move(job));
}
extern "C" void native_raster_snapshot(NativeRaster *r,uint16_t *out,uint64_t dirty[8]) {
    Job job;
    job.kind=Kind::Snapshot; job.destination=out; job.dirty=dirty;
    sync_job(r,std::move(job));
}
extern "C" void native_raster_restore(NativeRaster *r,const uint16_t *source) {
    Job job;
    job.kind=Kind::Restore; job.upload.assign(source,source+vram_words);
    sync_job(r,std::move(job));
}
extern "C" void native_raster_checkpoint(NativeRaster *r,uint16_t *vram,
    uint64_t dirty[8],SwNativeTextureState *texture) {
    Job job;
    job.kind=Kind::Snapshot;job.destination=vram;job.dirty=dirty;
    job.texture_destination=texture;
    sync_job(r,std::move(job));
}
extern "C" void native_raster_restore_checkpoint(NativeRaster *r,const uint16_t *vram,
    const SwNativeTextureState *texture) {
    Job job;
    job.kind=Kind::Restore;job.upload.assign(vram,vram+vram_words);
    job.texture_source=std::make_unique<SwNativeTextureState>(*texture);
    sync_job(r,std::move(job));
}
extern "C" uint64_t native_raster_completed(NativeRaster *r) {
    return r->completed.load(std::memory_order_acquire);
}
