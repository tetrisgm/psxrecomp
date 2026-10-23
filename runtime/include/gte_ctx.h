#ifndef PSXRECOMP_GTE_CTX_H
#define PSXRECOMP_GTE_CTX_H

/* GTE for draw contexts (docs/THREADED_CONTEXTS.md, "GTE").
 *
 * The command math is the same code the main context runs (gte.cpp); a
 * PsxGteCtx gives one draw context its own copy of the state gte.cpp keeps
 * per guest context (projection caller, NCLIP provenance) and routes PGXP to
 * the context's engine. Host caches and statistics are main-context only.
 * One PsxGteCtx is used by one thread at a time. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;
struct PgxpCtxEngine;
typedef struct PsxGteCtx PsxGteCtx;

PsxGteCtx *gte_ctx_create(struct PgxpCtxEngine *pgxp);
void gte_ctx_destroy(PsxGteCtx *x);
/* Copy the main context's per-command GTE side state into `out`. Main thread. */
void gte_ctx_snapshot_main(PsxGteCtx *out);
/* Start a pass from a snapshot taken with gte_ctx_snapshot_main. */
void gte_ctx_begin(PsxGteCtx *x, const PsxGteCtx *snapshot);

/* gte_execute / gte_write_data / gte_precision_store_word for a context.
 * No completion latency is armed: draw contexts run no cycle model. */
void gte_ctx_execute(PsxGteCtx *x, struct CPUState *cpu, uint32_t cmd);
void gte_ctx_write_data(PsxGteCtx *x, struct CPUState *cpu, uint8_t reg, uint32_t val);
void gte_ctx_precision_store_word(PsxGteCtx *x, uint32_t addr, uint8_t reg);
/* gte_nclip_exact_sign against the context's provenance (no statistics). */
int32_t gte_ctx_nclip_exact_sign(PsxGteCtx *x, int32_t native_mac0, uint32_t pc);

#ifdef __cplusplus
}
#endif

#endif
