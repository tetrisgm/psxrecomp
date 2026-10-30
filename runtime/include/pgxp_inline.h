#ifndef PSX_PGXP_INLINE_H
#define PSX_PGXP_INLINE_H
#include "pgxp_cpu_state.h"
#include "psx_memory.h"
#include "mod_memory.h"
#include <string.h>
/* Keep the ordinary compiled path contiguous; exceptional observers still run. */
#if defined(__GNUC__) || defined(__clang__)
#define PGXP_LIKELY(x) __builtin_expect(!!(x), 1)
#else
#define PGXP_LIKELY(x) (!!(x))
#endif
#if defined(_MSC_VER)
#define PGXP_INLINE static __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define PGXP_INLINE static inline __attribute__((always_inline))
#else
#define PGXP_INLINE static inline
#endif
/* Literal instruction fields fold away in generated code. Checkpoints and
 * explicitly armed diagnostics use the original journal/recording path. */
PGXP_INLINE PGXPValue *pgxp_inline_ptr(uint32_t addr) {
    uint32_t m = addr & 0x1fffffffu;
    if (PGXP_LIKELY(m < PSX_MAIN_RAM_WINDOW_BYTES))
        return g_pgxp_cpu.ram ? &g_pgxp_cpu.ram[(m & g_psx_ram_mask) >> 2] : NULL;
    if (m - PSX_MOD_GPU_DMA_APERTURE_BASE < PSX_MOD_GPU_DMA_APERTURE_SIZE)
        return g_pgxp_cpu.ram ? &g_pgxp_cpu.ram[(PSX_MAIN_RAM_BACKING_BYTES >> 2) +
            ((m - PSX_MOD_GPU_DMA_APERTURE_BASE) >> 2)] : NULL;
    if (m - PSX_MOD_MEMORY_BASE < PSX_MOD_MEMORY_SIZE)
        return g_pgxp_cpu.ram ? &g_pgxp_cpu.ram[(PSX_MAIN_RAM_BACKING_BYTES >> 2) +
            (PSX_MOD_GPU_DMA_APERTURE_SIZE >> 2) + ((m - PSX_MOD_MEMORY_BASE) >> 2)] : NULL;
    if ((m & 0xfffffc00u) == 0x1f800000u) return &g_pgxp_cpu.scratch[(m & 0x3fcu) >> 2];
    return NULL;
}
PGXP_INLINE void pgxp_inline_pv_validate(PGXPValue *pv, uint32_t actual) {
    if (pv->gen != g_pgxp_cpu.gen) return;
    uint32_t diff = pv->value ^ actual;
    if (diff) pv->source_address = 0;
    if (diff) pv->flags &= (uint16_t)~(PGXP_F_PROJECTION | PGXP_F_UNCLAMPED | PGXP_AUDIT_MASK);
    if ((pv->flags & PGXP_F_VX) && (diff & 0x0000FFFFu))
        pv->flags &= (uint16_t)~(PGXP_F_VX | PGXP_F_DX | PGXP_F_VZ);
    if ((pv->flags & PGXP_F_VY) && (diff & 0xFFFF0000u))
        pv->flags &= (uint16_t)~(PGXP_F_VY | PGXP_F_DY | PGXP_F_VZ);
    pv->value = actual;
}
PGXP_INLINE void pgxp_inline_pv_reset(PGXPValue *pv, uint32_t value) {
    pv->x16 = 0; pv->y16 = 0; pv->z = 0;
    pv->flags = 0;
    pv->source_address = 0;
    pv->value = value;
    pv->gen = g_pgxp_cpu.gen;
}
PGXP_INLINE int pgxp_inline_half_scalar(const PGXPValue *pv, uint32_t value, double *out) {
    if (!pv || pv->gen != g_pgxp_cpu.gen || pv->value != value) return 0;
    if ((pv->flags & PGXP_F_VXY) != PGXP_F_VXY) return 0;
    const int32_t sx = (int32_t)(int16_t)(value & 0xFFFFu);
    const int32_t zx = (int32_t)(value & 0xFFFFu);
    if ((int32_t)value != sx && (int32_t)value != zx) return 0;
    if (pv->y16 != (int32_t)(value & 0xffff0000u)) return 0;  /* exact extension */
    double v = pv->x16 / 65536.0;                 /* the half read as int16       */
    if ((int32_t)value == zx && zx != sx) v += 65536.0;   /* LHU of a negative half */
    *out = v;
    return 1;
}
PGXP_INLINE void psx_pgxp_load_inline(struct CPUState *cpu, uint32_t instr,
                              uint32_t addr, uint32_t value) {
    (void)cpu;
    if (!PGXP_LIKELY(g_pgxp_cpu.active)) return;
    if (!PGXP_LIKELY(!g_pgxp_cpu.checkpoint_depth)) { psx_pgxp_load(cpu, instr, addr, value); return; }
    uint32_t rt = ((instr >> 16) & 31u);
    if (rt == 0) return;
    PGXPValue *dst = &g_pgxp_cpu.gpr[rt];
    g_pgxp_cpu.scalar[rt].gen = 0;                           /* memory carries no scalars   */

    uint32_t lop = (instr >> 26);
    /* LWL at byte 3 and LWR at byte 0 of a word each load the WHOLE word
     * (little-endian R3000A): the unaligned-copy idiom lwl/lwr on an aligned
     * address is a plain word load, so it carries the word's shadow. */
    if ((lop == 0x22 && (addr & 3u) == 3u) || (lop == 0x26 && (addr & 3u) == 0u))
        lop = 0x23;
    switch (lop) {
    case 0x23: {                               /* LW                          */
        PGXPValue *src = pgxp_inline_ptr(addr);     /* validation may mutate it    */
        if (src && src->gen == g_pgxp_cpu.gen) {
            pgxp_inline_pv_validate(src, value);
            *dst = *src;
        } else {
            pgxp_inline_pv_reset(dst, value);
        }
        if(!dst->source_address) dst->source_address = addr & 0x1fffffffu;
        return;
    }
    case 0x21:                                 /* LH                          */
    case 0x25: {                               /* LHU                         */
        PGXPValue *src = pgxp_inline_ptr(addr);
        int hi_half = (addr >> 1) & 1;
        pgxp_inline_pv_reset(dst, value);
        /* The high half of the extended GPR is a known-exact constant
         * (0/-1 for LH, 0 for LHU): track it so re-packing via sll/or in
         * cpu-mode keeps working. */
        dst->y16 = (int32_t)(value & 0xffff0000u);
        dst->flags = PGXP_F_VY;
        if (src && src->gen == g_pgxp_cpu.gen && !(src->flags & PGXP_F_UNCLAMPED)) {
            uint32_t actual_half = (value & 0xFFFFu) << (hi_half ? 16 : 0);
            uint32_t mask = hi_half ? 0xFFFF0000u : 0x0000FFFFu;
            uint16_t want = hi_half ? PGXP_F_VY : PGXP_F_VX;
            if ((src->flags & want) && ((src->value ^ actual_half) & mask) == 0) {
                dst->x16 = hi_half ? src->y16 : src->x16;
                dst->flags |= PGXP_F_VX;
                if (src->flags & (hi_half ? PGXP_F_DY : PGXP_F_DX))
                    dst->flags |= PGXP_F_DX;
            }
        }
        return;
    }
    default:                                   /* LB/LBU/LWL/LWR: untrackable */
        pgxp_inline_pv_reset(dst, value);
        return;
    }
}


