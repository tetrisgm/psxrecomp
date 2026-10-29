#ifndef PSX_PACKET_INTERPOLATION_STREAM_H
#define PSX_PACKET_INTERPOLATION_STREAM_H
#include "pgxp.h"
#include "packet_interpolation.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Candidate provenance audit on the presenter. These are packet-owned
 * values, never pointers into the guest's PGXP engine. */
typedef struct PacketInterpolationSource {
    uint32_t transform_only; /* pre-cull RTPS/RTPT source, projection[0] */
    PGXPProjection projection[3];
    int32_t x[3],y[3];
    uint32_t primitive_type, texpage, clut;
    uint32_t valid_mask;
    uint32_t spatial_mask; /* depth evidence, including missing provenance */
} PacketInterpolationSource;
int packet_interpolation_stream_enabled(void);
void packet_interpolation_set_rate(unsigned hz);
unsigned packet_interpolation_rate(void);
void packet_interpolation_display_rate(double hz);
size_t packet_interpolation_stream_count(void);
int packet_interpolation_stream_snapped(void);
int packet_interpolation_stream_sample(double t);
const PacketInterpolationPrimitive *packet_interpolation_stream_vertex(size_t index);
const PacketInterpolationPrimitive *packet_interpolation_stream_endpoint(size_t index);

typedef struct PacketTransformTable PacketTransformTable;
void packet_interpolation_transform(const PGXPProjection *projection);
PacketTransformTable *packet_interpolation_transform_seal(void);
void packet_interpolation_transform_free(PacketTransformTable *table);
size_t packet_interpolation_transform_bytes(const PacketTransformTable *table);
void packet_interpolation_stream_table(const PacketTransformTable *table);

typedef void (*PacketInterpolationRecorder)(const PacketInterpolationSource *source);
/* Install during presenter startup; callbacks synchronously copy the source
 * into an owned record. No presenter means this optional audit is inactive. */
void packet_interpolation_set_recorder(PacketInterpolationRecorder recorder);
void packet_interpolation_record(const PacketInterpolationSource *source);
void packet_interpolation_stream_begin(uint64_t epoch,uint64_t sequence);
void packet_interpolation_stream_append(const PacketInterpolationSource *source);
void packet_interpolation_stream_end(void);
void packet_interpolation_stream_reset(void);
int packet_interpolation_stream_json(char *output,size_t capacity);
void packet_interpolation_capture_request(void);
int packet_interpolation_capture_json(char *output,size_t capacity,int older,size_t start);
#ifdef __cplusplus
}
#endif
#endif
