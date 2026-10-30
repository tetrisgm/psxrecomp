#ifndef PSX_PACKET_PRESENTER_H
#define PSX_PACKET_PRESENTER_H
#include "frame_packet.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Consumer-owned retained packet and display clock. prepare builds the pair
 * without drawing; draw consumes only immutable packet inputs and sampled
 * vertices. t=-1 requests the retained left endpoint verbatim; t=1 is
 * the current endpoint. Neither callback may execute guest code. */
typedef void (*PacketPresenterDraw)(const FramePacketView *, double);
void packet_presenter_reset(void);
/* Render-owner bounded delivery buffer, before any pair/metadata preparation.
 * prepare receives the mapped deadline, not wall-clock arrival. */
typedef void (*PacketPresenterPrepare)(const FramePacketView *,uint64_t);
void packet_presenter_buffer_reset(void);
int packet_presenter_buffer(const FramePacketView *,uint64_t,double,PacketPresenterPrepare);
uint64_t packet_presenter_deliver(uint64_t,PacketPresenterPrepare);
int packet_presenter_active(void);
const FramePacketView *packet_presenter_previous(void);
int packet_presenter_json(char *,size_t);
/* Debug request, consumed asynchronously by the render owner. -1: disabled. */
int packet_presenter_request_sample(int thousandths);
int packet_presenter_submit(const FramePacketView *, uint64_t now,
                            double guest_hz, PacketPresenterDraw);
uint64_t packet_presenter_tick(uint64_t now, PacketPresenterDraw);
#ifdef __cplusplus
}
#endif
#endif
