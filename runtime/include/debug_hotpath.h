#ifndef PSX_DEBUG_HOTPATH_H
#define PSX_DEBUG_HOTPATH_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
extern int g_debug_cycle_hooks;
void debug_server_cyc_observe_enabled(uint32_t block_leader_phys);
#ifdef PSX_DEBUG_HOTPATH_IMPLEMENTATION
void debug_server_cyc_observe(uint32_t block_leader_phys);
#else
static inline void debug_server_cyc_observe(uint32_t block_leader_phys) {
    if (g_debug_cycle_hooks) debug_server_cyc_observe_enabled(block_leader_phys);
}
#endif
#ifdef __cplusplus
}
#endif
#endif
