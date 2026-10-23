/* pgxp_engine_types.h - shadow record types shared by the main PGXP engine
 * (pgxp.cpp) and the per-context engines (pgxp_ctx.cpp). Private to those
 * two translation units. See docs/THREADED_CONTEXTS.md. */
#pragma once

#include <cstdint>

#include "pgxp.h"
#include "psx_memory.h"
#include "mod_memory.h"

enum {
    PGXP_F_VX = 1u << 0,   /* low half  (screen X) tracked                    */
    PGXP_F_VY = 1u << 1,   /* high half (screen Y) tracked                    */
    PGXP_F_VZ = 1u << 2,   /* projected depth rode along intact               */
    PGXP_F_PROJECTION = 1u << 3,
    /* The tracked half was DERIVED by guest arithmetic that rounds (a CPU
     * divide of a precise sum, see the scalar tier below), so the guest
     * integer is that arithmetic's rounding of the value, not its floor: the
     * precise value may sit up to one pixel below or two above it. */
    PGXP_F_DX = 1u << 4,
    PGXP_F_DY = 1u << 5,
    PGXP_F_UNCLAMPED = 1u << 6, /* exact GTE projection; intact word only */
    PGXP_F_VXY = PGXP_F_VX | PGXP_F_VY,
};

struct PGXPValue {
    int32_t  x16, y16;   /* sub-pixel screen coords, 16.16                    */
    uint16_t z;          /* projected SZ depth (perspective source), 0 = none */
    uint16_t flags;
    uint32_t value;      /* the guest word this shadow describes              */
    uint32_t gen;        /* valid iff == s_gen (O(1) invalidate-all)          */
    PGXPProjection projection;
};

/* Shadow covers the host RAM backing so the opt-in 8 MB map tracks its high
 * banks; retail sessions only ever touch the low 2 MiB of it. The same array
 * continues with the trusted-mod guest regions: the GPU DMA aperture (an
 * enhancement's enlarged primitive arena, e.g. Crash widescreen) and the
 * Expansion 1 mod memory. Packets built there are tracked exactly like
 * packets in RAM; without this every vertex an enlarged arena carried drew
 * native and had no depth. */
#define PGXP_RAM_WORDS      (PSX_MAIN_RAM_BACKING_BYTES >> 2)
#define PGXP_APERTURE_WORDS (PSX_MOD_GPU_DMA_APERTURE_SIZE >> 2)
#define PGXP_MODMEM_WORDS   (PSX_MOD_MEMORY_SIZE >> 2)
#define PGXP_SHADOW_WORDS   (PGXP_RAM_WORDS + PGXP_APERTURE_WORDS + PGXP_MODMEM_WORDS)
#define PGXP_SCRATCH_WORDS (0x400u >> 2)      /* 1 KB scratchpad              */
#define PGXP_REG_HI        32
#define PGXP_REG_LO        33

/* Scalar tier (cpu-mode). Engines also use the GTE as a multiplier for
 * vertex arithmetic: Ape Escape's terrain subdivider packs two projected X
 * (or Y) halves into a light-matrix row with CTC2, weights them with MVMVA
 * by their depths, reads MAC back with MFC2 and DIVs by the depth sum to get
 * the screen position of an edge midpoint. A 16.16 half cannot hold those
 * intermediate products, so registers carry an optional full-range precise
 * value beside their half shadows: valid while `gen` is current and `value`
 * still equals the register, killed by every hooked write that does not
 * produce one. It turns back into a half shadow only when stored (SH / SW)
 * as a 16-bit coordinate. */
struct PGXPScalar {
    double   v;          /* precise value of the 32-bit register              */
    uint32_t value;      /* the register value it describes                   */
    uint32_t gen;
};
struct PgxpRamShadow {
    uint32_t index;
    PGXPValue value;
};

/* Main-engine state a draw-context snapshot owns. Only live RAM shadows are
 * copied: old generations and untracked slots have no observable precision.
 * The context memory map currently supports RAM and scratchpad, not mod RAM. */
struct PgxpExport {
    int      enabled, cpu_mode, projection_tracking, position_fallback;
    int      preserve_projection, culling, active;
    float    tolerance;
    uint32_t gen;
    uint32_t ram_mask, ram_count;
    PgxpRamShadow *ram;
    PGXPValue  scratch[PGXP_SCRATCH_WORDS];
    PGXPValue  gpr[34], gte[32], gtc[32];
    PGXPScalar gpr_s[34], gte_s[32], gtc_s[32];
};
