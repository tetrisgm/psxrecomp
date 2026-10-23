#ifndef PSXRECOMP_PSX_ICACHE_STATE_H
#define PSXRECOMP_PSX_ICACHE_STATE_H

/* State-only form of the existing I-cache model. These helpers touch only
 * the caller's tags and return unscaled CPU work; they never charge the
 * machine, inspect globals, clear load give-back, or service devices.
 * The owner must apply its enabled/replay gates before calling them. */
#include <stdint.h>
#include "psx_instr_cost.h"

#define PSX_ICACHE_TAG_WORDS 1024u

static inline uint32_t psx_icache_state_fetch(uint32_t tags[PSX_ICACHE_TAG_WORDS],
                                             uint32_t address) {
    uint32_t index = (address & 0xFFCu) >> 2;
    if (tags[index] == address) return 0;
    if (psx_fetch_uncached(address)) return 4;
    uint32_t line = address & 0xFFFFFFF0u;
    uint32_t first = (address & 0xCu) >> 2;
    uint32_t base = (address & 0xFF0u) >> 2;
    for (uint32_t i = 0; i < 4; ++i)
        tags[base + i] = line | (i << 2) | (i < first ? 2u : 0u);
    return 7u - first;
}

static inline void psx_icache_state_isc_store(uint32_t tags[PSX_ICACHE_TAG_WORDS],
    uint32_t biu, uint32_t address, uint32_t value) {
    /* BCC IS1=bit11; TAG/INV/LOCK=bits2/1/0. This preserves the current
     * tag model, including its documented provenance/limitations. */
    if (!(biu & 0x800u) || !(biu & 7u)) return;
    uint32_t valid = (biu & 4u) ?
        ((value << ((address & 3u) * 8u)) & 15u) : 0;
    uint32_t line = address & 0xFFFFFFF0u;
    uint32_t base = (address & 0xFF0u) >> 2;
    for (uint32_t i = 0; i < 4; ++i)
        tags[base + i] = line | (i << 2) | ((valid & (1u << i)) ? 0u : 2u);
}
#endif
