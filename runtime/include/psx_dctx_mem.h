#ifndef PSXRECOMP_PSX_DCTX_MEM_H
#define PSXRECOMP_PSX_DCTX_MEM_H

/* Guest memory access for draw contexts (docs/THREADED_CONTEXTS.md).
 *
 * Used by the lean draw clones (PSX_DRAW_CTX_BUILD) and by the context
 * runtime. Main RAM and the scratchpad are the context's private copies,
 * reached inline; everything else (BIOS ROM, GPU/DMA/INTC registers, other
 * devices) goes through psx_dctx_read_slow / psx_dctx_write_slow in
 * psx_exec_ctx.c. Never touches the main context's memory. */

#include <stdint.h>
#include <string.h>

#include "psx_exec_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t psx_dctx_read_slow(CPUState *cpu, uint32_t addr, uint32_t size);
void     psx_dctx_write_slow(CPUState *cpu, uint32_t addr, uint32_t value, uint32_t size);

/* Host pointer to `size` bytes at `addr` in context RAM/scratchpad, or NULL. */
static inline uint8_t *psx_dctx_ptr(PsxExecCtx *x, uint32_t addr) {
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < 0x00800000u) return x->ram + (phys & x->ram_mask);
    if ((phys & 0xFFFFFC00u) == 0x1F800000u) return x->scratch + (phys & 0x3FFu);
    return 0;
}

static inline uint32_t psx_dctx_lw(CPUState *cpu, uint32_t addr) {
    uint8_t *p = psx_dctx_ptr(cpu->ctx, addr);
    uint32_t v;
    if (!p) return psx_dctx_read_slow(cpu, addr, 4u);
    memcpy(&v, p, 4);
    return v;
}

static inline uint16_t psx_dctx_lh(CPUState *cpu, uint32_t addr) {
    uint8_t *p = psx_dctx_ptr(cpu->ctx, addr);
    uint16_t v;
    if (!p) return (uint16_t)psx_dctx_read_slow(cpu, addr, 2u);
    memcpy(&v, p, 2);
    return v;
}

static inline uint8_t psx_dctx_lb(CPUState *cpu, uint32_t addr) {
    uint8_t *p = psx_dctx_ptr(cpu->ctx, addr);
    if (!p) return (uint8_t)psx_dctx_read_slow(cpu, addr, 1u);
    return *p;
}

/* Stores while SR.IsC (cache isolation, bit 16) is set reach the cache, not
 * RAM; the slow path counts and drops them. */
static inline void psx_dctx_sw(CPUState *cpu, uint32_t addr, uint32_t v) {
    uint8_t *p = psx_dctx_ptr(cpu->ctx, addr);
    if (!p || (cpu->cop0[12] & 0x10000u)) { psx_dctx_write_slow(cpu, addr, v, 4u); return; }
    memcpy(p, &v, 4);
}

static inline void psx_dctx_sh(CPUState *cpu, uint32_t addr, uint16_t v) {
    uint8_t *p = psx_dctx_ptr(cpu->ctx, addr);
    if (!p || (cpu->cop0[12] & 0x10000u)) { psx_dctx_write_slow(cpu, addr, v, 2u); return; }
    memcpy(p, &v, 2);
}

static inline void psx_dctx_sb(CPUState *cpu, uint32_t addr, uint8_t v) {
    uint8_t *p = psx_dctx_ptr(cpu->ctx, addr);
    if (!p || (cpu->cop0[12] & 0x10000u)) { psx_dctx_write_slow(cpu, addr, v, 1u); return; }
    *p = v;
}

#ifdef __cplusplus
}
#endif

#endif
