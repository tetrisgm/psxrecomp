#ifndef PSX_HOST_SCHEDULING_H
#define PSX_HOST_SCHEDULING_H
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>

/* Registration belongs to the calling thread and is reverted at thread
 * exit. Keep avrt loaded while TLS destructors may still use its functions. */
class PsxHostScheduling {
    using Enter=decltype(&AvSetMmThreadCharacteristicsW);
    using Leave=decltype(&AvRevertMmThreadCharacteristics);
    using Priority=decltype(&AvSetMmThreadPriority);
    HANDLE handle=nullptr;
    Leave leave=nullptr;
    int old_priority=THREAD_PRIORITY_NORMAL;
public:
    explicit PsxHostScheduling(bool audio) {
        static HMODULE module=LoadLibraryW(L"avrt.dll");
        if(module) {
            auto enter=reinterpret_cast<Enter>(GetProcAddress(module,"AvSetMmThreadCharacteristicsW"));
            leave=reinterpret_cast<Leave>(GetProcAddress(module,"AvRevertMmThreadCharacteristics"));
            auto priority=reinterpret_cast<Priority>(GetProcAddress(module,"AvSetMmThreadPriority"));
            DWORD index=0;
            if(enter && leave) handle=enter(audio?L"Pro Audio":L"Games",&index);
            if(handle && priority) priority(handle,AVRT_PRIORITY_HIGH);
        }
        if(!handle) {
            old_priority=GetThreadPriority(GetCurrentThread());
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
        }
        using Name=HRESULT(WINAPI *)(HANDLE,PCWSTR);
        auto name=reinterpret_cast<Name>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"SetThreadDescription"));
        if(name) name(GetCurrentThread(),audio?L"PSX audio":L"PSX emulation");
    }
    ~PsxHostScheduling() {
        if(handle) leave(handle);
        else if(old_priority!=THREAD_PRIORITY_ERROR_RETURN)
            SetThreadPriority(GetCurrentThread(),old_priority);
    }
};
static inline void psx_host_schedule_thread(bool audio) {
    static thread_local PsxHostScheduling scheduling(audio);
}
#else
static inline void psx_host_schedule_thread(bool) {}
#endif
#endif
