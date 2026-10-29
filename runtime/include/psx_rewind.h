#ifndef PSX_REWIND_H
#define PSX_REWIND_H

/* Local native per-frame history. Hold F8 / configured controller shortcut
 * to play backwards; release to resume. Disabled during netplay. */

#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PsxRewindStats {
    uint64_t captures, restores, failures, bytes;
    double capture_ms, max_ms, serialize_ms, delta_ms;
    uint64_t verified, verify_failures;
    uint32_t first_difference;
    uint32_t frame;
} PsxRewindStats;
void psx_rewind_get_stats(PsxRewindStats *out);
void psx_rewind_set_held(int down);
void psx_rewind_debug_hold(int down);
void psx_rewind_debug_verify(int on);

/* Legacy sparse counts map to 15 times as many per-frame history edges. */
void psx_rewind_set_depth(uint32_t depth);
void psx_rewind_set_interval(uint32_t interval);
/* Settings/launcher preference. Call before psx_rewind_configure(); to change
 * it mid-session, call this then configure() (on) or shutdown() (off). */
void psx_rewind_set_enabled(int enabled);

void psx_rewind_configure(uint32_t bios_checksum, uint32_t entry_pc);
void psx_rewind_shutdown(void);
/* A different mounted disc invalidates every earlier rewind snapshot. */
uint32_t psx_rewind_discard_history(void);

int  psx_rewind_enabled(void);
/* Title block (mod API psx_mod_set_rewind_blocked): while set, Rewind refuses
 * to open and captures no history (the ring keeps what it has). Cleared at
 * every mod session start. */
void psx_rewind_set_title_blocked(int blocked);
int  psx_rewind_title_blocked(void);
/* Snapshots in the ring (debug/status). */
uint32_t psx_rewind_snap_count(void);
int  psx_rewind_is_open(void);
/* True during reverse playback. */
int  psx_rewind_needs_present(void);

/* Guest vblank: bump frame counter / schedule capture when not paused. */
void psx_rewind_note_frame(void);

/* Safe BB-edge (beside savestate_poll): capture due snap / apply load. */
void psx_rewind_poll(CPUState *cpu, uint32_t resume_pc);

int  psx_rewind_toggle(void);          /* legacy overlay compatibility */
int  psx_rewind_cancel(void);          /* release hold */
int  psx_rewind_accept(void);          /* release hold */
void psx_rewind_move(int delta);       /* legacy no-op */

/* Edge-triggered nav with repeat. *_down is 1 while held. */
void psx_rewind_nav_held(int left_down, int right_down, int accept_down,
                         int cancel_down, uint32_t now_ms);

void psx_rewind_present_tick(uint32_t now_ms);

/* Legacy overlay ABI: no panel is emitted. */
int  psx_rewind_overlay_image(const uint32_t **pixels, int *w, int *h);
/* Legacy overlay ABI: always zero. */
float psx_rewind_slide(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_REWIND_H */
