/* pgxp_ctx.cpp - per-context PGXP engines (pgxp_ctx.h).
 *
 * The engine body (pgxp_engine.inc) is included here as the members of
 * PgxpCtxEngine, so a draw context runs exactly the code the main engine runs
 * against its own state. docs/THREADED_CONTEXTS.md, "PGXP". */

#include "pgxp.h"
#include "pgxp_ctx.h"
#include "pgxp_hooks.h"
#include "cpu_state.h"
#include "psx_memory.h"
#include "mod_memory.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" int gte_geometry_correction_lookup(uint32_t packed,
                                              int32_t *x16, int32_t *y16);
extern "C" int gte_geometry_correction_lookup_probe(uint32_t packed,
                                                    int32_t *x16, int32_t *y16);

#include "pgxp_engine_types.h"

extern "C" int pgxp_main_export(PgxpExport *out);

struct PgxpCtxEngine {
    uint32_t ram_mask = PSX_MAIN_RAM_RETAIL_BYTES - 1;
#define psx_ram_canonical_offset(address) ((address) & ram_mask)
#define PGXP_MEMBER_MODE 1
#define PGXP_S
#define PGXP_API
#include "pgxp_engine.inc"
#undef PGXP_S
#undef PGXP_API
#undef PGXP_MEMBER_MODE
#undef psx_ram_canonical_offset
};

struct PgxpCtxExport {
    PgxpExport state;
};

extern "C" PgxpCtxExport *pgxp_ctx_export_main(void) {
    PgxpCtxExport *e = new (std::nothrow) PgxpCtxExport();
    if (e && !pgxp_main_export(&e->state)) { delete e; return nullptr; }
    return e;
}

extern "C" void pgxp_ctx_export_free(PgxpCtxExport *e) {
    if (e) std::free(e->state.ram);
    delete e;
}

extern "C" int pgxp_ctx_export_active(const PgxpCtxExport *e) {
    return e && e->state.active;
}

extern "C" PgxpCtxEngine *pgxp_ctx_create(void) {
    return new (std::nothrow) PgxpCtxEngine();
}

extern "C" void pgxp_ctx_destroy(PgxpCtxEngine *e) {
    if (!e) return;
    std::free(e->s_ram);
    std::free(e->s_ck_bits);
    std::free(e->s_ck_log);
    delete e;
}

/* Re-stamp a main-engine shadow into this engine's generation: live there
 * (gen == main gen) means live here, anything else is dead. */
static inline void import_value(PGXPValue *dst, const PGXPValue *src,
                                uint32_t main_gen, uint32_t gen) {
    *dst = *src;
    dst->gen = src->gen == main_gen ? gen : 0u;
}

static inline void import_scalar(PGXPScalar *dst, const PGXPScalar *src,
                                 uint32_t main_gen, uint32_t gen) {
    *dst = *src;
    dst->gen = src->gen == main_gen ? gen : 0u;
}

