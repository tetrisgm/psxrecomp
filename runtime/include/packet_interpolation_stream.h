#ifndef PSX_PACKET_INTERPOLATION_STREAM_H
#define PSX_PACKET_INTERPOLATION_STREAM_H
#include "pgxp.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Candidate provenance audit on the presenter. These are packet-owned
 * values, never pointers into the guest's PGXP engine. */
typedef struct PacketInterpolationSource {
    PGXPProjection projection[3];
    int32_t x[3],y[3];
    uint32_t valid_mask;
} PacketInterpolationSource;
int packet_interpolation_stream_enabled(void);
void packet_interpolation_record(const PacketInterpolationSource *source);
void packet_interpolation_stream_begin(uint64_t epoch,uint64_t sequence);
void packet_interpolation_stream_append(const PacketInterpolationSource *source);
void packet_interpolation_stream_end(void);
void packet_interpolation_stream_reset(void);
int packet_interpolation_stream_json(char *output,size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
