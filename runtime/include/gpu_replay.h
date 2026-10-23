#ifndef PSX_GPU_REPLAY_H
#define PSX_GPU_REPLAY_H
#include <stddef.h>
#include <stdint.h>
#include "pgxp.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Immutable answers to all GPU precision queries at original consumption.
 * Keep raw shadows separate from positions accepted by PGXP seam policy. */
typedef struct GpuReplayVertex {
    int32_t x16, y16, raw_x16, raw_y16;
    uint16_t sz, source, raw_sz;
    uint32_t raw_valid, projection_valid;
    float unclamped_depth;
    PGXPProjection projection;
} GpuReplayVertex;
typedef struct GpuReplayWord {
    uint32_t word, source; /* UINT32_MAX GP0 port; UINT32_MAX-1 GP1 port */
    GpuReplayVertex vertex;
} GpuReplayWord;
typedef struct GpuReplayState GpuReplayState;
typedef struct GpuReplayProvider {
    void *user;
    size_t count;
    int (*word)(void *user, size_t index, GpuReplayWord *out);
    /* RAM/scratchpad and captured enhancement memory only; never live MMIO. */
    int (*read)(void *user, uint32_t address, unsigned width, uint32_t *out);
    /* Required: rebuild packet metadata from captured draw-hook events.
     * Called inside the provider scope; GPU tag helpers read private memory.
     * Return 0 if any active mod metadata is unavailable. A caller with no
     * relevant hooks must explicitly attest that with a successful callback. */
    int (*prepare_metadata)(void *user);
    /* Flush events at count (trailing empty nodes/lists), still in scope. */
    int (*finish)(void *user);
} GpuReplayProvider;

/* Call from provider.word while its read callback selects the corresponding
 * immutable DMA-list memory. Preserve begin/node/end order at word offsets. */
int gpu_replay_list_begin(uint32_t root);
int gpu_replay_list_node(uint32_t address,uint32_t header);
int gpu_replay_list_end(void);

/* Capture on main at snapshot creation; no machine mutation. Captures GPU
 * registers, scene predicates and packet tag tables, not textures/VRAM.
 * VRAM must still belong to this generation when the caller admits replay. */
GpuReplayState *gpu_replay_capture(void);
void gpu_replay_release(GpuReplayState *state);
/* Main GPU parser only, inside an existing render-pass checkpoint. Parser
 * output goes through the ordinary renderer queue. Return 0 => discard the
 * enclosing transaction. Never reruns guest code or reads live RAM/PGXP.
 * Configuration/session generation validation belongs to caller admission. */
int gpu_replay_execute(const GpuReplayState *state, const GpuReplayProvider *provider);
#ifdef __cplusplus
}
#endif
#endif
