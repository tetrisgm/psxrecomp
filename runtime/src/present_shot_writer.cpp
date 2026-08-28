#include "present_shot_writer.h"
#include "png_write.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

extern "C" void present_shot_done(int ok);

static std::mutex s_writer_mutex;
static std::thread s_writer_thread;

extern "C" void present_shot_write_async(const char *path, uint8_t *rgb,
                                           uint32_t width, uint32_t height)
{
    if (!path || !path[0] || !rgb || width == 0 || height == 0) {
        std::free(rgb);
        present_shot_done(0);
        return;
    }

    std::lock_guard<std::mutex> lock(s_writer_mutex);
    /* The protocol admits only one capture until its completion sequence
     * advances, so a previous join never waits in normal operation. */
    if (s_writer_thread.joinable()) s_writer_thread.join();
    try {
        const std::string owned_path(path);
        s_writer_thread = std::thread([owned_path, rgb, width, height]() {
            int wrote = 0;
            FILE *file = std::fopen(owned_path.c_str(), "wb");
            if (file) {
                wrote = png_write_rgb(file, rgb, width, height);
                if (std::fclose(file) != 0) wrote = 0;
            }
            std::free(rgb);
            present_shot_done(wrote);
        });
    } catch (...) {
        std::free(rgb);
        present_shot_done(0);
    }
}

extern "C" void present_shot_writer_shutdown(void)
{
    std::lock_guard<std::mutex> lock(s_writer_mutex);
    if (s_writer_thread.joinable()) s_writer_thread.join();
}
