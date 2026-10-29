#ifndef PSX_PACKET_PRESENTER_H
#define PSX_PACKET_PRESENTER_H
#include "frame_packet.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Consumer-owned retained packet and display clock. prepare builds the pair
 * without drawing; draw consumes only immutable packet inputs and sampled
 * vertices. Neither callback may execute guest code. */
typedef void (*PacketPresenterDraw)(const FramePacketView *, double);
void packet_presenter_reset(void);
int packet_presenter_submit(const FramePacketView *, uint64_t now,
                            double guest_hz, PacketPresenterDraw);
uint64_t packet_presenter_tick(uint64_t now, PacketPresenterDraw);
#ifdef __cplusplus
}
#endif
#endif
