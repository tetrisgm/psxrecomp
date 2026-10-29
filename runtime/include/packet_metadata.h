#ifndef PSX_PACKET_METADATA_H
#define PSX_PACKET_METADATA_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct PacketMetadata PacketMetadata;
typedef int (*PacketMetadataCapture)(void *output);
typedef void (*PacketMetadataBlend)(const void *older,const void *newer,double t,void *output);
/* Register at startup. Capture runs on the producer; blend and read run on
 * the presentation owner. Payloads must contain values, never guest pointers. */
int packet_metadata_register(uint64_t key,size_t bytes,PacketMetadataCapture,PacketMetadataBlend);
PacketMetadata *packet_metadata_capture(void);
void packet_metadata_free(PacketMetadata *);
size_t packet_metadata_bytes(const PacketMetadata *);
void packet_metadata_pair(const PacketMetadata *,uint64_t epoch);
void packet_metadata_sample(double t);
/* Borrow only while replaying this retained packet on the render owner. */
void packet_metadata_endpoint(const PacketMetadata *);
void packet_metadata_end(void);
void packet_metadata_reset(void);
int packet_metadata_read(uint64_t key,void *out,size_t bytes);
#ifdef __cplusplus
}
#endif
#endif
