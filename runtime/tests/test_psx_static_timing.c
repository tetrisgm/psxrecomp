#undef NDEBUG
#include "psx_cyc.h"

#include <assert.h>
#include <stdint.h>

uint64_t psx_cycle_count = 0;
uint64_t psx_next_service_cycle = 0;
uint32_t g_psx_cyc_block_raw;
int g_psx_cyc_block_active;
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
    g_psx_cyc_block_raw = 0; g_psx_cyc_block_active = 0;
    psx_cycle_count = 0;
    psx_next_service_cycle = deadline;
    g_psx_cyc_batch = 0;
    g_psx_cyc_batch_limit = 0;
    g_psx_cyc_bb_defer = 0;
    g_psx_cyc_local_acc = 0;
    service_count = 0;
}

int g_debug_cycle_hooks;
int g_psx_guest_time_frozen;
static uint32_t rng = 0x12345678u;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
int main(void) {
    /* Fast charge versus the unchanged general charge, including unusual
     * flags, fractional scaling, overflow and scheduled device publication. */
    for (unsigned trial = 0; trial < 300000; ++trial) {
        uint32_t scale=1+next()%64, frac=next()&65535u;
        uint32_t batch=trial%17 ? next()%128 : next();
        uint32_t limit=next()%128, cycles=trial%19 ? next()%128 : next();
        uint32_t local=next()%256, local_before=local;
        uint64_t deadline=next()%256;
        int mode=next()%32;
        reset_clock(deadline);
        g_psx_gcs_recip_q16=psx_gcs_recip_for(scale);g_psx_gcs_frac=frac;
        g_psx_gcs_batch=trial&1;g_psx_cyc_batch=batch;g_psx_cyc_batch_limit=limit;
        g_ls_replay_active=mode&1;g_event_step_conservative=(mode>>1)&1;
        psx_in_device_service=(mode>>2)&1;g_psx_cyc_bb_defer=(mode>>3)&1;
        g_psx_cyc_local_acc=(mode&16)?&local:0;
        psx_cyc_charge_slow(cycles);
        uint64_t expected_clock=psx_cycle_count, expected_deadline=psx_next_service_cycle;
        uint32_t expected_batch=g_psx_cyc_batch,expected_limit=g_psx_cyc_batch_limit;
        uint32_t expected_frac=g_psx_gcs_frac,expected_local=local;
        int expected_services=service_count;
        reset_clock(deadline);local=local_before;g_psx_gcs_frac=frac;
        g_psx_cyc_batch=batch;g_psx_cyc_batch_limit=limit;
        g_psx_cyc_bb_defer=(mode>>3)&1;g_psx_cyc_local_acc=(mode&16)?&local:0;
        psx_cyc_charge(cycles);
        assert(expected_clock==psx_cycle_count && expected_deadline==psx_next_service_cycle);
        assert(expected_batch==g_psx_cyc_batch && expected_limit==g_psx_cyc_batch_limit);
        assert(expected_frac==g_psx_gcs_frac && expected_local==local);
        assert(expected_services==service_count);
    }
    g_ls_replay_active=g_event_step_conservative=psx_in_device_service=0;

    for (unsigned trial = 0; trial < 200000; ++trial) {
        CPUState a = {0}, b;
        for (unsigned i = 0; i < 33; ++i) a.read_absorb[i].value = next() % 16;
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
    /* Whole blocks: combine raw charges only before the next event, then
     * compare every observation with the original per-instruction schedule.
     * Include fractional scaling, initial batches, near deadlines, explicit
     * device advances and latency calculations that consume the fraction. */
    for (unsigned trial=0;trial<50000;++trial) {
        uint32_t costs[32], kind[32], want_frac[32], want_result[32];
        uint64_t want_clock[32]; int want_services[32];
        uint32_t n=1+next()%32, bound=0, scale=1+next()%64;
        uint32_t frac=next()&65535u, prior=next()%64;
        uint64_t deadline=trial&1 ? 10000 : next()%256;
        for(unsigned i=0;i<n;++i) { costs[i]=next()%9; bound+=costs[i]; kind[i]=next()%16; }
        for(unsigned pass=0;pass<2;++pass) {
            reset_clock(deadline);g_psx_cyc_batch=prior;g_psx_cyc_batch_limit=64;
            g_psx_gcs_recip_q16=psx_gcs_recip_for(scale);g_psx_gcs_frac=frac;
            g_psx_gcs_batch=trial&1;g_psx_cyc_bb_defer=0;
            if(pass)psx_cyc_block_begin(bound);
            for(unsigned i=0;i<n;++i) {
                uint32_t result=0;
                if(pass && (i % 4u) == 0) psx_cyc_block_resume(bound);
                if(kind[i]==0) psx_cpu_charge(costs[i]);
                else if(kind[i]==1) psx_advance_cycles(costs[i]);
                else if(kind[i]==2) result=psx_cpu_cycles(costs[i]);
                else if(pass) psx_cyc_charge(costs[i]);
                else psx_cyc_charge_slow(costs[i]);
                if(kind[i]<3 || i+1==n) {
                    psx_cyc_batch_flush();
                    if(!pass) { want_clock[i]=psx_cycle_count;want_frac[i]=g_psx_gcs_frac;
                        want_result[i]=result;want_services[i]=service_count; }
                    else { assert(want_clock[i]==psx_cycle_count);
                        assert(want_frac[i]==g_psx_gcs_frac && want_result[i]==result);
                        assert(want_services[i]==service_count); }
                }
            }
        }
    }
    reset_clock(100);
    g_psx_gcs_recip_q16=65536u; g_psx_gcs_frac=0;
    psx_cyc_block_begin(80); psx_cyc_charge(10);
    psx_cyc_block_resume(80);
    assert(g_psx_cyc_block_active && g_psx_cyc_block_raw==10 && !g_psx_cyc_batch);
    psx_cpu_charge(20); /* cache/device observation publishes, then charges */
    assert(!g_psx_cyc_block_active && psx_cycle_count==30);
    psx_cyc_block_resume(70); assert(!g_psx_cyc_block_active); /* exact deadline */
    psx_cyc_block_resume(69); assert(g_psx_cyc_block_active);
    reset_clock(100);
    psx_cyc_block_begin(99);assert(g_psx_cyc_block_active);
    psx_cyc_charge(50);assert(g_psx_cyc_block_raw==50);
    psx_cyc_block_publish();assert(!g_psx_cyc_block_active && !g_psx_cyc_block_raw);
    reset_clock(100);psx_cyc_block_begin(100);assert(!g_psx_cyc_block_active);
    g_psx_guest_time_frozen=1;psx_cyc_block_begin(1);assert(!g_psx_cyc_block_active);
    g_psx_guest_time_frozen=0;
    reset_clock(4);
    assert(!psx_cyc_segment_ready(4));
    psx_next_service_cycle = 5;
    assert(psx_cyc_segment_ready(4));
    g_debug_cycle_hooks = 1;
    assert(!psx_cyc_segment_ready(4));
    return 0;
}
