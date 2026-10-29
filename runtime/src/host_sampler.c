/* host_sampler.c - always-on host CPU sampler for the emulation thread
 * (debug-tools builds; TCP host_profile, tools/host_profile.py).
 *
 * A background thread samples the emulation thread's instruction pointer
 * every millisecond into a ring, tagged with the guest frame and whether a
 * render pass was running. The ring covers the last ~65 seconds, so a probe
 * asks for a window that already happened instead of arming a profiler
 * before a workload. Addresses are stored relative to the executable's load
 * base; tools/host_profile.py maps them to functions with the image's own
 * symbol table (nm), so no debug info is needed.
 *
 * Windows: SuspendThread / GetThreadContext / ResumeThread from a sampler
 * thread. POSIX: the process CPU-time timer (SIGPROF) delivered to the
 * emulation thread, which reads its own interrupted PC from the ucontext.
 * Other hosts, and builds without debug tools, report the sampler as
 * unsupported. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1   /* REG_RIP, dladdr */
#endif
#include "host_sampler.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern int g_psx_render_pass_active;
extern int g_exec_phase;
extern uint64_t s_frame_count;

static HostSample s_ring[HOST_SAMPLER_CAP];
static volatile uint64_t s_seq;
static uintptr_t s_image_base;
static int s_supported;

#ifndef PSX_NO_DEBUG_TOOLS
static void record(uintptr_t pc, uintptr_t caller) {
    const uint64_t n = s_seq;
    HostSample *e = &s_ring[n % HOST_SAMPLER_CAP];
    e->rva = (pc >= s_image_base) ? (uint64_t)(pc - s_image_base) : 0;
    e->caller = (caller >= s_image_base) ? (uint64_t)(caller - s_image_base) : 0;
    e->frame = (uint32_t)s_frame_count;
    e->in_pass = g_psx_render_pass_active ? 1u : 0u;
    e->phase = (uint16_t)g_exec_phase;
    s_seq = n + 1;
}
#endif

#if defined(PSX_NO_DEBUG_TOOLS)
/* Production builds carry no sampler: it reports unsupported. */
void host_sampler_start(void) {}

#elif defined(_WIN32)
#include <windows.h>

static HANDLE s_target;
static uintptr_t s_image_end;               /* base + SizeOfImage */
static ULONG_PTR s_stack_lo, s_stack_hi;    /* the target thread's stack */
static int s_callers = -1;                  /* PSX_HOST_SAMPLER_CALLERS: 0, 1, 2 */

#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0602
WINBASEAPI VOID WINAPI GetCurrentThreadStackLimits(PULONG_PTR low, PULONG_PTR high);  /* Windows 8+ */
#endif
static void note_current_stack(void) {
    GetCurrentThreadStackLimits(&s_stack_lo, &s_stack_hi);
}

/* PSX_HOST_SAMPLER_CALLERS=1: a sample outside the executable (driver, OS
 * wait, C library) is unwound to its innermost caller inside it, so time in
 * nvoglv64.dll or a kernel wait is charged to the exe function that made the
 * call. PSX_HOST_SAMPLER_CALLERS=2 records every sample's caller: the first
 * exe frame above the sampled function (who calls a hot helper). Module
 * discovery runs only after ResumeThread. While suspended, unwind lookup uses
 * retained PE exception directories, never the process function-table lock. */
#if defined(_M_X64) || defined(__x86_64__)
/* RtlLookupFunctionEntry can acquire a lock owned by the suspended target.
 * Cache PE unwind directories with a module reference while the target runs.
 * Unknown modules lose only caller attribution for their first sample. Dynamic
 * function tables are deliberately not queried while the target is suspended. */
