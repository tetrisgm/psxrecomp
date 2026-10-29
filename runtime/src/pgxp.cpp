/* pgxp.cpp — PGXP value-propagation engine (docs/ENHANCEMENTS.md G1.2/G1.3).
 *
 * CLEAN-ROOM implementation of the publicly documented PGXP technique
 * (psx-spx GTE docs + public design write-ups + our own G1 measurements).
 * The vendored duckstation/ (CC BY-NC-ND) and beetle-psx/ (GPL) trees are
 * black-box behavioral oracles only — no code from them appears here.
 *
 * Model
 * -----
 * Every 32-bit word of guest RAM/scratchpad, every GPR (plus HI/LO), and
 * every GTE data register owns a shadow slot recording the sub-pixel screen
 * position that word carries (16.16 X/Y + projected SZ depth), the exact
 * guest word it describes (`value`), and per-half validity flags. RTPS/RTPT
 * fill the SXY shadow FIFO with the pre-truncation projection; the
 * psx_pgxp_* hooks copy shadows along with the data (loads, stores, COP2
 * transfers, and — in cpu-mode — the arithmetic games use to repack vertex
 * halves); the GPU asks for the precise position of a GP0 vertex word by the
 * packet's RAM address.
 *
 * The safety invariant: a shadow is only ever BELIEVED after validation
 * against the actual guest word it claims to describe, and only while no
 * writer it did not see has replaced that word. A matching word alone does
 * not prove provenance: two projections that round to the same integer X/Y
 * carry different fractions and depths. So every CPU write of a GPR or
 * memory word runs a hook that carries or resets its shadow, and DMA / host
 * stores drop the shadow of each word they touch (pgxp_invalidate_word, from
 * memory.c). Validation still catches any other writer that changes the
 * word. We never model side effects — overwrite, invalidate, validate.
 *
 * Everything here is host-only and visual-only: guest-visible state is never
 * read back from shadows, shadows are dropped on savestate/rewind, and the
 * speculative native-validation bracket suppresses all recording.
 */

#include "pgxp.h"
#include "pgxp_hooks.h"
#include "cpu_state.h"
#include "psx_memory.h"
#include "mod_memory.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

/* gte.cpp — position-cache fallback tier (ambiguity-gated, G1.4 exact table) */
extern "C" int gte_geometry_correction_lookup(uint32_t packed,
                                              int32_t *x16, int32_t *y16);
extern "C" int gte_geometry_correction_lookup_probe(uint32_t packed,
                                                    int32_t *x16, int32_t *y16);

/* ------------------------------------------------------------------------- */
/* Shadow storage                                                             */
/* ------------------------------------------------------------------------- */

#include "pgxp_engine_types.h"

#define PGXP_TRANSFORM_RECORD(p,m) do { if(s_transform_recorder) s_transform_recorder(p,m); } while(0)
static PGXPTransformRecorder s_transform_recorder;
extern "C" void pgxp_set_transform_recorder(PGXPTransformRecorder recorder) { s_transform_recorder=recorder; }
static int s_interpolation_tracking;
#define PGXP_INTERPOLATION_TRACKING s_interpolation_tracking
extern "C" void pgxp_set_interpolation_tracking(int enabled) {
    if(s_interpolation_tracking==!!enabled) return;
    s_interpolation_tracking=!!enabled;
    pgxp_invalidate_all();
}
#define PGXP_S static
#define PGXP_API extern "C"
#include "pgxp_engine.inc"
#undef PGXP_S
#undef PGXP_API

extern "C" int pgxp_projective_scope_begin(PGXPProjectiveScope *scope) {
    if (!scope || scope->active) return 0;
    scope->saved_enabled = s_enabled;
    scope->saved_cpu_mode = s_cpu_mode;
    if (!scope->saved_enabled) {
        pgxp_set_enabled(1);
        if (!s_enabled) {
            s_cpu_mode = scope->saved_cpu_mode;
            return 0;
        }
    }
    s_cpu_mode = 1;
    scope->active = 1;
    return 1;
}

extern "C" void pgxp_projective_scope_end(PGXPProjectiveScope *scope) {
    if (!scope || !scope->active) return;
    s_cpu_mode = scope->saved_cpu_mode;
    if (!scope->saved_enabled) pgxp_set_enabled(0);
    scope->active = 0;
}

/* Draw-context snapshot (pgxp_ctx.cpp, docs/THREADED_CONTEXTS.md): the main
 * engine's configuration and live RAM/register/scratchpad shadows. Main thread only,
 * at a snapshot point; reads only. */
extern "C" int pgxp_main_export(PgxpExport *out) {
    out->ram_mask = g_psx_ram_mask;
    out->ram_count = 0;
    out->ram = nullptr;
    const uint32_t words = psx_ram_live_bytes() >> 2;
    if (s_enabled && s_ram) {
        for (uint32_t i = 0; i < words; ++i)
            if (s_ram[i].gen == s_gen && s_ram[i].flags) ++out->ram_count;
        if (out->ram_count) {
            out->ram = (PgxpRamShadow *)std::malloc(out->ram_count * sizeof(PgxpRamShadow));
            if (!out->ram) return 0;
            uint32_t n = 0;
            for (uint32_t i = 0; i < words; ++i)
                if (s_ram[i].gen == s_gen && s_ram[i].flags)
                    out->ram[n++] = {i, s_ram[i]};
        }
    }
    out->enabled = s_enabled;
    out->cpu_mode = s_cpu_mode;
    out->projection_tracking = s_projection_tracking;
    out->position_fallback = s_position_fallback;
    out->preserve_projection = s_preserve_projection;
    out->culling = s_culling;
    out->active = g_pgxp_active;
    out->tolerance = s_tolerance;
    out->gen = s_gen;
    std::memcpy(out->scratch, s_scratch, sizeof s_scratch);
    std::memcpy(out->gpr, s_gpr, sizeof s_gpr);
    std::memcpy(out->gte, s_gte, sizeof s_gte);
    std::memcpy(out->gtc, s_gtc, sizeof s_gtc);
    std::memcpy(out->gpr_s, s_gpr_s, sizeof s_gpr_s);
    std::memcpy(out->gte_s, s_gte_s, sizeof s_gte_s);
    std::memcpy(out->gtc_s, s_gtc_s, sizeof s_gtc_s);
    return 1;
}
