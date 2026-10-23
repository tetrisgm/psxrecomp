#ifndef PSX_DCTX_BIOS_H
#define PSX_DCTX_BIOS_H
#include "cpu_state.h"
/* Qualified pure BIOS services. Returns zero without effects if the live
 * vector, dispatch stub, or implementation is not the verified code. */
int psx_dctx_bios_call(CPUState *cpu, uint32_t pc);
#endif
