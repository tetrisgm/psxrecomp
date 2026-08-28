#include "crc32.h"
#include "tex_pack.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

uint16_t s_vram[1024 * 512]{};
int s_failures = 0;
bool s_decode_enabled = false;

void check(bool ok, const char *what) {
    if (ok) return;
    std::fprintf(stderr, "tex_pack_copy_test: FAIL: %s\n", what);
    ++s_failures;
}

std::string debug_json(const char *subcmd) {
    char out[8192]{};
    const int n = tex_pack_debug_json(subcmd, out, (int)sizeof(out));
    return std::string(out, out + n);
}

std::string expected_key(const uint16_t *pixels, size_t count,
                         int clut_x, int clut_y) {
    const uint32_t tex = crc32_compute(reinterpret_cast<const uint8_t *>(pixels),
                                       count * sizeof(uint16_t));
    const uint32_t pal = crc32_compute(
        reinterpret_cast<const uint8_t *>(s_vram + clut_y * 1024 + clut_x),
        16 * sizeof(uint16_t));
    char key[64];
    std::snprintf(key, sizeof(key), "[\"%x-%x\"]", tex, pal);
    return key;
}

std::string raw_key(const uint16_t *pixels, size_t count,
                    int clut_x, int clut_y) {
    const std::string wrapped = expected_key(pixels, count, clut_x, clut_y);
    return wrapped.substr(2, wrapped.size() - 4);
}

void init_dump(const std::filesystem::path &root) {
    tex_pack_init((root / "copy-test.cue").string().c_str(), 0, 1,
                  root.string().c_str(), "");
}

void test_pack_key_parity(const std::filesystem::path &root) {
    constexpr int clut_x = 32;
    constexpr int clut_y = 400;
    const uint16_t pixels[8] = {
        0x3210, 0x7654, 0xBA98, 0xFEDC,
        0x1110, 0x3332, 0x5554, 0x7776,
    };
    const int lim[4] = {0, 0, 15, 1};
    const std::string expected = expected_key(pixels, 8, clut_x, clut_y);

    init_dump(root);
    tex_pack_on_upload(0, 0, 4, 2, pixels);
    tex_pack_on_textured_prim(lim, clut_x, clut_y, 0);
    const std::string upload_key = debug_json("dumped");
    check(upload_key == expected, "CPU upload emits expected texture/palette key");
    tex_pack_shutdown();

    init_dump(root);
    tex_pack_on_copy(100, 100, 64, 0, 4, 2, 1);
    int rect[4]{};
    check(tex_pack_pending_copy_for_prim(lim, 1, rect) == 1,
          "copied destination becomes a pending texture candidate");
    check(rect[0] == 64 && rect[1] == 0 && rect[2] == 4 && rect[3] == 2,
          "pending readback preserves the complete copy rectangle");
    tex_pack_resolve_copy(rect[0], rect[1], rect[2], rect[3], pixels);
    tex_pack_on_textured_prim(lim, clut_x, clut_y, 1);
    const std::string copy_key = debug_json("dumped");
    check(copy_key == expected, "VRAM copy emits expected texture/palette key");
    check(copy_key == upload_key, "copy and upload keys are identical");
    tex_pack_shutdown();
}