extern "C" int pgxp_ctx_begin(PgxpCtxEngine *e, const PgxpCtxExport *from) {
    if (!e) return 0;
    const PgxpExport *m = from ? &from->state : nullptr;
    e->s_suppress = 0;
    e->s_deferred_invalidate = 0;
    e->s_ck_depth = 0;
    e->s_ck_lossy = 0;
    e->s_store_ring = nullptr;          /* no store ring in contexts */
    if (!m || !m->enabled) {
        e->s_enabled = 0;
        e->recompute_active();
        return 0;
    }
    if (!e->s_ram) {
        e->s_ram = (PGXPValue *)std::calloc(PGXP_SHADOW_WORDS, sizeof(PGXPValue));
        if (!e->s_ram) {
            e->s_enabled = 0;
            e->recompute_active();
            return -1;
        }
    }
    e->s_enabled = 1;
    e->s_cpu_mode = m->cpu_mode;
    e->s_projection_tracking = m->projection_tracking;
    e->s_position_fallback = 0;         /* the geometry cache is main-only */
    e->s_preserve_projection = m->preserve_projection;
    e->s_culling = m->culling;
    e->s_tolerance = m->tolerance;
    e->ram_mask = m->ram_mask;
    /* O(1): every RAM shadow from an earlier pass is dead now. */
    e->pgxp_invalidate_all();
    const uint32_t gen = e->s_gen;
    for (uint32_t i = 0; i < m->ram_count; ++i)
        import_value(&e->s_ram[m->ram[i].index], &m->ram[i].value, m->gen, gen);
    for (unsigned i = 0; i < PGXP_SCRATCH_WORDS; ++i)
        import_value(&e->s_scratch[i], &m->scratch[i], m->gen, gen);
    for (unsigned i = 0; i < 34; ++i) {
        import_value(&e->s_gpr[i], &m->gpr[i], m->gen, gen);
        import_scalar(&e->s_gpr_s[i], &m->gpr_s[i], m->gen, gen);
    }
    for (unsigned i = 0; i < 32; ++i) {
        import_value(&e->s_gte[i], &m->gte[i], m->gen, gen);
        import_value(&e->s_gtc[i], &m->gtc[i], m->gen, gen);
        import_scalar(&e->s_gte_s[i], &m->gte_s[i], m->gen, gen);
        import_scalar(&e->s_gtc_s[i], &m->gtc_s[i], m->gen, gen);
    }
    std::memset(&e->s_stats, 0, sizeof e->s_stats);
    e->recompute_active();
    return e->g_pgxp_active;
}

extern "C" int pgxp_ctx_active(const PgxpCtxEngine *e) {
    return e && e->g_pgxp_active;
}

extern "C" void pgxp_ctx_capture_vertex(PgxpCtxEngine *e, uint32_t addr,
                                         uint32_t word, PgxpCtxVertex *out) {
    std::memset(out, 0, sizeof *out);
    const int32_t ix = (int32_t)((word & 0x7ffu) ^ 0x400u) - 0x400;
    const int32_t iy = (int32_t)(((word >> 16) & 0x7ffu) ^ 0x400u) - 0x400;
    out->x16 = ix * 65536;
    out->y16 = iy * 65536;
    if (!e || !e->s_enabled || addr == 0xFFFFFFFFu) return;
    out->source = (uint16_t)e->pgxp_get_precise_vertex(addr, word, ix, iy,
                                                       &out->x16, &out->y16, &out->sz);
    out->raw_valid = e->pgxp_load_precise_word(addr, word,
                                               &out->raw_x16, &out->raw_y16, &out->raw_sz);
    out->unclamped_depth = e->pgxp_get_unclamped_depth(addr, word);
    const PGXPValue *pv = e->pgxp_ptr(addr);
    if (pv && pv->gen == e->s_gen && pv->value == word &&
        (pv->flags & PGXP_F_PROJECTION)) {
        out->projection_valid = 1;
        out->projection = pv->projection;
    }
}

extern "C" void pgxp_ctx_load(PgxpCtxEngine *e, CPUState *cpu, uint32_t instr,
                              uint32_t addr, uint32_t value) {
    e->psx_pgxp_load(cpu, instr, addr, value);
}
extern "C" void pgxp_ctx_store(PgxpCtxEngine *e, CPUState *cpu, uint32_t instr,
                               uint32_t addr, uint32_t value) {
    e->psx_pgxp_store(cpu, instr, addr, value);
}
extern "C" void pgxp_ctx_alu(PgxpCtxEngine *e, CPUState *cpu, uint32_t instr,
                             uint32_t result, uint32_t s1, uint32_t s2) {
    e->psx_pgxp_alu(cpu, instr, result, s1, s2);
}
extern "C" void pgxp_ctx_muldiv(PgxpCtxEngine *e, CPUState *cpu, uint32_t instr,
                                uint32_t hi, uint32_t lo, uint32_t s1, uint32_t s2) {
    e->psx_pgxp_muldiv(cpu, instr, hi, lo, s1, s2);
}
extern "C" void pgxp_ctx_cop2(PgxpCtxEngine *e, CPUState *cpu, uint32_t instr,
                              uint32_t value, uint32_t addr) {
    e->psx_pgxp_cop2(cpu, instr, value, addr);
}

