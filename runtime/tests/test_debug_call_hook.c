#undef NDEBUG
#include "debug_server.h"
#include <assert.h>
int g_debug_call_hooks, g_ls_replay_active;
volatile uint32_t g_psx_last_fn_entry;
static unsigned calls;
void (debug_server_log_call_entry)(uint32_t addr) {
    ++calls;
    if (!g_ls_replay_active) g_psx_last_fn_entry = addr;
}
int main(void) {
    for (unsigned i=0;i<10000;++i) debug_server_log_call_entry(0x80010000u+i*4);
    assert(calls==0 && g_psx_last_fn_entry==0x80010000u+9999u*4);
    g_debug_call_hooks=1; debug_server_log_call_entry(0x80020000u);
    assert(calls==1 && g_psx_last_fn_entry==0x80020000u);
    g_debug_call_hooks=0; g_ls_replay_active=1;
    debug_server_log_call_entry(0x80030000u);
    assert(calls==2 && g_psx_last_fn_entry==0x80020000u);
    g_ls_replay_active=0; debug_server_log_call_entry(0x80040000u);
    assert(calls==2 && g_psx_last_fn_entry==0x80040000u);
    return 0;
}
