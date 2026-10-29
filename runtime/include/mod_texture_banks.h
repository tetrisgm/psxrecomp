#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Resolves lazily on the guest/producer thread. Successful definitions and
 * their pixel allocations are immutable and live until process termination;
 * presentation records may retain these pointers without calling a resolver. */
const uint16_t* mod_texture_bank_pixels(uint16_t id, uint32_t* width, uint32_t* height);
uint16_t mod_texture_packet_bank(uint32_t source, const uint32_t* words, uint32_t count);
int mod_texture_packet_precision(uint32_t source, float q[3], float xy[6]);
int mod_texture_bank_batchable(int immutable, int mask_check, int semi);
#ifdef __cplusplus
}
#endif
