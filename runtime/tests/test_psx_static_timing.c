#undef NDEBUG
#include "psx_cyc.h"

#include <assert.h>
#include <stdint.h>

uint64_t psx_cycle_count = 0;
uint64_t psx_next_service_cycle = 0;
uint32_t g_psx_cyc_batch = 0;
uint32_t g_psx_cyc_batch_limit = 0;
int g_psx_cyc_bb_defer = 0;
uint32_t *g_psx_cyc_local_acc = 0;
int psx_in_device_service = 0;
int g_event_step_conservative = 0;
int g_ls_replay_active = 0;
int g_ls_mode = 0;
volatile int g_ds_recording = 0;
uint8_t *g_psx_ram = 0;
int g_psx_load_delay = 1;
uint32_t g_psx_gcs_recip_q16 = 65536u;
uint32_t g_psx_gcs_frac = 0u;
int g_psx_gcs_batch = 0;

static int service_count;

void psx_devices_service_to_now(void) {
    service_count++;
    psx_next_service_cycle = psx_cycle_count + 1000u;
}

void psx_advance_cycles_slow(uint32_t cycles) {
    psx_cycle_count += cycles;
}

static void reset_clock(uint64_t deadline) {
    psx_cycle_count = 0;
    psx_next_service_cycle = deadline;
    g_psx_cyc_batch = 0;
    g_psx_cyc_batch_limit = 0;
    g_psx_cyc_bb_defer = 0;
    g_psx_cyc_local_acc = 0;
    service_count = 0;
}

int g_debug_cycle_hooks;
static uint32_t rng = 0x12345678u;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
int main(void) {
    for (unsigned trial = 0; trial < 200000; ++trial) {
        CPUState a = {0}, b;
        for (unsigned i = 0; i < 33; ++i) a.read_absorb[i] = next() % 16;
        a.read_absorb_which = next() % 32;
        a.ld_which_t = next() % 33;
        a.ld_absorb = next() % 16;
        a.read_fudge = next() % 33;
        b = a;
        uint32_t m[4] = {next(), next(), next(), next()};
        unsigned n = 2 + next() % 3;
        unsigned scale = 1 + next() % 64;
        uint32_t frac = next() & 65535;
        uint32_t batch = next() % 64;
        reset_clock(10000);
        g_psx_cyc_batch = batch; g_psx_cyc_batch_limit = 64;
        g_psx_gcs_recip_q16 = psx_gcs_recip_for(scale);
        g_psx_gcs_batch = trial & 1;
        g_psx_gcs_frac = frac;
        for (unsigned i = 0; i < n; ++i) psx_cyc_step(&a, m[i]);
        uint64_t clock = psx_cycle_count + g_psx_cyc_batch;
        uint32_t final_frac = g_psx_gcs_frac;
        reset_clock(10000);
        g_psx_cyc_batch = batch; g_psx_cyc_batch_limit = 64;
        g_psx_gcs_frac = frac;
        assert(psx_cyc_segment_ready(n));
        psx_cyc_segment(&b, n, m[0], m[1], m[2], m[3]);
        assert(memcmp(&a, &b, sizeof a) == 0);
        assert(clock == psx_cycle_count + g_psx_cyc_batch);
        assert(final_frac == g_psx_gcs_frac);
        assert(service_count == 0);
    }
    reset_clock(4);
    assert(!psx_cyc_segment_ready(4));
    psx_next_service_cycle = 5;
    assert(psx_cyc_segment_ready(4));
    g_debug_cycle_hooks = 1;
    assert(!psx_cyc_segment_ready(4));
    return 0;
}