extern "C" int pgxp_ctx_preserve_projection(const PgxpCtxEngine *e) {
    return e->s_preserve_projection;
}
extern "C" int pgxp_ctx_projection_tracking(const PgxpCtxEngine *e) {
    return e->s_projection_tracking;
}
extern "C" int pgxp_ctx_culling(const PgxpCtxEngine *e) { return e->s_culling; }

extern "C" int pgxp_ctx_project_precise(PgxpCtxEngine *e, int64_t mac1, int64_t mac2,
                                        int64_t mac3, int shift, int32_t ir1,
                                        int32_t ir2, uint32_t sz3, uint32_t h,
                                        int32_t ofx, int32_t ofy, int64_t x_num,
                                        int64_t x_den, int32_t *x16, int32_t *y16) {
    return e->pgxp_project_precise(mac1, mac2, mac3, shift, ir1, ir2, sz3, h,
                                   ofx, ofy, x_num, x_den, x16, y16);
}
extern "C" int pgxp_ctx_ppp_accept(PgxpCtxEngine *e, int32_t x16, int32_t y16,
                                   uint32_t packed) {
    return e->pgxp_ppp_accept(x16, y16, packed);
}
extern "C" void pgxp_ctx_gte_push_sxy(PgxpCtxEngine *e, int32_t x16, int32_t y16,
                                      uint16_t sz3, uint32_t packed) {
    e->pgxp_gte_push_sxy(x16, y16, sz3, packed);
}
extern "C" void pgxp_ctx_gte_mark_unclamped(PgxpCtxEngine *e, uint32_t depth) {
    e->pgxp_gte_mark_unclamped(depth);
}
extern "C" void pgxp_ctx_gte_set_projection(PgxpCtxEngine *e,
                                            const PGXPProjection *projection) {
    e->pgxp_gte_set_projection(projection);
}
extern "C" int pgxp_ctx_get_gte_sxy_checked(PgxpCtxEngine *e, uint32_t index,
                                            uint32_t expect, int check,
                                            int32_t *x16, int32_t *y16) {
    return e->pgxp_get_gte_sxy_checked(index, expect, check, x16, y16);
}
extern "C" int pgxp_ctx_gte_nclip_precise(PgxpCtxEngine *e, const uint32_t sxy[3],
                                          int64_t *cross) {
    return e->pgxp_gte_nclip_precise(sxy, cross);
}
extern "C" void pgxp_ctx_note_nclip(PgxpCtxEngine *e, int disagree, int corrected) {
    e->pgxp_note_nclip(disagree, corrected);
}
extern "C" void pgxp_ctx_gte_reg_written(PgxpCtxEngine *e, int reg, uint32_t value) {
    e->pgxp_gte_reg_written(reg, value);
}
extern "C" void pgxp_ctx_gte_mvmva(PgxpCtxEngine *e, const PGXPMvmva *op) {
    e->pgxp_gte_mvmva(op);
}
extern "C" void pgxp_ctx_gte_op_end(PgxpCtxEngine *e, uint32_t func) {
    e->pgxp_gte_op_end(func);
}
extern "C" void pgxp_ctx_store_gte_reg(PgxpCtxEngine *e, uint32_t addr, uint8_t reg) {
    e->pgxp_store_gte_reg(addr, reg);
}
extern "C" void pgxp_ctx_invalidate_word(PgxpCtxEngine *e, uint32_t addr) {
    e->pgxp_invalidate_word(addr);
}
extern "C" int pgxp_ctx_get_precise_vertex(PgxpCtxEngine *e, uint32_t addr,
                                           uint32_t packet_word, int32_t int_x,
                                           int32_t int_y, int32_t *x16,
                                           int32_t *y16, uint16_t *sz) {
    return e->pgxp_get_precise_vertex(addr, packet_word, int_x, int_y, x16, y16, sz);
}
extern "C" void pgxp_ctx_get_stats(const PgxpCtxEngine *e, PGXPStats *out) {
    if (out) *out = e->s_stats;
}