void test_copy_invalidation(const std::filesystem::path &root) {
    const uint16_t source[8] = {
        0x0123, 0x4567, 0x89AB, 0xCDEF,
        0x0246, 0x1357, 0x8ACE, 0x9BDF,
    };
    const uint16_t stale[8] = {
        0xFFFF, 0xEEEE, 0xDDDD, 0xCCCC,
        0xBBBB, 0xAAAA, 0x9999, 0x8888,
    };

    init_dump(root);
    tex_pack_on_upload(0, 0, 4, 2, source);
    tex_pack_on_upload(64, 0, 4, 2, stale);
    tex_pack_on_copy(0, 0, 64, 0, 4, 2, 1);
    const std::string after = debug_json("uploads");
    char stale_hash[32];
    std::snprintf(stale_hash, sizeof(stale_hash), "\"hash\":\"%x\"",
                  crc32_compute(reinterpret_cast<const uint8_t *>(stale),
                                sizeof(stale)));
    check(after.find(stale_hash) == std::string::npos,
          "copy invalidates the old destination first");
    check(after.find("\"x\":64") != std::string::npos,
          "tracked source propagates to destination immediately");
    check(after.find("\"x\":0") != std::string::npos,
          "disjoint source remains tracked");

    const int lim[4] = {0, 0, 15, 1};
    int rect[4]{};
    check(tex_pack_pending_copy_for_prim(lim, 1, rect) == 0,
          "tracked full-rect source avoids readback");
    tex_pack_invalidate(65, 0, 1, 1);
    check(debug_json("uploads").find("\"x\":64") == std::string::npos,
          "partial overwrite kills copied upload");
    tex_pack_shutdown();
}

void test_copy_is_replaceable(const std::filesystem::path &root) {
    constexpr int clut_x = 32;
    constexpr int clut_y = 400;
    const uint16_t pixels[8] = {
        0x3210, 0x7654, 0xBA98, 0xFEDC,
        0x1110, 0x3332, 0x5554, 0x7776,
    };
    const int lim[4] = {0, 0, 15, 1};
    const std::string key = raw_key(pixels, 8, clut_x, clut_y);
    const std::filesystem::path pack = root / "replacement-pack";
    std::filesystem::create_directories(pack);
    std::ofstream(pack / (key + ".png"), std::ios::binary).put('\0');

    tex_pack_init((root / "copy-test.cue").string().c_str(), 1, 0,
                  root.string().c_str(), pack.string().c_str());
    tex_pack_on_copy(100, 100, 64, 0, 4, 2, 1);
    int rect[4]{};
    check(tex_pack_pending_copy_for_prim(lim, 1, rect) == 1,
          "indexed replacement keeps copy pending");
    tex_pack_resolve_copy(64, 0, 4, 2, pixels);
    check(debug_json("uploads").find("\"x\":64") != std::string::npos,
          "resolved copy becomes replacement candidate");

    s_decode_enabled = true;
    TexPackRepl repl{};
    check(tex_pack_lookup_replacement(lim, clut_x, clut_y, 1, &repl) == 1,
          "copied destination resolves through replacement lookup");
    s_decode_enabled = false;
    tex_pack_shutdown();
}

} // namespace

extern "C" const uint16_t *gpu_get_vram(void) { return s_vram; }

extern "C" unsigned char *stbi_load_from_memory(const unsigned char *, int,
                                                 int *x, int *y, int *comp,
                                                 int) {
    if (!s_decode_enabled) return nullptr;
    *x = 16; *y = 2; *comp = 4;
    auto *pixels = static_cast<unsigned char *>(std::malloc(16 * 2 * 4));
    if (!pixels) return nullptr;
    for (int i = 0; i < 16 * 2; ++i) {
        pixels[i * 4 + 0] = (unsigned char)(32 + i);
        pixels[i * 4 + 1] = (unsigned char)(96 + i);
        pixels[i * 4 + 2] = (unsigned char)(160 + i);
        pixels[i * 4 + 3] = 255;
    }
    return pixels;
}

extern "C" void stbi_image_free(void *pixels) { std::free(pixels); }

int main() {
    for (int i = 0; i < 16; ++i)
        s_vram[400 * 1024 + 32 + i] = (uint16_t)(0x0421u * (uint16_t)i);

    const auto stamp = std::to_string(
        (unsigned long long)std::filesystem::file_time_type::clock::now()
            .time_since_epoch().count());
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("tex_pack_copy_test_" + stamp);
    std::filesystem::create_directories(root);
    test_pack_key_parity(root);
    test_copy_invalidation(root);
    test_copy_is_replaceable(root);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    if (s_failures) return 1;
    std::printf("tex_pack_copy_test: PASS\n");
    return 0;
}