PGXP_INLINE void psx_pgxp_store_inline(struct CPUState *cpu, uint32_t instr,
                                       uint32_t addr, uint32_t value) {
    if (!PGXP_LIKELY(g_pgxp_cpu.active)) return;
    uint32_t rt = (instr >> 16) & 31u, op = instr >> 26;
    if (g_pgxp_cpu.checkpoint_depth || g_pgxp_cpu.store_trace ||
        (g_pgxp_cpu.cpu_mode && rt && g_pgxp_cpu.scalar[rt].gen == g_pgxp_cpu.gen)) {
        psx_pgxp_store(cpu, instr, addr, value); return;
    }
    PGXPValue *dst = pgxp_inline_ptr(addr);
    if (!dst) return;
    PGXPValue *src = rt ? &g_pgxp_cpu.gpr[rt] : NULL;
    if ((op == 0x2a && (addr & 3u) == 3u) || (op == 0x2e && !(addr & 3u))) op = 0x2b;
    if (op == 0x2b) {
        if (src && src->gen == g_pgxp_cpu.gen) {
            pgxp_inline_pv_validate(src, value); *dst = *src;
        } else pgxp_inline_pv_reset(dst, value);
    } else if (op == 0x29) {
        uint32_t hi = (addr >> 1) & 1u, half = value & 0xffffu;
        if (dst->gen != g_pgxp_cpu.gen) pgxp_inline_pv_reset(dst, half << (hi ? 16 : 0));
        dst->value = hi ? (dst->value & 0xffffu) | (half << 16) :
                         (dst->value & 0xffff0000u) | half;
        dst->flags &= (uint16_t)~((hi ? (PGXP_F_VY | PGXP_F_DY) : (PGXP_F_VX | PGXP_F_DX)) |
            PGXP_F_VZ | PGXP_F_PROJECTION | PGXP_F_UNCLAMPED | PGXP_AUDIT_MASK);
        dst->z = 0; dst->source_address = 0;
        if (src && src->gen == g_pgxp_cpu.gen && !(src->flags & PGXP_F_UNCLAMPED) &&
            (src->flags & PGXP_F_VX) && !((src->value ^ value) & 0xffffu)) {
            uint16_t d = (src->flags & PGXP_F_DX) ? (hi ? PGXP_F_DY : PGXP_F_DX) : 0;
            if (hi) { dst->y16 = src->x16; dst->flags |= PGXP_F_VY | d; }
            else { dst->x16 = src->x16; dst->flags |= PGXP_F_VX | d; }
        }
    } else if (op == 0x28) {
        if (dst->gen != g_pgxp_cpu.gen) return;
        uint32_t sh = (addr & 3u) * 8u;
        dst->value = (dst->value & ~(0xffu << sh)) | ((value & 0xffu) << sh);
        dst->flags &= (uint16_t)~(((addr & 2u) ? (PGXP_F_VY | PGXP_F_DY) : (PGXP_F_VX | PGXP_F_DX)) |
            PGXP_F_VZ | PGXP_F_PROJECTION | PGXP_F_UNCLAMPED | PGXP_AUDIT_MASK);
        dst->z = 0; dst->source_address = 0;
    } else dst->gen = 0; /* partial unaligned stores are not trackable */
}
PGXP_INLINE int pgxp_inline_has_precision(uint32_t reg) {
    return reg && ((g_pgxp_cpu.gpr[reg].gen == g_pgxp_cpu.gen &&
        (g_pgxp_cpu.gpr[reg].flags & PGXP_F_VXY)) ||
        (g_pgxp_cpu.cpu_mode && g_pgxp_cpu.scalar[reg].gen == g_pgxp_cpu.gen));
}
PGXP_INLINE void pgxp_inline_reset_gpr(uint32_t reg, uint32_t value) {
    if (!reg) return;
    pgxp_inline_pv_reset(&g_pgxp_cpu.gpr[reg], value);
    if (g_pgxp_cpu.cpu_mode) g_pgxp_cpu.scalar[reg].gen = 0;
}
PGXP_INLINE void pgxp_inline_move(uint32_t dst, uint32_t src, uint32_t value) {
    if (!dst) return;
    double scalar = 0; int precise = 0;
    if (g_pgxp_cpu.cpu_mode && src) {
        PGXPScalar *s = &g_pgxp_cpu.scalar[src];
        if (s->gen == g_pgxp_cpu.gen && s->value == value) { precise = 1; scalar = s->v; }
        else precise = pgxp_inline_half_scalar(&g_pgxp_cpu.gpr[src], value, &scalar);
    }
    if (src && g_pgxp_cpu.gpr[src].gen == g_pgxp_cpu.gen) {
        g_pgxp_cpu.gpr[dst] = g_pgxp_cpu.gpr[src];
        pgxp_inline_pv_validate(&g_pgxp_cpu.gpr[dst], value);
    } else pgxp_inline_pv_reset(&g_pgxp_cpu.gpr[dst], value);
    if (g_pgxp_cpu.cpu_mode) {
        PGXPScalar *s = &g_pgxp_cpu.scalar[dst];
        if (precise) { s->v = scalar; s->value = value; s->gen = g_pgxp_cpu.gen; }
        else s->gen = 0;
    }
}
PGXP_INLINE int pgxp_inline_comp_tracked(const PGXPValue *pv, uint32_t value, int hi_half) {
    if (!pv || pv->gen != g_pgxp_cpu.gen || (pv->flags & PGXP_F_UNCLAMPED)) return 0;
    if (!hi_half)
        return (pv->flags & PGXP_F_VX) &&
               ((pv->value ^ value) & 0x0000FFFFu) == 0;
    return (pv->flags & PGXP_F_VY) &&
           ((pv->value ^ value) & 0xFFFF0000u) == 0;
}

