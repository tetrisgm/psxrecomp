#ifndef PSXRECOMP_TIMING_SNAPSHOT_H
#define PSXRECOMP_TIMING_SNAPSHOT_H
#include <stdint.h>
#include "cpu_state.h"
#include "psx_icache_state.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Immutable, process-local diagnostic input for a future timed context.
 * Capture on the main guest thread between instructions. Capture does not
 * flush pending charges, initialize the cache, read MMIO or service devices.
 * This is not a savestate or a complete device/IRQ execution snapshot; no
 * restore/publication API is provided. No pointer into live state escapes. */
typedef struct PsxTimingSnapshot PsxTimingSnapshot;
typedef struct PsxTimingView {
    uint64_t cycle_count, next_service_cycle, fast_limit;
    uint64_t muldiv_done, gte_done;
    uint32_t pc, cop0_status, biu;
    uint32_t batch, batch_limit, local_acc_value;
    uint32_t scale_recip_q16, scale_state[3]; /* fraction, mod gate, RAM gate */
    uint32_t scale_config, scale_live;
    uint32_t ld_absorb;
    uint8_t read_absorb[33], read_absorb_which, read_fudge, ld_which_t;
    int local_acc_active, bb_defer, in_device_service, conservative;
    int scaled_batch, replay_active, guest_time_frozen;
    int icache_active; /* -1 remains unresolved; capture never initializes it */
    uint32_t icache_tags[PSX_ICACHE_TAG_WORDS];
} PsxTimingView;

PsxTimingSnapshot *psx_timing_snapshot_capture(const CPUState *cpu);
const PsxTimingView *psx_timing_snapshot_view(const PsxTimingSnapshot *snapshot);
void psx_timing_snapshot_release(PsxTimingSnapshot *snapshot);

#ifdef __cplusplus
}
#endif
#endif
