#ifndef PSXRECOMP_PGXP_CTX_H
#define PSXRECOMP_PGXP_CTX_H

/* Per-context PGXP engines (docs/THREADED_CONTEXTS.md, "PGXP").
 *
 * A draw context owns a private copy of the PGXP value-propagation engine:
 * the same code as the process-wide engine the main guest context uses
 * (runtime/src/pgxp_engine.inc, instantiated as class members by
 * pgxp_ctx.cpp), with its own RAM / scratchpad / register shadows, generation
 * and statistics. Nothing here touches the main engine, so any number of
 * engines may run on different threads at once; one engine is used by one
 * thread at a time.
 *
 * pgxp_ctx_begin() takes the main engine's configuration and register and
 * scratchpad and live RAM shadows from an owned snapshot
 * (pgxp_ctx_export_main(), main thread). Prior context generations are
 * invalidated before importing the captured words. The
 * position-cache fallback tier uses the associated private GTE cache. */

#include <stdint.h>

#include "pgxp.h"

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;
struct PsxGteCtx;
typedef struct PgxpCtxEngine PgxpCtxEngine;
typedef struct PgxpCtxExport PgxpCtxExport;
void pgxp_ctx_set_geometry(PgxpCtxEngine *engine, struct PsxGteCtx *geometry);

/* Immutable precision at command consumption, including homogeneous
 * projection for clipping. Later RAM reuse must not change a queued draw. */
typedef struct PgxpCtxVertex {
    int32_t x16, y16;
    uint16_t sz, source;
    uint32_t projection_valid;
    PGXPProjection projection;
    uint32_t raw_valid;
    int32_t raw_x16, raw_y16;
    uint16_t raw_sz;
    uint16_t reserved;
    float unclamped_depth;
} PgxpCtxVertex;
void pgxp_ctx_capture_vertex(PgxpCtxEngine *e, uint32_t addr, uint32_t word,
                              PgxpCtxVertex *out);

/* Main engine state for a snapshot. Main thread only. */
PgxpCtxExport *pgxp_ctx_export_main(void);
void pgxp_ctx_export_free(PgxpCtxExport *e);
int  pgxp_ctx_export_active(const PgxpCtxExport *e);

PgxpCtxEngine *pgxp_ctx_create(void);
void pgxp_ctx_destroy(PgxpCtxEngine *e);
/* Reset to `from` (NULL: disabled). Returns active (1), disabled (0), or -1
 * on allocation failure; callers must refuse the pass on failure. */
int  pgxp_ctx_begin(PgxpCtxEngine *e, const PgxpCtxExport *from);
int  pgxp_ctx_active(const PgxpCtxEngine *e);

/* Generated-code hooks (pgxp_hooks.h semantics). */
void pgxp_ctx_load  (PgxpCtxEngine *e, struct CPUState *cpu, uint32_t instr, uint32_t addr, uint32_t value);
void pgxp_ctx_store (PgxpCtxEngine *e, struct CPUState *cpu, uint32_t instr, uint32_t addr, uint32_t value);
void pgxp_ctx_alu   (PgxpCtxEngine *e, struct CPUState *cpu, uint32_t instr, uint32_t result, uint32_t s1, uint32_t s2);
void pgxp_ctx_muldiv(PgxpCtxEngine *e, struct CPUState *cpu, uint32_t instr, uint32_t hi, uint32_t lo, uint32_t s1, uint32_t s2);
void pgxp_ctx_cop2  (PgxpCtxEngine *e, struct CPUState *cpu, uint32_t instr, uint32_t value, uint32_t addr);

/* gte.cpp surface (pgxp.h semantics). */
int  pgxp_ctx_preserve_projection(const PgxpCtxEngine *e);
int  pgxp_ctx_projection_tracking(const PgxpCtxEngine *e);
int  pgxp_ctx_culling(const PgxpCtxEngine *e);
int  pgxp_ctx_project_precise(PgxpCtxEngine *e, int64_t mac1, int64_t mac2, int64_t mac3,
                              int shift, int32_t ir1, int32_t ir2, uint32_t sz3,
                              uint32_t h, int32_t ofx, int32_t ofy, int64_t x_num,
                              int64_t x_den, int32_t *x16, int32_t *y16);
int  pgxp_ctx_ppp_accept(PgxpCtxEngine *e, int32_t x16, int32_t y16, uint32_t packed);
void pgxp_ctx_gte_push_sxy(PgxpCtxEngine *e, int32_t x16, int32_t y16, uint16_t sz3, uint32_t packed);
void pgxp_ctx_gte_mark_audit(PgxpCtxEngine *e, uint32_t reasons);
void pgxp_ctx_gte_mark_unclamped(PgxpCtxEngine *e, uint32_t depth);
void pgxp_ctx_gte_set_projection(PgxpCtxEngine *e, const PGXPProjection *projection);
int  pgxp_ctx_get_gte_sxy_checked(PgxpCtxEngine *e, uint32_t index, uint32_t expect,
                                  int check, int32_t *x16, int32_t *y16);
int  pgxp_ctx_gte_nclip_precise(PgxpCtxEngine *e, const uint32_t sxy[3], int64_t *cross);
void pgxp_ctx_note_nclip(PgxpCtxEngine *e, int disagree, int corrected);
void pgxp_ctx_gte_reg_written(PgxpCtxEngine *e, int reg, uint32_t value);
void pgxp_ctx_gte_mvmva(PgxpCtxEngine *e, const PGXPMvmva *op);
void pgxp_ctx_gte_op_end(PgxpCtxEngine *e, uint32_t func);
void pgxp_ctx_store_gte_reg(PgxpCtxEngine *e, uint32_t addr, uint8_t reg);

/* Context memory writers that are not CPU stores (DMA, host pokes). */
void pgxp_ctx_invalidate_word(PgxpCtxEngine *e, uint32_t addr);

/* Consumer (pgxp_get_precise_vertex semantics) against this engine. */
int  pgxp_ctx_get_precise_vertex(PgxpCtxEngine *e, uint32_t addr, uint32_t packet_word,
                                 int32_t int_x, int32_t int_y,
                                 int32_t *x16, int32_t *y16, uint16_t *sz);
void pgxp_ctx_get_stats(const PgxpCtxEngine *e, PGXPStats *out);

#ifdef __cplusplus
}
#endif

#endif
