#include "draw_call_gate.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <thread>

struct DrawCallGate {
    enum State { IDLE, PENDING, SERVING, DONE } state = IDLE;
    const std::thread::id owner = std::this_thread::get_id();
    std::mutex mutex;
    std::condition_variable changed;
    void *payload = nullptr;
    int status = DRAW_CALL_CLOSED, reply = 0;
    bool closed = false;
};

extern "C" DrawCallGate *draw_call_gate_create(void) {
    try { return new DrawCallGate; } catch (...) { return nullptr; }
}
extern "C" int draw_call_gate_request(DrawCallGate *g, void *payload,
                                        uint32_t timeout_ms, int *reply) {
    if (!g || !payload || !reply) return DRAW_CALL_BAD_ARGUMENT;
    if (std::this_thread::get_id() == g->owner) return DRAW_CALL_WRONG_THREAD;
    std::unique_lock<std::mutex> lock(g->mutex);
    if (g->closed) return DRAW_CALL_CLOSED;
    if (g->state != DrawCallGate::IDLE) return DRAW_CALL_BUSY;
    g->payload = payload;
    g->state = DrawCallGate::PENDING;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    if (!g->changed.wait_until(lock, deadline, [&] { return g->state == DrawCallGate::DONE; })) {
        if (g->state == DrawCallGate::PENDING) {
            g->payload = nullptr;
            g->state = DrawCallGate::IDLE;
            g->changed.notify_all();
            return DRAW_CALL_TIMEOUT;
        }
        /* A claimed callback owns a live reference to payload. */
        g->changed.wait(lock, [&] { return g->state == DrawCallGate::DONE; });
    }
    const int status = g->status;
    if (status == DRAW_CALL_REPLIED) *reply = g->reply;
    g->payload = nullptr;
    g->state = DrawCallGate::IDLE;
    g->changed.notify_all();
    return status;
}
extern "C" int draw_call_gate_service(DrawCallGate *g, DrawCallServe serve, void *user) {
    if (!g || !serve) return DRAW_CALL_BAD_ARGUMENT;
    if (std::this_thread::get_id() != g->owner) return DRAW_CALL_WRONG_THREAD;
    std::unique_lock<std::mutex> lock(g->mutex);
    if (g->state == DrawCallGate::SERVING) return DRAW_CALL_BUSY;
    if (g->state != DrawCallGate::PENDING) return 0;
    g->state = DrawCallGate::SERVING;
    void *payload = g->payload;
    lock.unlock();
    int result = 0, status = DRAW_CALL_REPLIED;
    try { result = serve(payload, user); }
    catch (...) { status = DRAW_CALL_CALLBACK_FAILED; }
    lock.lock();
    g->reply = result;
    g->status = status;
    g->state = DrawCallGate::DONE;
    g->changed.notify_all();
    return 1;
}
static int close_locked(DrawCallGate *g) {
    if (g->state == DrawCallGate::SERVING) return DRAW_CALL_BUSY;
    g->closed = true;
    if (g->state == DrawCallGate::PENDING) {
        g->status = DRAW_CALL_CLOSED;
        g->state = DrawCallGate::DONE;
        g->changed.notify_all();
    }
    return 1;
}
extern "C" int draw_call_gate_close(DrawCallGate *g) {
    if (!g) return DRAW_CALL_BAD_ARGUMENT;
    if (std::this_thread::get_id() != g->owner) return DRAW_CALL_WRONG_THREAD;
    std::lock_guard<std::mutex> lock(g->mutex);
    return close_locked(g);
}
extern "C" int draw_call_gate_destroy(DrawCallGate *g) {
    if (!g) return 1;
    if (std::this_thread::get_id() != g->owner) return DRAW_CALL_WRONG_THREAD;
    std::unique_lock<std::mutex> lock(g->mutex);
    int result = close_locked(g);
    if (result != 1) return result;
    g->changed.wait(lock, [&] { return g->state == DrawCallGate::IDLE; });
    lock.unlock();
    delete g;
    return 1;
}
