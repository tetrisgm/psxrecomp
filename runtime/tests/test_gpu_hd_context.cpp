#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"
#include "gpu_hd_textures.h"
#include "hd_texture_pack.h"
#include "native_raster.h"
#include "../src/png_write.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>

static std::atomic<int> gl_calls{0};
extern "C" void gl_renderer_set_hd_texture_mode(int) { ++gl_calls; }
extern "C" void gl_renderer_clear_hd_texture_cache(void) { ++gl_calls; }
extern "C" void gl_renderer_render_thread_sync(const char*) { ++gl_calls; }
extern "C" int gl_renderer_hd_texture_cached(uint64_t,uint64_t,uint32_t*,uint32_t*) { ++gl_calls; return 0; }
extern "C" {
int g_ws_bd_stretch_on=0,g_ws_bd_stretch_pct=0;
int psx_ws_prim_in_backdrop(void) { return 0; }
}

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path()/(
        "psx-hd-context-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto beetle = root/"1-texture-replacements";
    fs::create_directories(beetle);
    const uint16_t native_words[8]={2,2,2,2,2,2,2,2};
    char filename[32];
    std::snprintf(filename,sizeof(filename),"%x-0.png",hd_texture_crc32_words_le(native_words,8));
    const uint8_t rgba[4*2*4]={255,0,0,255};
    FILE* png=std::fopen((beetle/filename).string().c_str(),"wb");
    if (!png) return 1;
    const int png_ok=png_write_rgba(png,rgba,4,2); std::fclose(png);
    if (!png_ok) return 1;
    std::atomic<int> failures{0};
    const auto run = [&](int index) {
        auto* native = gpu_hd_textures_create_native_context();
        if (!native) { ++failures; return; }
        auto* previous = gpu_hd_textures_select_context(native);
        std::vector<uint16_t> vram(1024*512,uint16_t(index+1));
        gpu_hd_textures_set_vram(vram.data());
        char error[512]{};
        const auto path = (index ? beetle : root/"0").u8string();
        if (!gpu_hd_textures_configure(path.c_str(),1,0,error,sizeof(error))) ++failures;
        if (!gpu_hd_textures_active() || !gpu_hd_textures_replacements_enabled()) ++failures;
        for (int i=0; i<100; ++i) {
            gpu_hd_textures_begin_upload(64,0,4,2);
            gpu_hd_textures_track_upload(64,0,4,2,nullptr);
            gpu_hd_textures_begin_copy(64,0,80,0,4,2);
            gpu_hd_textures_end_copy();
            GpuHdTextureDiag diag{};
            gpu_hd_textures_get_diag(&diag);
            if (!diag.active || path != diag.root || diag.format != (index ? 1 : 2)) ++failures;
            const auto generation = gpu_hd_textures_tracking_generation();
            if (generation != gpu_hd_textures_tracking_generation()) ++failures;
        }
        /* Drive the tracker from the real native worker, including a partial
         * masked DMA upload. No renderer mirror participates in the key. */
        gpu_hd_textures_reset_tracking();
        vram[64]=0x8002;
        auto* raster=native_raster_create(vram.data());
        struct Probe { GpuHdTextureContext* context; int tracked=0; uint16_t a=0,b=0; } probe{native};
        const auto inspect=[](void* data,const uint16_t* words) {
            auto& probe=*static_cast<Probe*>(data);
            auto* previous=gpu_hd_textures_select_context(probe.context);
            probe.tracked=gpu_hd_textures_invalidation_changes(64,0,4,2);
            probe.a=words[64];probe.b=words[65];
            gpu_hd_textures_select_context(previous);
        };
        if(!raster) ++failures;
        else {
            native_raster_set_observer(raster,gpu_hd_textures_native_event,native);
            NativeRasterCommand upload{};upload.op=NR_CPU_UPLOAD;
            upload.args[0]=64;upload.args[2]=4;upload.args[3]=2;
            upload.args[5]=upload.args[6]=1;
            const uint16_t words[8]={3,3,3,3,3,3,3,3};
            if(!native_raster_submit(raster,&upload,words,2)) ++failures;
            native_raster_visit(raster,inspect,&probe);
            if(probe.tracked || probe.a!=0x8002 || probe.b!=0x8003) ++failures;
            upload.args[4]=2;
            if(!native_raster_submit(raster,&upload,words+2,6)) ++failures;
            native_raster_visit(raster,inspect,&probe);
            if(!probe.tracked || probe.a!=0x8002 || probe.b!=0x8003) ++failures;
            NativeRasterState state{};state.right=1023;state.bottom=511;
            const uint32_t copy[4]={0x80000000,64,80,0x00020004};
            if(!native_raster_gp0(raster,&state,copy,4)) ++failures;
            native_raster_visit(raster,inspect,&probe);
            if(!probe.tracked) ++failures;
            native_raster_snapshot(raster,vram.data(),nullptr);
            native_raster_set_observer(raster,nullptr,nullptr);
            // The bound pointer belongs to the worker; rebind before teardown.
            gpu_hd_textures_bind_vram(vram.data());
            native_raster_destroy(raster);
        }
        auto* tracking = gpu_hd_textures_capture_tracking();
        auto* destination = gpu_hd_textures_create_native_context();
        if (!tracking || !destination) ++failures;
        if (destination) {
            gpu_hd_textures_select_context(destination);
            gpu_hd_textures_set_vram(vram.data());
            if (gpu_hd_textures_apply_tracking(tracking)) ++failures; // no matching configuration yet
            if (!gpu_hd_textures_configure(path.c_str(),1,0,error,sizeof(error)) ||
                !gpu_hd_textures_apply_tracking(tracking) ||
                !gpu_hd_textures_invalidation_changes(64,0,1,1)) ++failures;
            const auto applied_generation=gpu_hd_textures_tracking_generation();
            if(!gpu_hd_textures_apply_tracking(tracking) ||
               gpu_hd_textures_tracking_generation()!=applied_generation) ++failures;
            gpu_hd_textures_invalidate(64,0,4,2);
            if(!gpu_hd_textures_apply_tracking(tracking) ||
               !gpu_hd_textures_invalidation_changes(64,0,1,1)) ++failures;
            gpu_hd_textures_free_tracking(tracking); tracking = nullptr;
            if (!gpu_hd_textures_invalidation_changes(64,0,1,1)) ++failures;
            gpu_hd_textures_shutdown();
            gpu_hd_textures_select_context(native);
            gpu_hd_textures_destroy_context(destination);
        }
        gpu_hd_textures_free_tracking(tracking);
        gpu_hd_textures_set_dump_enabled(1);
        gpu_hd_textures_set_dump_enabled(0);
        if (!gpu_hd_textures_reload(error,sizeof(error))) ++failures;
        gpu_hd_textures_shutdown();
        gpu_hd_textures_select_context(previous);
        gpu_hd_textures_destroy_context(native);
    };
    std::thread a(run,0), b(run,1);
    a.join(); b.join();
    if (gpu_hd_textures_active() || gl_calls != 0) ++failures;
    {
        auto* raster=native_raster_create(nullptr);
        char error[512]{};
        if(!raster || !gpu_hd_textures_attach_native(raster,error,sizeof error) ||
           !gpu_hd_textures_configure(beetle.u8string().c_str(),1,0,error,sizeof error)) ++failures;
        else {
            const auto gl_before=gl_calls.load();
            NativeRasterCommand upload{};upload.op=NR_CPU_UPLOAD;
            upload.args[0]=64;upload.args[2]=4;upload.args[3]=2;
            if(!native_raster_submit(raster,&upload,native_words,8)) ++failures;
            std::vector<uint16_t> pixels(1024*512);
            SwNativeTextureState cache{};
            auto* tracking=gpu_hd_textures_capture_native(pixels.data(),&cache);
            if(!tracking || pixels[64]!=2 || pixels[1024+67]!=2 || gl_calls!=gl_before ||
               gpu_hd_textures_tracking_bytes(tracking)==0) ++failures;
            gpu_hd_textures_bind_vram(pixels.data());
            if(!gpu_hd_textures_apply_tracking(tracking) || !gpu_hd_textures_invalidation_changes(64,0,4,2)) ++failures;
            gpu_hd_textures_free_tracking(tracking);
            const auto missing=(root/"missing-texture-replacements").u8string();
            if(gpu_hd_textures_configure(missing.c_str(),1,0,error,sizeof error)) ++failures;
            tracking=gpu_hd_textures_capture_native(nullptr,nullptr);
            if(!gpu_hd_textures_apply_tracking(tracking) || !gpu_hd_textures_invalidation_changes(64,0,4,2)) ++failures;
            gpu_hd_textures_free_tracking(tracking);
            NativeRasterState state{};state.right=1023;state.bottom=511;
            state.page=1|256;
            native_raster_inputs_enable(raster,1);
            const uint32_t sample[4]={0x65000080,0x006402bc,0,0x00010001};
            const uint64_t before_fill=native_raster_gp0_identified(raster,&state,sample,4);
            const uint64_t unchanged=native_raster_gp0_identified(raster,&state,sample,4);
            const uint32_t fill[3]={0x020000ff,64,0x00020010};
            if(!native_raster_gp0(raster,&state,fill,3)) ++failures;
            const uint64_t after_fill=native_raster_gp0_identified(raster,&state,sample,4);
            auto* inputs=native_raster_inputs_seal(raster);
            const auto* before_metadata=static_cast<const GpuHdTextureTracking*>(native_raster_inputs_metadata(inputs,before_fill));
            const auto* after_metadata=static_cast<const GpuHdTextureTracking*>(native_raster_inputs_metadata(inputs,after_fill));
            if(!native_raster_inputs_valid(inputs) || !before_metadata || !after_metadata ||
               before_metadata==after_metadata ||
               before_metadata!=native_raster_inputs_metadata(inputs,unchanged)) ++failures;
            if(!gpu_hd_textures_apply_tracking(before_metadata) || !gpu_hd_textures_invalidation_changes(64,0,4,2) ||
               !gpu_hd_textures_apply_tracking(after_metadata) || gpu_hd_textures_invalidation_changes(64,0,4,2)) ++failures;
            native_raster_inputs_destroy(inputs);
            tracking=gpu_hd_textures_capture_native(pixels.data(),nullptr);
            if(!gpu_hd_textures_apply_tracking(tracking) || gpu_hd_textures_invalidation_changes(64,0,4,2) ||
               pixels[64]!=31 || gl_calls!=gl_before) ++failures;
            gpu_hd_textures_free_tracking(tracking);
            gpu_hd_textures_set_dump_enabled(1);
            gpu_hd_textures_set_dump_enabled(0);
            gpu_hd_textures_shutdown();
            tracking=gpu_hd_textures_capture_native(nullptr,nullptr);
            if(!gpu_hd_textures_apply_tracking(tracking)) ++failures;
            gpu_hd_textures_free_tracking(tracking);
        }
        gpu_hd_textures_attach_native(nullptr,error,sizeof error);
        native_raster_destroy(raster);
    }
    /* Only this freshly created test directory is removed. */
    fs::remove_all(root);
    if (failures) { std::fprintf(stderr,"HD context failures: %d; GL calls: %d\n",int(failures),int(gl_calls)); return 1; }
    std::puts("PASS: independent native HD contexts, no GL calls");
    return 0;
}
