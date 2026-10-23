/* Decode an immutable, ordered render-thread command stream into the exact
 * seam inputs. The caller maps RTH opcodes to these events and must reject
 * projected triangles, drawing-area changes, frame boundaries and unknown
 * stateful commands. No GL or mutable renderer state is read here. */
#ifndef PGXP_SEAM_RTH_STREAM_H
#define PGXP_SEAM_RTH_STREAM_H

#include "pgxp_seam_batch.h"

typedef enum {
    PGXP_RTH_IGNORE,
    PGXP_RTH_PRECISE,
    PGXP_RTH_DEPTH,
    PGXP_RTH_SEMI,
    PGXP_RTH_AREA,
    PGXP_RTH_FLAT_TRI,
    PGXP_RTH_GOURAUD_TRI,
    PGXP_RTH_TEX_TRI,
    PGXP_RTH_SHADED_TEX_TRI,
    PGXP_RTH_UNSUPPORTED
} PgxpRthSeamEvent;

typedef struct {
    size_t ordinal;
    int32_t area[4];
} PgxpRthSeamArea;

typedef struct {
    PgxpSnapBatchInput *input;
    size_t count, capacity;
    PgxpRthSeamArea *areas;
    size_t area_count, area_capacity;
    float z[3];
    int32_t precise[6];
    float depth_near;
    int precise_valid, depth_valid, semi_enabled;
    int depth_enabled, seam_enabled, valid;
} PgxpRthSeamStream;

static inline void pgxp_rth_seam_stream_init(PgxpRthSeamStream *s,
                                              int depth_enabled, int seam_enabled,
                                              float depth_near, int semi_enabled) {
    memset(s, 0, sizeof *s);
    s->depth_enabled = depth_enabled;
    s->seam_enabled = seam_enabled;
    s->depth_near = depth_near;
    s->semi_enabled = semi_enabled;
    s->valid = 1;
}

static inline void pgxp_rth_seam_stream_release(PgxpRthSeamStream *s) {
    free(s->input);
    free(s->areas);
    memset(s, 0, sizeof *s);
}

