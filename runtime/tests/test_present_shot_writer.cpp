#include "present_shot_writer.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static std::atomic<int> completion_count{0};
static std::atomic<int> completion_ok{0};

extern "C" void present_shot_done(int ok)
{
    completion_ok.store(ok);
    completion_count.fetch_add(1);
}

int main(void)
{
    const char *path = "present_shot_writer_test.png";
    unsigned char *rgb = (unsigned char *)std::malloc(2 * 2 * 3);
    if (!rgb) return 1;
    const unsigned char pixels[12] = {
        255, 0, 0,  0, 255, 0,
        0, 0, 255,  255, 255, 255
    };
    std::memcpy(rgb, pixels, sizeof(pixels));
    std::remove(path);
    present_shot_write_async(path, rgb, 2, 2);
    present_shot_writer_shutdown();
    if (completion_count.load() != 1 || completion_ok.load() != 1) return 2;
    FILE *file = std::fopen(path, "rb");
    if (!file) return 3;
    unsigned char sig[8] = {0};
    const size_t got = std::fread(sig, 1, sizeof(sig), file);
    std::fclose(file);
    std::remove(path);
    const unsigned char expected[8] = {137,80,78,71,13,10,26,10};
    if (got != sizeof(sig) || std::memcmp(sig, expected, sizeof(sig)) != 0)
        return 4;
    std::puts("present_shot_writer_test: PASS");
    return 0;
}
