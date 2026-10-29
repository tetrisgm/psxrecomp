#undef NDEBUG
/* Windows sampler regression: PE lookup agrees with the OS without taking
 * its function-table lock while the guest is suspended. */
#include "../src/host_sampler.c"
#include <assert.h>
int g_psx_render_pass_active, g_exec_phase;
uint64_t s_frame_count;
int main(void) {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    const char *names[]={"NtProtectVirtualMemory","NtWaitForSingleObject","RtlVirtualUnwind"};
    HMODULE ntdll=GetModuleHandleW(L"ntdll.dll");
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i) {
        uintptr_t pc=(uintptr_t)GetProcAddress(ntdll,names[i]);
        assert(pc);cache_unwind_image(pc);
        DWORD64 expected_base=0,actual_base=0;int known=0;
        PRUNTIME_FUNCTION expected=RtlLookupFunctionEntry(pc,&expected_base,NULL);
        PRUNTIME_FUNCTION actual=cached_unwind_entry(pc,&actual_base,&known);
        assert(known && actual==expected);
        if(expected)assert(actual_base==expected_base);
    }
    {DWORD64 base=0;int known=1;
     assert(!cached_unwind_entry(1,&base,&known) && !known);s_unwind_missing=0;}
    _putenv("PSX_HOST_SAMPLER_CALLERS=1");host_sampler_start();
    for(unsigned i=0;i<100;++i) {
        HMODULE module=LoadLibraryW(L"version.dll");assert(module);
        Sleep(1);FreeLibrary(module);
    }
    assert(host_sampler_supported() && host_sampler_seq()>10);
    HostSample sample;assert(host_sampler_get(host_sampler_seq()-1,&sample));
#endif
    return 0;
}