struct UnwindImage {
    HMODULE handle;
    uintptr_t base, end;
    const RUNTIME_FUNCTION *functions;
    size_t count;
};
static struct UnwindImage s_unwind_images[256];
static size_t s_unwind_count;
static uintptr_t s_unwind_missing;
static void cache_unwind_image(uintptr_t pc) {
    HMODULE module = NULL;
    if (!pc || s_unwind_count == 256 ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCWSTR)pc, &module)) return;
    for (size_t i=0; i<s_unwind_count; ++i) if(s_unwind_images[i].handle==module) {
        FreeLibrary(module); return;
    }
    uintptr_t base=(uintptr_t)module;
    const IMAGE_DOS_HEADER *dos=(const IMAGE_DOS_HEADER*)base;
    const IMAGE_NT_HEADERS *nt=(const IMAGE_NT_HEADERS*)(base+dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    struct UnwindImage *image=&s_unwind_images[s_unwind_count++];
    image->handle=module; image->base=base; image->end=base+nt->OptionalHeader.SizeOfImage;
    image->functions=(const RUNTIME_FUNCTION*)(base+dir.VirtualAddress);
    image->count=dir.VirtualAddress?dir.Size/sizeof(RUNTIME_FUNCTION):0;
}
static PRUNTIME_FUNCTION cached_unwind_entry(uintptr_t pc,DWORD64 *base,int *known) {
    *known=0;
    for(size_t i=0;i<s_unwind_count;++i) {
        const struct UnwindImage *image=&s_unwind_images[i];
        if(pc<image->base || pc>=image->end)continue;
        *known=1; *base=image->base;
        DWORD rva=(DWORD)(pc-image->base);
        size_t lo=0,hi=image->count;
        while(lo<hi) {
            size_t mid=lo+(hi-lo)/2;
            const RUNTIME_FUNCTION *entry=&image->functions[mid];
            if(rva<entry->BeginAddress)hi=mid;
            else if(rva>=entry->EndAddress)lo=mid+1;
            else return (PRUNTIME_FUNCTION)entry;
        }
        return NULL; /* A known image's leaf function has no unwind entry. */
    }
    s_unwind_missing=pc;
    return NULL;
}
static uintptr_t exe_caller(const CONTEXT *start) {
    CONTEXT c = *start;
    for (int k = 0; k < 64; k++) {
        DWORD64 ib = 0;
        int known=0;
        PRUNTIME_FUNCTION fe = cached_unwind_entry(c.Rip, &ib, &known);
        if(!known)return 0;
        if (fe) {
            PVOID hd = NULL; DWORD64 est = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c.Rip, fe, &c, &hd, &est, NULL);
        } else {
            if (c.Rsp < s_stack_lo || c.Rsp + 8 > s_stack_hi) return 0;
            c.Rip = *(const DWORD64 *)(uintptr_t)c.Rsp;
            c.Rsp += 8;
        }
        if (!c.Rip || c.Rsp < s_stack_lo || c.Rsp > s_stack_hi) return 0;
        if (c.Rip >= s_image_base && c.Rip < s_image_end) return (uintptr_t)c.Rip;
    }
    return 0;
}
#endif

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static DWORD WINAPI sampler_main(LPVOID arg) {
    (void)arg;
    /* A 1 ms period needs a high-resolution timer; Sleep(1) would follow
     * whatever timer resolution the process last asked for. */
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -10000;   /* 1 ms, relative */
        if (!SetWaitableTimer(timer, &due, 1, NULL, NULL, FALSE)) {
            CloseHandle(timer);
            timer = NULL;
        }
    }
    for (;;) {
        if (timer) WaitForSingleObject(timer, INFINITE);
        else Sleep(1);
        HANDLE target = s_target;
        if (SuspendThread(target) == (DWORD)-1) continue;
        CONTEXT ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = s_callers ? CONTEXT_CONTROL | CONTEXT_INTEGER : CONTEXT_CONTROL;
        if (GetThreadContext(target, &ctx)) {
#if defined(_M_X64) || defined(__x86_64__)
            uintptr_t caller = 0;
            if (s_callers == 2 ||
                (s_callers && (ctx.Rip < s_image_base || ctx.Rip >= s_image_end)))
                caller = exe_caller(&ctx);
            record((uintptr_t)ctx.Rip, caller);
#elif defined(_M_ARM64) || defined(__aarch64__)
            record((uintptr_t)ctx.Pc, 0);
#endif
        }
        ResumeThread(target);
#if defined(_M_X64) || defined(__x86_64__)
        if(s_unwind_missing) {
            uintptr_t missing=s_unwind_missing;s_unwind_missing=0;
            cache_unwind_image(missing);
        }
#endif
    }
    return 0;
}