PGXP_INLINE int pgxp_inline_half_exact(const PGXPValue *pv, uint32_t value, int hi_half) {
    if (!pgxp_inline_comp_tracked(pv, value, hi_half)) return 0;
    int32_t v16 = hi_half ? pv->y16 : pv->x16;
    const int32_t half = (int32_t)(int16_t)(hi_half ? (value >> 16) : value);
    /* A derived half keeps its rounding window around the integer. */
    if (pv->flags & (hi_half ? PGXP_F_DY : PGXP_F_DX)) {
        const int64_t d = (int64_t)v16 - (int64_t)half * 65536;
        return d > -65536 && d < 2 * 65536;
    }
    /* A saturated projection shadow sits beyond the clamped word; it is not
     * the value of that half and must not be carried as one. */
    if (g_pgxp_cpu.preserve_projection) {
        const int32_t field = (int32_t)((uint32_t)half << 21) >> 21;
        const int64_t d = (int64_t)v16 - (int64_t)half * 65536;
        // Clip metadata uses bits 13..15. Bits 11/12 must still be the
        // coordinate's sign fill; a real wrapped coordinate stays strict.
        const uint16_t sign_fill = field < 0 ? 0x1800u : 0;
        if (field > -0x400 && field < 0x3FF &&
            ((uint16_t)(half) & 0x1800u) == sign_fill)
            return d > -(int64_t)PGXP_PPP_AGREE_BELOW * 65536 &&
                   d < (int64_t)PGXP_PPP_AGREE_ABOVE * 65536;
    }
    return (v16 >> 16) == half;
}

