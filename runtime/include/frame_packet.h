#ifndef PSX_FRAME_PACKET_H
#define PSX_FRAME_PACKET_H
#include <stddef.h>
#include <stdint.h>
#include "render_thread.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Owned, bounded presentation packets. The producer supplies a complete
 * initial state and native inputs, never references into the live machine.
 * Neither publication nor replacement waits for a consumer. Commands within
 * a packet retain their original order. This transport does not decide which
 * renderer commands are state, nor make incremental streams independent. */
typedef struct FramePacketQueue FramePacketQueue;
typedef struct FramePacketView {
    uint64_t sequence,epoch;
    const void *initial;
    size_t initial_bytes;
    const void *records;
    size_t record_bytes;
    const void *attachment;
} FramePacketView;

/* Limits are per slot, with three slots total (writing, pending, reading).
 * Allocate/grow only on the producer. A failed append invalidates the whole
 * packet; it can never publish a truncated draw list. */
FramePacketQueue *frame_packet_create(size_t initial_limit,size_t record_limit);
void frame_packet_destroy(FramePacketQueue *queue); /* stopped owners only */
int frame_packet_begin(FramePacketQueue *queue,uint64_t sequence,
                       const void *initial,size_t bytes);
/* Reserve the owned seed directly, avoiding an intermediate full native VRAM
 * copy. Fill every byte before publish; the pointer expires on publication. */
int frame_packet_reserve_initial(FramePacketQueue *queue,uint64_t sequence,size_t bytes,void **initial);
/* Takes ownership only on success. One immutable CPU-only attachment per
 * packet, accounted against initial_limit along with the seed. Failure
 * invalidates the open frame, so missing provenance cannot be published. Its disposer
 * runs on slot reuse/destruction, never while the consumer holds that slot;
 * it must not call GL, wait for another owner, or reference live guest data. */
int frame_packet_attach(FramePacketQueue *queue,void *attachment,size_t bytes,
                        void (*dispose)(void*));
void *frame_packet_append(FramePacketQueue *queue,uint16_t op,uint16_t flags,
                          uint32_t bytes);
/* Append an optional private batch atomically. Invalid/over-budget batches
 * return zero without invalidating the main frame or appending a prefix.
 * Records use RtCmd headers/alignment and own all payload bytes afterward. */
int frame_packet_append_records(FramePacketQueue *queue,const void *records,size_t bytes);
/* Discard only the open producer frame, preserving the last complete pending
 * frame and its epoch. Used for a failed native-input capture. */
void frame_packet_cancel(FramePacketQueue *queue);
int frame_packet_publish(FramePacketQueue *queue);
int frame_packet_acquire(FramePacketQueue *queue,FramePacketView *view);
void frame_packet_release(FramePacketQueue *queue);
uint64_t frame_packet_replaced(const FramePacketQueue *queue);
int frame_packet_pending(const FramePacketQueue *queue);
/* Producer: load/rewind changes timeline. Discard pending/open packets;
 * an already leased packet remains immutable, but is no longer presentable.
 * Consumer checks current immediately before handing an image to present. */
void frame_packet_invalidate(FramePacketQueue *queue);
int frame_packet_current(const FramePacketQueue *queue,const FramePacketView *view);

#ifdef __cplusplus
}
#endif
#endif