void host_sampler_start(void) {
    if (s_supported) return;
    s_image_base = (uintptr_t)GetModuleHandleW(NULL);
    {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)s_image_base;
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(s_image_base + (uintptr_t)dos->e_lfanew);
        s_image_end = s_image_base + nt->OptionalHeader.SizeOfImage;
    }
    {
        const char *e = getenv("PSX_HOST_SAMPLER_CALLERS");
        s_callers = (e && *e) ? atoi(e) : 0;
    }
    note_current_stack();
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &s_target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                             THREAD_QUERY_INFORMATION,
                         FALSE, 0))
        return;
    HANDLE t = CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
    if (!t) return;
    SetThreadPriority(t, THREAD_PRIORITY_TIME_CRITICAL);
    CloseHandle(t);
    s_supported = 1;
}

/* PSX_HOST_SAMPLER_THREAD=render: the render thread calls this when it
 * starts and the sampler follows it instead of the emulation thread. */
void host_sampler_retarget_current(const char *name) {
    const char *e = getenv("PSX_HOST_SAMPLER_THREAD");
    if (!s_supported || !e || !name || strcmp(e, name) != 0) return;
    HANDLE h;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &h,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                             THREAD_QUERY_INFORMATION,
                         FALSE, 0))
        return;
    note_current_stack();
    s_target = h;   /* the sampler reads it once per sample; the old handle leaks, once */
}

#elif defined(__linux__) || defined(__APPLE__)
#include <signal.h>
#include <sys/time.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>   /* <ucontext.h> errors without _XOPEN_SOURCE */
#else
#include <ucontext.h>
#endif
#include <pthread.h>
#include <dlfcn.h>

static pthread_t s_thread;

#if defined(__APPLE__)
/* The emulation thread's stack, [lo, hi). SIGPROF can land on a thread that
 * is still being created; pthread_self() there traps (arm64e pointer
 * authentication), so the handler tells threads apart by the interrupted
 * stack pointer instead, which needs no thread state at all. */
static uintptr_t s_stack_lo, s_stack_hi;
#endif

static void on_prof(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si;
    ucontext_t *uc = (ucontext_t *)uc_;
#if defined(__APPLE__)
    if (!uc || !uc->uc_mcontext) return;
#if defined(__aarch64__)
    const uintptr_t sp = (uintptr_t)uc->uc_mcontext->__ss.__sp;
#else
    const uintptr_t sp = (uintptr_t)uc->uc_mcontext->__ss.__rsp;
#endif
    if (sp < s_stack_lo || sp >= s_stack_hi) return;
#else
    if (!pthread_equal(pthread_self(), s_thread)) return;
#endif
#if defined(__linux__) && defined(__x86_64__)
    record((uintptr_t)uc->uc_mcontext.gregs[REG_RIP], 0);
#elif defined(__linux__) && defined(__aarch64__)
    record((uintptr_t)uc->uc_mcontext.pc, 0);
#elif defined(__APPLE__) && defined(__x86_64__)
    record((uintptr_t)uc->uc_mcontext->__ss.__rip, 0);
#elif defined(__APPLE__) && defined(__aarch64__)
    record((uintptr_t)uc->uc_mcontext->__ss.__pc, 0);
#else
    (void)uc;
#endif
}

void host_sampler_start(void) {
    if (s_supported) return;
    Dl_info info;
    if (dladdr((void *)&host_sampler_start, &info) && info.dli_fbase)
        s_image_base = (uintptr_t)info.dli_fbase;
    s_thread = pthread_self();
#if defined(__APPLE__)
    s_stack_hi = (uintptr_t)pthread_get_stackaddr_np(s_thread);
    s_stack_lo = s_stack_hi - (uintptr_t)pthread_get_stacksize_np(s_thread);
#endif
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, NULL) != 0) return;
    struct itimerval it;
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 1000;
    it.it_value = it.it_interval;
    if (setitimer(ITIMER_PROF, &it, NULL) != 0) return;
    s_supported = 1;
}

#else
void host_sampler_start(void) {}
#endif

int host_sampler_supported(void) { return s_supported; }

uint64_t host_sampler_seq(void) { return s_seq; }

uint64_t host_sampler_image_base(void) { return (uint64_t)s_image_base; }

int host_sampler_get(uint64_t seq, HostSample *out) {
    const uint64_t now = s_seq;
    if (seq >= now || now - seq > HOST_SAMPLER_CAP) return 0;
    *out = s_ring[seq % HOST_SAMPLER_CAP];
    return 1;
}

#if defined(PSX_NO_DEBUG_TOOLS) || !defined(_WIN32)
void host_sampler_retarget_current(const char *name) { (void)name; }
#endif
