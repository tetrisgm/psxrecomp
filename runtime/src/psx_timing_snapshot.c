#include "psx_timing_snapshot.h"
#include "psx_cycles.h"
#include "psx_cycle_freeze.h"
#include "psx_memory.h"
#include <stdlib.h>
#include <string.h>

extern uint64_t g_psx_cycle_fast_limit;

struct PsxTimingSnapshot { PsxTimingView view; };

PsxTimingSnapshot *psx_timing_snapshot_capture(const CPUState *cpu) {
    if (!cpu) return NULL;
    PsxTimingSnapshot *snapshot = calloc(1, sizeof *snapshot);
    if (!snapshot) return NULL;
    PsxTimingView *v = &snapshot->view;
    v->cycle_count = psx_cycle_count;
    v->next_service_cycle = psx_next_service_cycle;
    v->fast_limit = g_psx_cycle_fast_limit;
    v->batch = g_psx_cyc_batch;
    v->batch_limit = g_psx_cyc_batch_limit;
    v->local_acc_active = g_psx_cyc_local_acc != NULL;
    v->local_acc_value = g_psx_cyc_local_acc ? *g_psx_cyc_local_acc : 0;
    v->bb_defer = g_psx_cyc_bb_defer;
    v->in_device_service = psx_in_device_service;
    v->conservative = g_event_step_conservative;
    v->scaled_batch = g_psx_gcs_batch;
    v->replay_active = g_ls_replay_active;
    v->guest_time_frozen = g_psx_guest_time_frozen;
    v->scale_recip_q16 = g_psx_gcs_recip_q16;
    psx_guest_cycle_scale_snapshot(v->scale_state);
    v->scale_config = psx_guest_cycle_scale_config();
    v->scale_live = psx_guest_cycle_scale();
    v->biu = psx_memory_cache_control();
    v->pc = cpu->pc;
    v->cop0_status = cpu->cop0[12];
    v->muldiv_done = cpu->muldiv_ts_done;
    v->gte_done = cpu->gte_ts_done;
    v->ld_absorb = cpu->ld_absorb;
    v->read_absorb_which = cpu->read_absorb_which;
    v->read_fudge = cpu->read_fudge;
    v->ld_which_t = cpu->ld_which_t;
    memcpy(v->read_absorb, cpu->read_absorb, sizeof v->read_absorb);
    v->icache_active = g_psx_icache_active;
    memcpy(v->icache_tags, g_psx_icache_tv, sizeof v->icache_tags);
    return snapshot;
}

const PsxTimingView *psx_timing_snapshot_view(const PsxTimingSnapshot *snapshot) {
    return snapshot ? &snapshot->view : NULL;
}
void psx_timing_snapshot_release(PsxTimingSnapshot *snapshot) { free(snapshot); }
