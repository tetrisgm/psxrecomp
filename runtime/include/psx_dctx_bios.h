#ifndef PSX_DCTX_BIOS_H
#define PSX_DCTX_BIOS_H
#include "cpu_state.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Qualified pure BIOS services. Returns zero without effects if the live
 * vector, dispatch stub, or implementation is not the verified code. */
int psx_dctx_bios_call(CPUState *cpu, uint32_t pc);
#ifdef __cplusplus
}
#endif
#endif
