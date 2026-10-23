/* Compile with PSX_ICACHE_STATE_HELPERS=0 as well: this compares the pure
 * helper against the retained pre-refactor runtime, not just itself. */
#include "psx_icache.h"
#include "psx_icache_state.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int g_ls_replay_active;
static uint64_t cycles;
void psx_cpu_charge(uint32_t n) { cycles += n; }
void psx_advance_cycles(uint32_t n) { cycles += n; }
static uint32_t random_state = 0x483119u;
static uint32_t next(void) {
    random_state = random_state * 1664525u + 1013904223u;
    return random_state;
}
int main(void) {
    CPUState cpu = {0};
    uint32_t tags[1024];
    psx_icache_reset();
    g_psx_icache_active = 1;
    memcpy(tags, g_psx_icache_tv, sizeof tags);
    /* Every refill offset: later words valid, earlier words still invalid. */
    for (uint32_t first = 0; first < 4; ++first) {
        for (unsigned i = 0; i < 1024; ++i) tags[i] = 1;
        assert(psx_icache_state_fetch(tags, 0x80010000u + first * 4) == 7 - first);
        for (uint32_t i = 0; i < 4; ++i)
            assert(tags[i] == (0x80010000u | (i * 4) | (i < first ? 2u : 0u)));
        assert(psx_icache_state_fetch(tags, 0x80010000u + first * 4) == 0);
    }
    psx_icache_reset();
    g_psx_icache_active = 1;
    memcpy(tags, g_psx_icache_tv, sizeof tags);
    uint64_t expected = 0;
    for (unsigned iteration = 0; iteration < 30000; ++iteration) {
        uint32_t r = next();
        uint32_t address = (r & 0x3FFCu) | ((r >> 28) % 3 == 0 ? 0xA0010000u :
                                          (r >> 28) % 3 == 1 ? 0x80010000u : 0x10000u);
        if (iteration % 3 == 0) {
            uint32_t biu = (r & 8u ? 0x800u : 0) | ((r >> 4) & 7u);
            uint32_t value = next();
            address |= r & 3u; /* IsC lane shifts */
            psx_icache_state_isc_store(tags, biu, address, value);
            psx_icache_isc_store(biu, address, value);
        } else {
            cpu.read_absorb_which = 1 + (r % 32);
            cpu.read_absorb[cpu.read_absorb_which] = 99;
            uint32_t cost = psx_icache_state_fetch(tags, address);
            uint8_t selected = cpu.read_absorb_which;
            expected += cost;
            psx_icache_fetch(&cpu, address);
            assert(cpu.read_absorb_which == (cost ? 0 : selected));
            assert(cpu.read_absorb[selected] == (cost ? 0 : 99));
            assert(cycles == expected);
        }
        assert(memcmp(tags, g_psx_icache_tv, sizeof tags) == 0);
    }
    g_psx_icache_active = 0;
    psx_icache_fetch(&cpu, 0x80001000u);
    psx_icache_isc_store(0x807u, 0x80001000u, 0);
    assert(cycles == expected && memcmp(tags, g_psx_icache_tv, sizeof tags) == 0);
    printf("PASS: 30000 cache transitions, %llu CPU cycles\n", (unsigned long long)cycles);
    return 0;
}
