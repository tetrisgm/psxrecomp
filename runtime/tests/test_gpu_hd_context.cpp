#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"
#include "gpu_hd_textures.h"
#include "hd_texture_pack.h"
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
    /* Only this freshly created test directory is removed. */
    fs::remove_all(root);
    if (failures) { std::fprintf(stderr,"HD context failures: %d; GL calls: %d\n",int(failures),int(gl_calls)); return 1; }
    std::puts("PASS: independent native HD contexts, no GL calls");
    return 0;
}
