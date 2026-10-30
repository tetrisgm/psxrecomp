#ifndef PSX_PGXP_CPU_STATE_H
#define PSX_PGXP_CPU_STATE_H
#include <stdint.h>
#include "pgxp.h"
/* Host-only precision state. Keeping the compiled hook view in the same
 * storage as the main engine avoids synchronization and snapshot ABI changes.
 * Private draw engines retain their own independent copies. */
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

typedef struct PGXPValue {
    int32_t  x16, y16;   /* sub-pixel screen coords, 16.16                    */
    uint16_t z;          /* projected SZ depth (perspective source), 0 = none */
    uint16_t flags;
    uint32_t value;      /* the guest word this shadow describes              */
    uint32_t gen;        /* valid iff == s_gen (O(1) invalidate-all)          */
    uint32_t source_address; /* original load address, valid with generation/value */
    PGXPProjection projection;
} PGXPValue;

typedef struct PGXPScalar {
    double   v;          /* precise value of the 32-bit register              */
    uint32_t value;      /* the register value it describes                   */
    uint32_t gen;
} PGXPScalar;

typedef struct PGXPCompiledState {
    PGXPValue gpr[34];
    PGXPScalar scalar[34];
    PGXPValue scratch[256];
    PGXPValue *ram;
    uint32_t gen, checkpoint_depth;
    int active, cpu_mode, store_trace, preserve_projection;
} PGXPCompiledState;
#ifdef __cplusplus
extern "C" {
#endif
extern PGXPCompiledState g_pgxp_cpu;
#ifdef __cplusplus
}
#endif
#endif