static inline int pgxp_rth_seam_stream_feed(PgxpRthSeamStream *s,
                                             PgxpRthSeamEvent event,
                                             const int32_t *v, size_t n) {
    if (!s || !s->valid) return 0;
    if (event == PGXP_RTH_UNSUPPORTED) return s->valid = 0;
    if (event == PGXP_RTH_IGNORE) return 1;
    if (!v) return s->valid = 0;
    if (event == PGXP_RTH_PRECISE) {
        if (n != 7) return s->valid = 0;
        s->precise_valid = v[0] != 0;
        memcpy(s->precise, v + 1, sizeof s->precise);
        return 1;
    }
    if (event == PGXP_RTH_DEPTH) {
        if (n != 4) return s->valid = 0;
        memcpy(s->z, v + 1, sizeof s->z);
        s->depth_valid = v[0] != 0 && s->z[0] > 0.0f &&
                         s->z[1] > 0.0f && s->z[2] > 0.0f;
        return 1;
    }
    if (event == PGXP_RTH_SEMI) {
        if (n != 2) return s->valid = 0;
        s->semi_enabled = v[0] != 0;
        return 1;
    }
    if (event == PGXP_RTH_AREA) {
        if (n != 4) return s->valid = 0;
        if (s->area_count == s->area_capacity) {
            if (s->area_capacity > SIZE_MAX / 2 / sizeof *s->areas)
                return s->valid = 0;
            size_t capacity = s->area_capacity ? s->area_capacity * 2 : 8;
            PgxpRthSeamArea *next = (PgxpRthSeamArea *)realloc(s->areas,
                                             capacity * sizeof *next);
            if (!next) return s->valid = 0;
            s->areas = next; s->area_capacity = capacity;
        }
        PgxpRthSeamArea *area = &s->areas[s->area_count++];
        area->ordinal = s->count;
        memcpy(area->area, v, sizeof area->area);
        return 1;
    }
    size_t x[3], y[3];
    if (event == PGXP_RTH_FLAT_TRI && n == 7) {
        x[0]=0; y[0]=1; x[1]=2; y[1]=3; x[2]=4; y[2]=5;
    } else if (event == PGXP_RTH_GOURAUD_TRI && n == 9) {
        x[0]=0; y[0]=1; x[1]=3; y[1]=4; x[2]=6; y[2]=7;
    } else if (event == PGXP_RTH_TEX_TRI && n == 15) {
        x[0]=0; y[0]=1; x[1]=4; y[1]=5; x[2]=8; y[2]=9;
    } else if (event == PGXP_RTH_SHADED_TEX_TRI && n == 19) {
        x[0]=0; y[0]=1; x[1]=5; y[1]=6; x[2]=10; y[2]=11;
    } else return s->valid = 0;

    if (s->depth_enabled && s->seam_enabled && !s->semi_enabled &&
        s->precise_valid && s->depth_valid &&
        fminf(s->z[0], fminf(s->z[1], s->z[2])) >= s->depth_near) {
        if (s->count == s->capacity) {
            if (s->capacity > SIZE_MAX / 2 / sizeof *s->input) return s->valid = 0;
            size_t next_capacity = s->capacity ? s->capacity * 2 : 64;
            PgxpSnapBatchInput *next = (PgxpSnapBatchInput *)realloc(s->input,
                                             next_capacity * sizeof *next);
            if (!next) return s->valid = 0;
            s->input = next; s->capacity = next_capacity;
        }
        PgxpSnapBatchInput *in = &s->input[s->count++];
        memset(in, 0, sizeof *in);
        for (int i = 0; i < 3; i++) {
            in->ix[i] = v[x[i]]; in->iy[i] = v[y[i]];
            /* Match glb_set_precise_triangle's float round trip, followed
             * by seam_snap_measure's lrintf conversion to 16.16. */
            in->fx[i] = (int32_t)lrintf(((float)s->precise[2*i] / 65536.0f) * 65536.0f);
            in->fy[i] = (int32_t)lrintf(((float)s->precise[2*i+1] / 65536.0f) * 65536.0f);
            in->z[i] = s->z[i];
        }
    }
    s->precise_valid = s->depth_valid = 0; /* one triangle only */
    return 1;
}

/* The reserve publishes the true initial drawing area later than command
 * recording. Resolve every reset against it before running the batch. */
static inline int pgxp_rth_seam_stream_mark_resets(PgxpRthSeamStream *s,
                                                    const int32_t initial_area[4]) {
    if (!s || !s->valid || !initial_area) return 0;
    int32_t current[4];
    memcpy(current, initial_area, sizeof current);
    size_t area_at = 0;
    int pending = 0;
    for (size_t i = 0; i < s->count; i++) {
        while (area_at < s->area_count && s->areas[area_at].ordinal <= i) {
            if (memcmp(current, s->areas[area_at].area, sizeof current) != 0) {
                pending = 1;
                memcpy(current, s->areas[area_at].area, sizeof current);
            }
            area_at++;
        }
        s->input[i].reset_before = (uint32_t)pending;
        pending = 0;
    }
    return 1;
}

/* RTH_GROUP keeps setup and its triangle in one publication. Decode each
 * logical record in order; the renderer supplies the private opcode map. */
static inline int pgxp_rth_seam_stream_feed_group(PgxpRthSeamStream *s,
        const int32_t *words, size_t count,
        PgxpRthSeamEvent (*map)(uint16_t op)) {
    if (!s || !words || !map) return 0;
    for (size_t at = 0; at < count;) {
        if (count - at < 2) return s->valid = 0;
        uint32_t id = (uint32_t)words[at];
        uint32_t n = (uint32_t)words[at + 1];
        if (n > count - at - 2) return s->valid = 0;
        if (!pgxp_rth_seam_stream_feed(s, map((uint16_t)id), words + at + 2, n))
            return 0;
        at += 2u + n;
    }
    return 1;
}

#endif
