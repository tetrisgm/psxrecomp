#include "draw_service.h"
#include <cassert>
#include <thread>

static void count(void *user) {
    ++*static_cast<int *>(user);
    draw_service_poll(); /* nested admission must not recurse */
}
int main() {
    int calls = 0;
    draw_service_poll();
    draw_service_register(count, &calls);
    assert(g_draw_service_enabled);
    draw_service_poll();
    assert(calls == 1);
    std::thread other([] { for (int i = 0; i < 10000; ++i) draw_service_poll(); });
    other.join();
    assert(calls == 1);
    draw_service_poll();
    assert(calls == 2);
    draw_service_register(nullptr, nullptr);
    draw_service_poll();
    assert(!g_draw_service_enabled && calls == 2);
}
