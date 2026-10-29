#ifndef PSX_PACKET_INTERPOLATION_H
#define PSX_PACKET_INTERPOLATION_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Renderer-only, immutable input. Identity is provenance, never colour, UV,
 * packet address, screen position, depth bucket or order in a draw list.
 * Duplicate instances pair by occurrence when counts agree; otherwise a
 * gated minimum-cost centroid assignment resolves the group. */
typedef struct FramePrimitiveIdentity { uint64_t object, primitive; } FramePrimitiveIdentity;
typedef struct PacketInterpolationVertex {
    double view[3];
    double h, ofx, ofy, x_scale;
    double screen[2]; /* precise packet N endpoint, including GPU transforms */
    double uv[2];
    uint32_t colour;
} PacketInterpolationVertex;
typedef struct PacketInterpolationPrimitive {
    FramePrimitiveIdentity identity;
    uint32_t vertices; /* 3 or 4 */
    uint32_t spatial;  /* 0: HUD/2D, takes packet N verbatim */
    PacketInterpolationVertex vertex[4];
} PacketInterpolationPrimitive;
typedef struct PacketInterpolationPacket {
    uint64_t epoch, sequence;
    const PacketInterpolationPrimitive *primitives;
    size_t count;
    int discontinuity; /* explicit cut, respawn or timeline reset */
} PacketInterpolationPacket;
typedef struct PacketInterpolationStats {
    uint64_t matched, unmatched, disappeared, ambiguous, invalid;
    uint64_t duplicate_equal;
    uint64_t resolved_occurrence, resolved_assignment, appeared_candidates;
    int snapped;
} PacketInterpolationStats;
typedef struct PacketInterpolation PacketInterpolation;
PacketInterpolation *packet_interpolation_create(void);
void packet_interpolation_destroy(PacketInterpolation *context);
/* Copies the pair. All later sampling is independent of packet slots and
 * guest memory. Allocation failure clears the old pair and returns zero.
 * max_motion is camera-space displacement / newer depth; <=0 disables the
 * geometric cut guard. Explicit discontinuities always snap the entire pair. */
int packet_interpolation_pair(PacketInterpolation *context,
    const PacketInterpolationPacket *older,const PacketInterpolationPacket *newer,
    double max_motion);
/* Writes exactly packet N's primitives in N's order. Colours, UVs, 2D and
 * unmatched primitives remain bit-for-bit N. Projection is applied to lerped
 * view-space positions. No allocations, GL calls, clocks or guest callbacks. */
int packet_interpolation_sample(const PacketInterpolation *context,double t,
    PacketInterpolationPrimitive *output,size_t capacity);
PacketInterpolationStats packet_interpolation_stats(const PacketInterpolation *context);
/* Absolute rational output cadence: no accumulated truncated-period drift.
 * index zero is the first output. Returns UINT64_MAX on invalid/overflow. */
uint64_t packet_interpolation_deadline(uint64_t start_ns,uint64_t index,
    uint32_t rate_num,uint32_t rate_den);
#ifdef __cplusplus
}
#endif
#endif
