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
enum class Kind { Draw, Read, Snapshot, Restore };
struct Job {
    Kind kind = Kind::Draw;
    NativeRasterCommand command{};
    std::vector<uint16_t> upload;
    uint16_t *destination = nullptr;
    uint64_t *dirty = nullptr;
    std::promise<void> *completion = nullptr;
    size_t bytes() const { return sizeof(Job) + upload.size() * sizeof(uint16_t); }
};
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
    case NR_COPY: sw_copy_rect(a[0],a[1],a[2],a[3],a[4],a[5]); break;
    case NR_FLAT_TRIANGLE: sw_draw_flat_triangle(a[0],a[1],a[2],a[3],a[4],a[5],a[6]); break;
    case NR_GOURAUD_TRIANGLE: sw_draw_gouraud_triangle(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8]); break;
    case NR_TEXTURED_TRIANGLE: sw_draw_textured_triangle(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11],a[12],a[13],a[14]); break;
    case NR_SHADED_TEXTURED_TRIANGLE: sw_draw_shaded_textured_triangle(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11],a[12],a[13],a[14],a[15],a[16],a[17],a[18]); break;
    case NR_FLAT_RECT: sw_draw_flat_rect(a[0],a[1],a[2],a[3],a[4]); break;
    case NR_TEXTURED_RECT: sw_draw_textured_rect(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8]); break;
    case NR_TEXTURED_RECT_SCALED: sw_draw_textured_rect_scaled(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10]); break;
    case NR_LINE: sw_draw_line(a[0],a[1],a[2],a[3],a[4]); break;
    case NR_SHADED_LINE: sw_draw_shaded_line(a[0],a[1],a[2],a[3],a[4],a[5]); break;
    case NR_WRITE: sw_vram_write(a[0],a[1],a[2]); break;
    case NR_UPLOAD: sw_vram_transfer_in(a[0],a[1],a[2],a[3],j.upload.data()); break;
    case NR_CPU_UPLOAD:
        for (size_t i = 0; i < j.upload.size(); ++i) {
            const size_t offset = size_t(a[4]) + i;
            const int x = (a[0] + int(offset % size_t(a[2]))) & 1023;
            const int y = (a[1] + int(offset / size_t(a[2]))) & 511;
            if (a[6] && (sw_vram_read(x,y) & 0x8000)) continue;
            sw_vram_write(x,y,j.upload[i] | (a[5] ? 0x8000 : 0));
        }
        break;
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

    void push(Job job) {
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
            switch (job.kind) {
            case Kind::Draw: draw(job); break;
            case Kind::Read: sw_vram_transfer_out(a[0],a[1],a[2],a[3],job.destination); break;
            case Kind::Snapshot:
                std::memcpy(job.destination,vram.data(),sizeof(vram));
                sw_context_take_dirty(job.dirty);
                break;
            case Kind::Restore:
                sw_vram_transfer_in(0,0,1024,512,job.upload.data());
                break;
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
    if (!r || !c || c->op < NR_AREA || c->op > NR_CPU_UPLOAD) return 0;
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
static void sync_job(NativeRaster *r, Job job) {
    std::promise<void> done;
    auto result = done.get_future();
    job.completion = &done;
    r->push(std::move(job));
    result.get();
}
extern "C" void native_raster_read(NativeRaster *r, int x,int y,int w,int h,uint16_t *out) {
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
extern "C" uint64_t native_raster_completed(NativeRaster *r) {
    return r->completed.load(std::memory_order_acquire);
}
