/* Immutable, ordered PGXP seam work for a discardable render transaction.
 * The caller must publish a frozen baseline and input stream before starting
 * preparation. It may run on any worker with its own PgxpSnapBatch. The render
 * owner must preflight the complete input stream and starting index before
 * using any result. A failed preflight runs the existing serial path; a
 * mismatch after preflight aborts the discardable transaction. */
#ifndef PGXP_SEAM_BATCH_H
#define PGXP_SEAM_BATCH_H

#include "pgxp_seam_snap.h"
#include <stdlib.h>

typedef struct {
    int32_t ix[3], iy[3], fx[3], fy[3];
    float z[3];
    uint32_t word[3];
    uint32_t reset_before;
} PgxpSnapBatchInput;

typedef struct {
    PgxpSnapBatchInput input;
    uint64_t checksum;
    size_t ordinal;
    float gap[3];
    PgxpSnapStrip strip[3];
    int count;
} PgxpSnapBatchEntry;

typedef struct {
    PgxpSnap *index;              /* private copy of the starting index */
    PgxpSnapBatchEntry *entries;
    size_t capacity, count, consumed;
    uint64_t epoch, baseline_checksum;
    int ready, verified;
} PgxpSnapBatch;

static inline uint64_t pgxp_snap_batch_bytes(uint64_t h, const void *bytes, size_t count) {
    const uint8_t *p = (const uint8_t *)bytes;
    for (size_t i = 0; i < count; i++) h = (h ^ p[i]) * UINT64_C(1099511628211);
    return h;
}

static inline uint64_t pgxp_snap_batch_input_checksum(const PgxpSnapBatchInput *in) {
    uint64_t h = UINT64_C(1469598103934665603);
    h = pgxp_snap_batch_bytes(h, in->ix, sizeof in->ix);
    h = pgxp_snap_batch_bytes(h, in->iy, sizeof in->iy);
    h = pgxp_snap_batch_bytes(h, in->fx, sizeof in->fx);
    h = pgxp_snap_batch_bytes(h, in->fy, sizeof in->fy);
    h = pgxp_snap_batch_bytes(h, in->z, sizeof in->z);
    h = pgxp_snap_batch_bytes(h, in->word, sizeof in->word);
    return pgxp_snap_batch_bytes(h, &in->reset_before, sizeof in->reset_before);
}

static inline int pgxp_snap_batch_input_equal(const PgxpSnapBatchInput *a,
                                               const PgxpSnapBatchInput *b) {
    return memcmp(a->ix, b->ix, sizeof a->ix) == 0 &&
           memcmp(a->iy, b->iy, sizeof a->iy) == 0 &&
           memcmp(a->fx, b->fx, sizeof a->fx) == 0 &&
           memcmp(a->fy, b->fy, sizeof a->fy) == 0 &&
           memcmp(a->z, b->z, sizeof a->z) == 0 &&
           memcmp(a->word, b->word, sizeof a->word) == 0 &&
           a->reset_before == b->reset_before;
}

/* Counters do not affect gap results. Hash the complete live index, including
 * link chains and stamps. Padding can only cause a safe false rejection. */
static inline uint64_t pgxp_snap_batch_index_checksum(const PgxpSnap *s) {
    uint64_t h = UINT64_C(1469598103934665603);
    if (!s || s->n_edges < 0 || s->n_edges > PGXP_SNAP_MAX_EDGES ||
        s->n_links < 0 || s->n_links > PGXP_SNAP_MAX_LINKS) return 0;
    h = pgxp_snap_batch_bytes(h, &s->n_edges, sizeof s->n_edges);
    h = pgxp_snap_batch_bytes(h, &s->n_links, sizeof s->n_links);
    h = pgxp_snap_batch_bytes(h, s->head, sizeof s->head);
    h = pgxp_snap_batch_bytes(h, s->links, (size_t)s->n_links * sizeof s->links[0]);
    h = pgxp_snap_batch_bytes(h, s->stamp, (size_t)s->n_edges * sizeof s->stamp[0]);
    h = pgxp_snap_batch_bytes(h, &s->stamp_gen, sizeof s->stamp_gen);
    return pgxp_snap_batch_bytes(h, s->edges, (size_t)s->n_edges * sizeof s->edges[0]);
}

static inline void pgxp_snap_batch_copy_index(PgxpSnap *dst, const PgxpSnap *src) {
    dst->n_edges = src->n_edges; dst->n_links = src->n_links;
    memcpy(dst->head, src->head, sizeof src->head);
    memcpy(dst->links, src->links, (size_t)src->n_links * sizeof src->links[0]);
    memcpy(dst->stamp, src->stamp, (size_t)src->n_edges * sizeof src->stamp[0]);
    dst->stamp_gen = src->stamp_gen;
    memcpy(dst->edges, src->edges, (size_t)src->n_edges * sizeof src->edges[0]);
    dst->tris = src->tris; dst->snapped_tris = src->snapped_tris;
    dst->snapped_edges = src->snapped_edges;
}

