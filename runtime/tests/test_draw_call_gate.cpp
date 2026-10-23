#include "draw_call_gate.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace std::chrono_literals;
static int add(void *payload, void *) { return ++*static_cast<int *>(payload); }
static void serve_once(DrawCallGate *g, DrawCallServe fn, void *user = nullptr) {
    const auto limit = std::chrono::steady_clock::now() + 2s;
    while (!draw_call_gate_service(g, fn, user)) {
        assert(std::chrono::steady_clock::now() < limit);
        std::this_thread::yield();
    }
}
struct Slow {
    DrawCallGate *gate;
    std::atomic<bool> returned{false};
};
static int slow(void *payload, void *user) {
    auto *s = static_cast<Slow *>(user);
    assert(draw_call_gate_service(s->gate, add, nullptr) == DRAW_CALL_BUSY);
    assert(draw_call_gate_close(s->gate) == DRAW_CALL_BUSY);
    assert(draw_call_gate_destroy(s->gate) == DRAW_CALL_BUSY);
    std::thread competitor([&] {
        int p = 0, reply = 0;
        assert(draw_call_gate_request(s->gate, &p, 10, &reply) == DRAW_CALL_BUSY);
        assert(draw_call_gate_service(s->gate, add, nullptr) == DRAW_CALL_WRONG_THREAD);
        assert(draw_call_gate_close(s->gate) == DRAW_CALL_WRONG_THREAD);
    });
    competitor.join();
    std::this_thread::sleep_for(150ms); /* beyond the request's claim deadline */
    assert(!s->returned);
    return ++*static_cast<int *>(payload);
}
static int throws(void *, void *) { throw 1; }
int main() {
    DrawCallGate *g = draw_call_gate_create(); assert(g);
    int p = 0, reply = 0;
    assert(draw_call_gate_request(g, &p, 1, &reply) == DRAW_CALL_WRONG_THREAD);
    assert(draw_call_gate_service(g, add, nullptr) == 0);
    std::atomic<bool> finished{false};
    std::thread ordered([&] {
        for (int n = 0; n < 50; ++n) {
            int stack_payload = n, result = -1;
            assert(draw_call_gate_request(g, &stack_payload, 1000, &result) == DRAW_CALL_REPLIED);
            assert(stack_payload == n + 1 && result == n + 1);
        }
        finished = true;
    });
    auto limit = std::chrono::steady_clock::now() + 2s;
    while (!finished) {
        assert(draw_call_gate_service(g, add, nullptr) >= 0);
        assert(std::chrono::steady_clock::now() < limit);
        std::this_thread::yield();
    }
    ordered.join();
    Slow state{g};
    std::thread held_stack([&] {
        int stack_payload = 7, result = -1;
        assert(draw_call_gate_request(g, &stack_payload, 100, &result) == DRAW_CALL_REPLIED);
        assert(stack_payload == 8 && result == 8);
        state.returned = true;
    });
    serve_once(g, slow, &state); held_stack.join();
    std::thread timeout([&] {
        int payload = 11, result = -1;
        assert(draw_call_gate_request(g, &payload, 10, &result) == DRAW_CALL_TIMEOUT);
        assert(payload == 11 && result == -1);
    });
    timeout.join();
    assert(draw_call_gate_service(g, add, nullptr) == 0);
    std::thread exceptional([&] {
        int payload = 0, result = -1;
        assert(draw_call_gate_request(g, &payload, 1000, &result) == DRAW_CALL_CALLBACK_FAILED);
        assert(result == -1);
    });
    serve_once(g, throws); exceptional.join();
    std::atomic<bool> started{false};
    std::thread cancelled([&] {
        int payload = 0, result = -1;
        started = true;
        assert(draw_call_gate_request(g, &payload, 1000, &result) == DRAW_CALL_CLOSED);
        assert(result == -1);
    });
    while (!started) std::this_thread::yield();
    std::this_thread::sleep_for(10ms);
    assert(draw_call_gate_close(g) == 1);
    cancelled.join();
    assert(draw_call_gate_destroy(g) == 1);
    std::puts("PASS: owner call gate ordering, live worker stack, refusal, timeout and close");
}