PGXP_INLINE int pgxp_inline_carry_field(PGXPValue *dst, const PGXPValue *src,
                              uint32_t sval, uint32_t result, int hi_half) {
    if (!pgxp_inline_half_exact(src, sval, hi_half)) return 0;
    uint32_t sh = hi_half ? 16u : 0u;
    if ((((sval ^ result) >> sh) & 0x7FFu) != 0) return 0;
    /* Keep the precise offset from the operand's integer and rebase it on
     * the result half's integer (equal field, flag bits may differ). */
    const int32_t from = (int32_t)(int16_t)(sval >> sh);
    const int32_t to = (int32_t)(int16_t)(result >> sh);
    const int32_t v = (int32_t)((int64_t)(hi_half ? src->y16 : src->x16) +
                                ((int64_t)to - from) * 65536);
    const uint16_t d = src->flags & (hi_half ? PGXP_F_DY : PGXP_F_DX);
    if (hi_half) { dst->y16 = v; dst->flags |= (uint16_t)(PGXP_F_VY | d); }
    else         { dst->x16 = v; dst->flags |= (uint16_t)(PGXP_F_VX | d); }
    return 1;
}

PGXP_INLINE int pgxp_inline_has_depth(const PGXPValue *pv) {
    return pv && pv->gen == g_pgxp_cpu.gen && (pv->flags & PGXP_F_VZ) && pv->z != 0;
}