static inline void pgxp_snap_batch_release(PgxpSnapBatch *b) {
    if (!b) return;
    free(b->index); free(b->entries);
    memset(b, 0, sizeof *b);
}

static inline int pgxp_snap_batch_prepare(PgxpSnapBatch *b, const PgxpSnap *baseline,
                                          uint64_t epoch, const PgxpSnapBatchInput *input,
                                          size_t count) {
    if (!b) return 0;
    b->ready = b->verified = 0; b->count = b->consumed = 0;
    const uint64_t baseline_checksum = pgxp_snap_batch_index_checksum(baseline);
    if (!baseline || (!input && count) || !epoch || !baseline_checksum ||
        count > SIZE_MAX / sizeof *b->entries) return 0;
    if (!b->index) b->index = (PgxpSnap *)malloc(sizeof *b->index);
    if (!b->index) return 0;
    if (count > b->capacity) {
        PgxpSnapBatchEntry *next = (PgxpSnapBatchEntry *)realloc(b->entries, count * sizeof *next);
        if (!next) return 0;
        b->entries = next; b->capacity = count;
    }
    b->baseline_checksum = baseline_checksum;
    pgxp_snap_batch_copy_index(b->index, baseline);
    for (size_t i = 0; i < count; i++) {
        PgxpSnapBatchEntry *e = &b->entries[i];
        e->input = input[i]; e->ordinal = i;
        e->checksum = pgxp_snap_batch_input_checksum(&input[i]);
        if (input[i].reset_before || b->index->n_edges > PGXP_SNAP_MAX_EDGES - 3 ||
            b->index->n_links > PGXP_SNAP_MAX_LINKS - 4096)
            pgxp_snap_reset(b->index);
        e->count = pgxp_snap_triangle_ex(b->index, input[i].ix, input[i].iy,
                                         input[i].fx, input[i].fy, input[i].z,
                                         input[i].word, e->gap, e->strip);
    }
    b->epoch = epoch; b->count = count; b->ready = 1;
    return 1;
}

/* Preflight the entire batch before drawing. Inputs are checked bit-for-bit
 * after their checksum, so a collision cannot admit different geometry. */
static inline int pgxp_snap_batch_preflight(PgxpSnapBatch *b, const PgxpSnap *baseline,
                                            uint64_t epoch, const PgxpSnapBatchInput *input,
                                            size_t count) {
    if (!b) return 0;
    b->verified = 0; b->consumed = 0;
    if (!b->ready || !baseline || (!input && count) || b->epoch != epoch ||
        b->count != count ||
        b->baseline_checksum != pgxp_snap_batch_index_checksum(baseline)) return 0;
    for (size_t i = 0; i < count; i++) {
        const PgxpSnapBatchEntry *e = &b->entries[i];
        if (e->ordinal != i || e->checksum != pgxp_snap_batch_input_checksum(&input[i]) ||
            !pgxp_snap_batch_input_equal(&e->input, &input[i])) return 0;
    }
    b->verified = 1;
    return 1;
}

/* Sequential consumption enforces the same draw order. On any failure the
 * caller discards the optional transaction; never mix a prepared prefix with
 * a serial suffix against an index that the prepared prefix did not advance. */
static inline int pgxp_snap_batch_next_ex(PgxpSnapBatch *b, size_t ordinal,
                                          const PgxpSnapBatchInput *input,
                                          float gap[3], PgxpSnapStrip strip[3], int *count) {
    if (!b || !b->verified) return 0;
    if (ordinal != b->consumed || ordinal >= b->count || !input || !gap || !count) {
        b->verified = 0;
        return 0;
    }
    const PgxpSnapBatchEntry *e = &b->entries[ordinal];
    if (e->ordinal != ordinal || e->checksum != pgxp_snap_batch_input_checksum(input) ||
        !pgxp_snap_batch_input_equal(&e->input, input)) {
        b->verified = 0; return 0;
    }
    memcpy(gap, e->gap, sizeof e->gap);
    if (strip) memcpy(strip, e->strip, sizeof e->strip);
    *count = e->count; b->consumed++;
    return 1;
}
static inline int pgxp_snap_batch_next(PgxpSnapBatch *b, size_t ordinal,
                                       const PgxpSnapBatchInput *input,
                                       float gap[3], int *count) {
    return pgxp_snap_batch_next_ex(b, ordinal, input, gap, NULL, count);
}

#endif /* PGXP_SEAM_BATCH_H */
