#ifndef PSX_FRAME_OWNER_H
#define PSX_FRAME_OWNER_H
#include "frame_packet.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Dedicated presentation owner. Context transfers happen at start/shutdown
 * only. execute runs outside every transport lock; a stalled graphics driver
 * cannot prevent the guest from replacing pending presentation packets. */
typedef struct FrameOwner FrameOwner;
typedef void (*FrameOwnerExecute)(void *user,const FramePacketView *packet);
typedef struct FrameOwnerConfig {
    size_t initial_limit,record_limit;
    FrameOwnerExecute execute;
    RtCtxFn context;
    void *user;
    RtTickFn tick;                 /* optional render-side timed work */
    uint64_t (*clock_ns)(void);    /* same monotonic domain as tick deadlines */
} FrameOwnerConfig;
FrameOwner *frame_owner_start(const FrameOwnerConfig *config);
void frame_owner_stop(FrameOwner *owner);
/* Producer-only recording API. Failed append invalidates this whole frame. */
int frame_owner_begin(FrameOwner *owner,uint64_t sequence,const void *initial,size_t bytes);
int frame_owner_attach(FrameOwner *owner,void *attachment,size_t bytes,void (*dispose)(void*));
void *frame_owner_append(FrameOwner *owner,uint16_t op,uint16_t flags,uint32_t bytes);
int frame_owner_publish(FrameOwner *owner);
void frame_owner_invalidate(FrameOwner *owner);
/* Consumer check before presenting; producer invalidation never frees a
 * consumer lease. Keep the image's epoch with any subsequent present job. */
int frame_owner_current(const FrameOwner *owner,const FramePacketView *packet);
uint64_t frame_owner_replaced(const FrameOwner *owner);
uint64_t frame_owner_completed(const FrameOwner *owner);
int frame_owner_is_thread(void);
/* Consumer only; earliest request wins, including requests made by tick. */
void frame_owner_tick_at(FrameOwner *owner,uint64_t deadline_ns);
#ifdef __cplusplus
}
#endif
#endif
