#include "psx_timing_snapshot.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

uint64_t psx_cycle_count = 100, psx_next_service_cycle = 120, g_psx_cycle_fast_limit = 119;
uint32_t g_psx_cyc_batch = 3, g_psx_cyc_batch_limit = 20;
static uint32_t local = 7;
uint32_t *g_psx_cyc_local_acc = &local;
uint32_t g_psx_gcs_recip_q16 = 32768;
int g_psx_cyc_bb_defer = 2, psx_in_device_service = 0, g_event_step_conservative = 1;
int g_psx_gcs_batch = 1, g_ls_replay_active = 0, g_psx_guest_time_frozen = 0;
int g_psx_icache_active = -1;
uint32_t g_psx_icache_tv[1024];
static uint32_t scale_state[3] = {123, 1, 0}, biu = 0x804;
void psx_guest_cycle_scale_snapshot(uint32_t out[3]) { memcpy(out, scale_state, sizeof scale_state); }
uint32_t psx_guest_cycle_scale(void) { return 1; }
uint32_t psx_guest_cycle_scale_config(void) { return 2; }
uint32_t psx_memory_cache_control(void) { return biu; }

int main(void) {
    CPUState cpu = {0};
    cpu.pc = 0x80012340; cpu.cop0[12] = 0x10900000;
    cpu.muldiv_ts_done = 105; cpu.gte_ts_done = 108;
    cpu.read_absorb[32] = 41; cpu.read_absorb_which = 32;
    cpu.read_fudge = 0x20; cpu.ld_which_t = 5; cpu.ld_absorb = 3;
    for (unsigned i = 0; i < 1024; ++i) g_psx_icache_tv[i] = i * 4 + 0x80000000;
    CPUState saved = cpu;
    PsxTimingSnapshot *snapshot = psx_timing_snapshot_capture(&cpu);
    const PsxTimingView *v = psx_timing_snapshot_view(snapshot);
    assert(v && memcmp(&cpu, &saved, sizeof cpu) == 0);
    assert(psx_cycle_count == 100 && g_psx_cyc_batch == 3 && local == 7);
    assert(g_psx_cyc_local_acc == &local && g_psx_icache_active == -1);
    assert(v->cycle_count == 100 && v->next_service_cycle == 120 && v->fast_limit == 119);
    assert(v->batch == 3 && v->batch_limit == 20 && v->local_acc_active && v->local_acc_value == 7);
    assert(v->bb_defer == 2 && v->conservative && v->scaled_batch);
    assert(!v->in_device_service && !v->replay_active && !v->guest_time_frozen);
    assert(v->pc == cpu.pc && v->cop0_status == cpu.cop0[12] && v->biu == 0x804);
    assert(v->scale_recip_q16 == 32768 && v->scale_config == 2 && v->scale_live == 1);
    assert(memcmp(v->scale_state, scale_state, sizeof scale_state) == 0);
    assert(v->muldiv_done == 105 && v->gte_done == 108 && v->ld_absorb == 3);
    assert(v->read_absorb[32] == 41 && v->read_absorb_which == 32 && v->read_fudge == 0x20 && v->ld_which_t == 5);
    assert(v->icache_active == -1 && memcmp(v->icache_tags, g_psx_icache_tv, sizeof g_psx_icache_tv) == 0);
    /* The capture owns values, including a former stack-backed accumulator. */
    local = 999; g_psx_cyc_local_acc = NULL; psx_cycle_count = 999;
    memset(g_psx_icache_tv, 0, sizeof g_psx_icache_tv); memset(&cpu, 0, sizeof cpu);
    scale_state[0] = 999; biu = 0;
    assert(v->local_acc_value == 7 && v->cycle_count == 100 && v->scale_state[0] == 123);
    assert(v->icache_tags[1] == 0x80000004 && v->read_absorb[32] == 41 && v->biu == 0x804);
    psx_timing_snapshot_release(snapshot);
    assert(!psx_timing_snapshot_capture(NULL) && !psx_timing_snapshot_view(NULL));
    psx_timing_snapshot_release(NULL);
    puts("PASS: immutable timing capture leaves live state untouched");
    return 0;
}