/* Depth belongs to a whole vertex: it survives an op only when both halves of
 * the result came from one operand that carried it, and the other operand is
 * not itself a vertex (a constant, a mask, an offset). */
PGXP_INLINE void pgxp_inline_keep_depth(PGXPValue *dst, const PGXPValue *p,
                              const PGXPValue *other) {
    if ((dst->flags & PGXP_F_VXY) == PGXP_F_VXY && pgxp_inline_has_depth(p) &&
        !pgxp_inline_has_depth(other)) {
        dst->z = p->z;
        dst->flags |= PGXP_F_VZ;
    }
}

/* Precise-half arithmetic preserves the reference carry/depth rules. */
PGXP_INLINE void pgxp_inline_alu_bitwise(
PGXPValue *dst, const PGXPValue *a, uint32_t s1,
                        const PGXPValue *b, uint32_t s2, uint32_t result) {
    pgxp_inline_pv_reset(dst, result);
    /* Prefer the operand that is a whole vertex, so a vertex ORed with flags
     * keeps its depth even when the flag word happens to share a field. */
    const PGXPValue *p = a; uint32_t pval = s1;
    const PGXPValue *q = b; uint32_t qval = s2;
    if (!pgxp_inline_has_depth(a) && pgxp_inline_has_depth(b)) { p = b; pval = s2; q = a; qval = s1; }
    const PGXPValue *from[2] = { NULL, NULL };
    for (int hi = 0; hi < 2; hi++) {
        if (pgxp_inline_carry_field(dst, p, pval, result, hi))      from[hi] = p;
        else if (pgxp_inline_carry_field(dst, q, qval, result, hi)) from[hi] = q;
        else if (((hi ? result >> 16 : result) & 0xFFFFu) == 0) {
            /* An exactly-zero half is an exact coordinate (the repack
             * pattern ORs a shifted half into a zero half). */
            if (hi) { dst->y16 = 0; dst->flags |= PGXP_F_VY; }
            else    { dst->x16 = 0; dst->flags |= PGXP_F_VX; }
        }
    }
    if (from[0] && from[0] == from[1])
        pgxp_inline_keep_depth(dst, from[0], from[0] == p ? q : p);
}


/* Bitwise arithmetic consumes only this prefix. Snapshot it before a
 * destination reset, without copying or initializing unused projection data. */
