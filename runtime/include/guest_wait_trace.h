#ifndef PSX_GUEST_WAIT_TRACE_H
#define PSX_GUEST_WAIT_TRACE_H
#include <array>
#include <chrono>
#include <cstdint>
/* Opt-in, guest-thread-local attribution. No sampling thread, allocation or
 * cross-thread synchronization. Durations include descheduling inside a scope;
 * they are wall time, not CPU time. Disabled release builds compile scopes out. */
namespace guest_wait_trace {
enum Kind { Publish, Reserve, NativeLock, NativeSpace, History, NativeCompletion, Count };
#ifndef PSX_NO_DEBUG_TOOLS
inline thread_local bool enabled=false;
inline thread_local std::array<uint64_t,Count> ns{};
inline uint64_t now() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct Scope {
    Kind kind; uint64_t start;
    explicit Scope(Kind k):kind(k),start(enabled?now():0) {}
    ~Scope() { if(start) ns[kind]+=now()-start; }
};
#else
struct Scope { explicit Scope(Kind) {} };
#endif
}
#endif
