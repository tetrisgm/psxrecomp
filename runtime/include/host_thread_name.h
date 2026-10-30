#ifndef PSX_HOST_THREAD_NAME_H
#define PSX_HOST_THREAD_NAME_H
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
static inline void psx_host_thread_name(const wchar_t *label) {
    typedef HRESULT (WINAPI *SetName)(HANDLE,PCWSTR);
    SetName set_name=(SetName)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"SetThreadDescription");
    if(set_name) set_name(GetCurrentThread(),label);
}
#else
#include <stddef.h>
static inline void psx_host_thread_name(const wchar_t *label) { (void)label; }
#endif
#endif