PGXP_INLINE void pgxp_inline_half_snapshot(PGXPValue *out, uint32_t reg) {
    if (reg) {
        const PGXPValue *src = &g_pgxp_cpu.gpr[reg];
        out->x16=src->x16; out->y16=src->y16; out->z=src->z;
        out->flags=src->flags; out->value=src->value; out->gen=src->gen;
    } else {
        out->x16=out->y16=0; out->z=out->flags=0; out->value=out->gen=0;
    }
}
#ifdef __cplusplus
extern "C" {
#endif
void pgxp_compiled_bitwise(uint32_t instr, uint32_t result, uint32_t s1, uint32_t s2);
#ifdef __cplusplus
}
#endif
PGXP_INLINE void psx_pgxp_alu_inline(struct CPUState *cpu, uint32_t instr,
                                     uint32_t result, uint32_t s1, uint32_t s2) {
    if (!PGXP_LIKELY(g_pgxp_cpu.active)) return;
    uint32_t op = instr >> 26, fn = instr & 63u;
    uint32_t rs = (instr >> 21) & 31u, rt = (instr >> 16) & 31u;
    uint32_t dst = op ? rt : ((instr >> 11) & 31u);
    if (op == 0 && (fn == 0x10 || fn == 0x11 || fn == 0x12 || fn == 0x13)) {
        psx_pgxp_alu(cpu, instr, result, s1, s2); return;
    }
    if (!dst) return;
    if (op == 0x0f) {
        pgxp_inline_reset_gpr(dst, result);
        g_pgxp_cpu.gpr[dst].y16 = (int32_t)(result & 0xffff0000u);
        g_pgxp_cpu.gpr[dst].flags = PGXP_F_VXY; return;
    }
    if ((op == 8 || op == 9) && !(instr & 0xffffu) && result == s1) {
        pgxp_inline_move(dst, rs, result); return;
    }
    if (!op && (fn == 0x25 || fn == 0x20 || fn == 0x21 || fn == 0x22 || fn == 0x23) &&
        (!rt || (!rs && fn != 0x22 && fn != 0x23)) && result == (rt ? s2 : s1)) {
        pgxp_inline_move(dst, rt ? rt : rs, result); return;
    }
    if ((!op && (fn == 0x2a || fn == 0x2b)) || op == 0x0a || op == 0x0b || op == 0x10) {
        pgxp_inline_reset_gpr(dst, result); return;
    }
    int shift = !op && (fn == 0 || fn == 2 || fn == 3 || fn == 4 || fn == 6 || fn == 7);
    if (shift) {
        uint32_t sh = fn < 4 ? ((instr >> 6) & 31u) : (s2 & 31u);
        if (!pgxp_inline_has_precision(rt) && sh != 16) {
            pgxp_inline_reset_gpr(dst, result); return;
        }
    } else if (!pgxp_inline_has_precision(rs) && (op || !pgxp_inline_has_precision(rt))) {
        if ((op == 8 || op == 9) || (!op && fn >= 0x20 && fn <= 0x23)) {
            pgxp_inline_reset_gpr(dst, result); return;
        }
        if ((op >= 0xc && op <= 0xe) || (!op && fn >= 0x24 && fn <= 0x27)) {
            pgxp_inline_reset_gpr(dst, result);
            PGXPValue *d = &g_pgxp_cpu.gpr[dst];
            if (!(result & 0xffffu)) d->flags |= PGXP_F_VX;
            if (!(result & 0xffff0000u)) d->flags |= PGXP_F_VY;
            if (op == 0xd && !(d->flags & PGXP_F_VX) && !(s1 & 0xffffu)) {
                d->x16 = (int32_t)((instr & 0xffffu) << 16); d->flags |= PGXP_F_VX;
            }
            return;
        }
    }
    if (shift) {
        const PGXPValue *source = rt ? &g_pgxp_cpu.gpr[rt] : NULL;
        double scalar;
        if (g_pgxp_cpu.cpu_mode && rt &&
            ((g_pgxp_cpu.scalar[rt].gen == g_pgxp_cpu.gen && g_pgxp_cpu.scalar[rt].value == s1) ||
             pgxp_inline_half_scalar(source, s1, &scalar))) {
            psx_pgxp_alu(cpu, instr, result, s1, s2); return;
        }
        /* Snapshot only consumed fields before reset: the register may alias.
         * This arithmetic resets precision but preserves the destination's
         * dormant projection payload, exactly like the reference engine. */
        uint32_t sh = fn < 4 ? ((instr >> 6) & 31u) : (s2 & 31u);
        int right = (fn & 3u) != 0;
        int tracked = pgxp_inline_comp_tracked(source, s1, right);
        int32_t component = source ? (right ? source->y16 : source->x16) : 0;
        uint16_t flags = source ? source->flags : 0;
        uint32_t gen = source ? source->gen : 0;
        pgxp_inline_reset_gpr(dst, result);
        PGXPValue *d = &g_pgxp_cpu.gpr[dst];
        if (!g_pgxp_cpu.cpu_mode || sh != 16 || gen != g_pgxp_cpu.gen) return;
        if (!right) {
            if (tracked) { d->y16 = component; d->flags |= PGXP_F_VY;
                if (flags & PGXP_F_DX) d->flags |= PGXP_F_DY; }
            d->flags |= PGXP_F_VX;
        } else {
            if (tracked) { d->x16 = component; d->flags |= PGXP_F_VX;
                if (flags & PGXP_F_DY) d->flags |= PGXP_F_DX; }
            d->y16 = (int32_t)(result & 0xffff0000u);
            d->flags |= PGXP_F_VY;
        }
        return;
    }
    if ((op >= 0xc && op <= 0xe) || (!op && fn >= 0x24 && fn <= 0x27)) {
        pgxp_compiled_bitwise(instr, result, s1, s2);
        return;
    }
    psx_pgxp_alu(cpu, instr, result, s1, s2);
}
#endif
