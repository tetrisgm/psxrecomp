/* PGXP seam snapping (docs/ENHANCEMENTS.md G1.15).
 *
 * Two neighbouring surfaces whose edges the PS1 drew on the SAME integer
 * line (a crest's consecutive road rows, block boundaries, T-junctions of
 * subdivided meshes) can sit up to about a native pixel apart once PGXP
 * gives every vertex its sub-pixel position: the integer snap that closed
 * them on hardware is gone, and at a high internal scale the gap is many
 * output pixels wide (whatever was drawn behind shows through as thin
 * stripes). Seam expansion (1 output px on every edge) cannot cover that
 * without bloating every silhouette.
 *
 * This finds those gaps per frame, in draw order, and measures them: a PGXP
 * 3D triangle's edge that lies on an earlier triangle's edge in integer
 * space (collinear and overlapping), or a vertex on one (a T-junction),
 * with the two surfaces on opposite sides, parallel in precise space and at
 * a consistent depth. The result is the gap per edge (native px, at most
 * PGXP_SNAP_MAX_PX, the integer rounding a PS1 hid it in). The renderer
 * fills it with a strip from that edge outward, its attributes extrapolated
 * from the triangle (textures continue, nothing slides) and its depth pushed
 * behind the triangle's plane, so the strip only shows where the scenery
 * behind would show through the gap and never covers the neighbour or a
 * decal on it. Vertices never move (every position stays exactly PGXP's)
 * and an edge with no neighbour (a silhouette) gets no strip.
 *
 * Host-side and visual only: guest state is untouched. Header-only (static)
 * so the unit test (tests/test_pgxp_seam_snap.c) drives the exact code the
 * GL renderer uses. */
#ifndef PGXP_SEAM_SNAP_H
#define PGXP_SEAM_SNAP_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define PGXP_SNAP_CELL        8          /* index cell length, native px    */
#define PGXP_SNAP_BUCKETS     16384u     /* power of two                    */
#define PGXP_SNAP_MAX_EDGES   24576
#define PGXP_SNAP_MAX_LINKS   196608
#define PGXP_SNAP_MAX_PX      1.0f       /* largest gap closed, native px   */
#define PGXP_SNAP_LINE_TOL    0.75f      /* integer collinearity, px        */
#define PGXP_SNAP_ON_EDGE_TOL 0.5f       /* integer vertex-on-edge, px      */
#define PGXP_SNAP_EXTEND_PX   3.0f       /* vertex rule: past an edge's end */
#define PGXP_SNAP_PARALLEL    0.05f      /* sin of the largest angle (~3 deg) */
#define PGXP_SNAP_DEPTH_TOL   0.12f      /* relative 1/z agreement          */
#define PGXP_SNAP_OVERLAY_PX  0.125f     /* on an earlier edge: a decal     */

typedef struct {
    float ax, ay, bx, by;        /* precise endpoints (native px)            */
    float iax, iay, ibx, iby;    /* integer endpoints                        */
    float iza, izb;              /* 1/z at the endpoints                     */
    float cx, cy;                /* precise centroid of the owning triangle  */
    float ux, uy, il;            /* integer unit direction, integer length   */
    float nx, ny;                /* unit normal of the precise line, pointing
                                  * away from the owning triangle            */
    uint32_t ka, kb;             /* vertex keys (shared vertices never pair) */
} PgxpSnapEdge;

typedef struct {
    float ax, ay, ux, uy;        /* the edge's integer line                  */
    int32_t next, edge;          /* chain, edge index                        */
    uint32_t ka, kb;             /* the edge's vertex keys                   */
} PgxpSnapLink;

typedef struct {
    int n_edges, n_links;
    int32_t head[PGXP_SNAP_BUCKETS];
    /* Each link carries its edge's integer line (origin, unit direction), so
     * a query rejects non-collinear candidates from the link alone. */
    PgxpSnapLink links[PGXP_SNAP_MAX_LINKS];
    uint32_t stamp[PGXP_SNAP_MAX_EDGES];  /* (triangle generation << 3) | vertices */
    uint32_t stamp_gen;
    PgxpSnapEdge edges[PGXP_SNAP_MAX_EDGES];
    /* Cumulative counters (debug server). */
    uint64_t tris, snapped_tris, snapped_edges;
} PgxpSnap;

/* Start a new frame: forget every edge (counters keep accumulating). Call
 * once before the first triangle. */
static inline void pgxp_snap_reset(PgxpSnap *s) {
    s->n_edges = 0;
    s->n_links = 0;
    for (unsigned i = 0; i < PGXP_SNAP_BUCKETS; i++) s->head[i] = -1;
}

/* The index keys a line, not an area: a mostly horizontal edge (family 0)
 * lives in cells of PGXP_SNAP_CELL px along x and ONE integer row in y (a
 * mostly vertical one the other way round), so a cell holds only edges
 * that run along that row, and a query finds collinear edges among a few
 * candidates however much geometry overlaps the spot. */
/* floor to int without a libm call (the index is hot). */
static inline int pgxp_snap_ifloor(float v) {
    const int i = (int)v;
    return i - (v < (float)i);
}

static inline uint32_t pgxp_snap_cell_hash(int fam, int major, int minor) {
    uint32_t h = (uint32_t)major * 73856093u ^ (uint32_t)minor * 19349663u ^ (uint32_t)fam * 83492791u;
    return (h ^ (h >> 13)) & (PGXP_SNAP_BUCKETS - 1u);
}

static inline int pgxp_snap_family(float dx, float dy) {
    return fabsf(dy) > fabsf(dx);
}

static inline float pgxp_snap_cross(float ax, float ay, float bx, float by) {
    return ax * by - ay * bx;
}

static inline void pgxp_snap_add_edge(PgxpSnap *s, const PgxpSnapEdge *e) {
    if (s->n_edges >= PGXP_SNAP_MAX_EDGES) return;
    {   /* An edge shorter than 2 integer px never pairs: keep chains short. */
        const float dx = e->ibx - e->iax, dy = e->iby - e->iay;
        if (dx * dx + dy * dy < 4.0f) return;
    }
    const int idx = s->n_edges++;
    s->edges[idx] = *e;
    s->stamp[idx] = 0;
    PgxpSnapEdge *d = &s->edges[idx];
    {
        const float dx = d->ibx - d->iax, dy = d->iby - d->iay;
        d->il = sqrtf(dx * dx + dy * dy);
        d->ux = dx / d->il;
        d->uy = dy / d->il;
        const float ex = d->bx - d->ax, ey = d->by - d->ay;
        const float l = sqrtf(ex * ex + ey * ey);
        d->nx = l > 1e-6f ? -ey / l : 0.0f;    /* the cross > 0 side */
        d->ny = l > 1e-6f ? ex / l : 0.0f;
        if (pgxp_snap_cross(ex, ey, d->cx - d->ax, d->cy - d->ay) > 0.0f) { d->nx = -d->nx; d->ny = -d->ny; }
    }
    /* Every (cell, row) holding an integer point a query can match: walked
     * cell by cell along the major axis over the segment and PGXP_SNAP_
     * EXTEND_PX past each end, the rows within PGXP_SNAP_LINE_TOL of the line
     * inside that cell. A query then looks in its own (cell, row) alone. */
    const int fam = pgxp_snap_family(e->ibx - e->iax, e->iby - e->iay);
    float ma0 = fam ? e->iay : e->iax, mi0 = fam ? e->iax : e->iay;
    float ma1 = fam ? e->iby : e->ibx, mi1 = fam ? e->ibx : e->iby;
    if (ma1 < ma0) { float t = ma0; ma0 = ma1; ma1 = t; t = mi0; mi0 = mi1; mi1 = t; }
    const float slope = ma1 > ma0 ? (mi1 - mi0) / (ma1 - ma0) : 0.0f;
    const float band = PGXP_SNAP_LINE_TOL * sqrtf(1.0f + slope * slope);
    const float lo = ma0 - PGXP_SNAP_EXTEND_PX, hi = ma1 + PGXP_SNAP_EXTEND_PX;
    const int c0 = pgxp_snap_ifloor(lo * (1.0f / PGXP_SNAP_CELL));
    const int c1 = pgxp_snap_ifloor(hi * (1.0f / PGXP_SNAP_CELL));
    for (int c = c0; c <= c1; c++) {
        const float a = fmaxf(lo, (float)(c * PGXP_SNAP_CELL));
        const float b = fminf(hi, (float)(c * PGXP_SNAP_CELL + PGXP_SNAP_CELL - 1));
        const float ya = mi0 + (a - ma0) * slope, yb = mi0 + (b - ma0) * slope;
        const int r0 = -pgxp_snap_ifloor(-(fminf(ya, yb) - band));      /* ceil */
        const int r1 = pgxp_snap_ifloor(fmaxf(ya, yb) + band);
        for (int r = r0; r <= r1; r++) {
            if (s->n_links >= PGXP_SNAP_MAX_LINKS) return;
            const uint32_t cell = pgxp_snap_cell_hash(fam, c, r);
            PgxpSnapLink *k = &s->links[s->n_links];
            k->ax = d->iax; k->ay = d->iay; k->ux = d->ux; k->uy = d->uy;
            k->edge = idx;
            k->ka = d->ka; k->kb = d->kb;
            k->next = s->head[cell];
            s->head[cell] = s->n_links++;
        }
    }
}

/* Identifies one vertex (packet word + PGXP position): edges of the same
 * mesh that share it are already consistent and never pair. */
static inline uint32_t pgxp_snap_vkey(uint32_t word, int32_t x16, int32_t y16) {
    uint32_t h = word * 2654435761u ^ (uint32_t)x16 * 2246822519u ^ (uint32_t)y16 * 3266489917u;
    return h ^ (h >> 15);
}

static inline int pgxp_snap_depth_ok(float iz, float iz_ref) {
    if (!(iz > 0.0f) || !(iz_ref > 0.0f)) return 0;
    return fabsf(iz / iz_ref - 1.0f) <= PGXP_SNAP_DEPTH_TOL;
}


/* Gap from point (x,y) to an edge's precise line, measured away from the
 * edge's own triangle: > 0 when the point lies on the far side. */
static inline float pgxp_snap_gap(const PgxpSnapEdge *f, float x, float y) {
    return (x - f->ax) * f->nx + (y - f->ay) * f->ny;
}

/* One PGXP 3D triangle in draw order. ix/iy: integer vertex positions (the
 * hardware's, draw offset included); fx/fy: PGXP 16.16 positions in the
 * same frame; z: per-vertex depth (> 0); word: packet words. Writes e[k],
 * the outward offset (native px, 0 = none) edge k (vertex k -> k+1) needs
 * to close its gap, records the triangle for the ones after it, and returns
 * the number of edges with a gap. word may be 0 (positions alone identify
 * a shared vertex then). */
#define PGXP_SNAP_CANDS 48
static inline int pgxp_snap_triangle(PgxpSnap *s, const int32_t ix[3], const int32_t iy[3],
                                     const int32_t fx[3], const int32_t fy[3], const float z[3],
                                     const uint32_t word[3], float e[3]) {
    s->tris++;
    float px[3], py[3], iz[3], on[3][2] = {{0, 0}, {0, 0}, {0, 0}};
#ifndef PSX_PGXP_SNAP_EAGER_NORMALS
    unsigned char normal_done[3] = {0, 0, 0};
#endif
    uint32_t key[3];
    int offgrid[3];
    e[0] = e[1] = e[2] = 0.0f;
    for (int i = 0; i < 3; i++) {
        key[i] = pgxp_snap_vkey(word[i], fx[i], fy[i]);
        px[i] = (float)fx[i] / 65536.0f;
        py[i] = (float)fy[i] / 65536.0f;
        iz[i] = 1.0f / z[i];
        /* A vertex whose precise position is not within the integer rounding
         * of its packet word (native-wide recovery, an unclamped far
         * projection) says nothing about integer adjacency. */
        offgrid[i] = fabsf(px[i] - (float)ix[i]) > 1.5f || fabsf(py[i] - (float)iy[i]) > 1.5f;
    }
    const float area = pgxp_snap_cross(px[1] - px[0], py[1] - py[0], px[2] - px[0], py[2] - py[0]);
    const int usable = fabsf(area) > 1e-4f && s->n_edges > 0;
#ifdef PSX_PGXP_SNAP_EAGER_NORMALS
    for (int k = 0; k < 3 && usable; k++) {            /* old eager path for comparison */
        const int b = (k + 1) % 3;
        const float ex = px[b] - px[k], ey = py[b] - py[k], l = sqrtf(ex * ex + ey * ey);
        const float sg = area > 0.0f ? 1.0f : -1.0f;
        if (l > 1e-6f) { on[k][0] = sg * ey / l; on[k][1] = -sg * ex / l; }
    }
#endif

    /* Gather, once per vertex: the earlier edges whose integer line passes
     * within PGXP_SNAP_LINE_TOL of it (both index families, the vertex's row
     * and its neighbours). stamp[] marks which of this triangle's vertices
     * found each edge, so an edge both ends of one of ours lie on is known
     * without a second search. */
    int cand[3][PGXP_SNAP_CANDS], ncand[3] = {0, 0, 0};
    float cdv[3][PGXP_SNAP_CANDS];
    if (usable) {
        if (++s->stamp_gen >= (1u << 29)) { memset(s->stamp, 0, sizeof s->stamp); s->stamp_gen = 1; }
        const uint32_t g = s->stamp_gen << 3;
        for (int v = 0; v < 3; v++) {
            if (offgrid[v]) continue;
            const float vx = (float)ix[v], vy = (float)iy[v];
            for (int fam = 0; fam < 2; fam++) {
                const int ma = pgxp_snap_ifloor((fam ? vy : vx) * (1.0f / PGXP_SNAP_CELL));
                const int mi = fam ? ix[v] : iy[v];
                {
                    for (int l = s->head[pgxp_snap_cell_hash(fam, ma, mi)]; l >= 0; l = s->links[l].next) {
                        const PgxpSnapLink *ln = &s->links[l];
                        const float dv = fabsf(pgxp_snap_cross(ln->ux, ln->uy, vx - ln->ax, vy - ln->ay));
                        if (dv > PGXP_SNAP_LINE_TOL) continue;
                        /* An edge of this vertex's own mesh (it shares the
                         * vertex) never pairs with it. */
                        if (ln->ka == key[v] || ln->kb == key[v]) continue;
                        const int ei = ln->edge;
                        uint32_t m = s->stamp[ei];
                        if ((m & ~7u) != g) m = g;
                        if (m & (1u << v)) continue;
                        s->stamp[ei] = m | (1u << v);
                        if (ncand[v] < PGXP_SNAP_CANDS) {
                            cand[v][ncand[v]] = ei;
                            cdv[v][ncand[v]] = dv;
                            ncand[v]++;
                        }
                    }
                }
            }
        }
    }

    /* Edge rule: an earlier edge both ends of this one lie on (integer),
     * overlapping it, across it from this triangle, parallel to it in
     * precise space and at a consistent depth: the gap between them is this
     * edge's. One that lies along most of this edge on this triangle's own
     * side, in precise space too, means this triangle covers a surface
     * already drawn (a decal: lane marks, painted kerbs, shadows): its gaps
     * are the surface's to fill (with the surface's texture, not the
     * decal's), so it gets none. */
    int overlay = 0;
    for (int k = 0; k < 3 && usable && !overlay; k++) {
        const int a = k, b = (k + 1) % 3, o = (k + 2) % 3;
        if (offgrid[a] || offgrid[b]) continue;
        const float ex = px[b] - px[a], ey = py[b] - py[a];
        const float epl = sqrtf(ex * ex + ey * ey);
        const float eix = (float)(ix[b] - ix[a]), eiy = (float)(iy[b] - iy[a]);
        const float el = sqrtf(eix * eix + eiy * eiy);
        if (el < 2.0f || epl < 1e-4f) continue;
        float best = 0.0f;
        /* Candidates either end found; the other end is tested on the
         * candidate's line directly when its own gather did not reach it
         * (the candidate ends short of it). */
        for (int c = 0; c < ncand[a] + ncand[b]; c++) {
            const int from_b = c >= ncand[a];
            const int ei = from_b ? cand[b][c - ncand[a]] : cand[a][c];
            const uint32_t m = s->stamp[ei];
            if (from_b && (m & (1u << a))) continue;          /* seen from a */
            const PgxpSnapEdge *f = &s->edges[ei];
            if (f->ka == key[a] || f->kb == key[a] || f->ka == key[b] || f->kb == key[b]) continue;
            const int w = from_b ? a : b;
            if (!(m & (1u << w)) &&
                fabsf(pgxp_snap_cross(f->ux, f->uy, (float)ix[w] - f->iax, (float)iy[w] - f->iay)) > PGXP_SNAP_LINE_TOL)
                continue;
            const float fdx = f->ibx - f->iax, fdy = f->iby - f->iay;
            const float fl = f->il, fl2 = fl * fl;
            const float ta = (((float)ix[a] - f->iax) * fdx + ((float)iy[a] - f->iay) * fdy) / fl2;
            const float tb = (((float)ix[b] - f->iax) * fdx + ((float)iy[b] - f->iay) * fdy) / fl2;
            const float lo = fmaxf(fminf(ta, tb), 0.0f), hi = fminf(fmaxf(ta, tb), 1.0f);
            if ((hi - lo) * fl < 1.0f) continue;
            const float fex = f->bx - f->ax, fey = f->by - f->ay;
            const float fpl = sqrtf(fex * fex + fey * fey);
            if (fpl < 1e-4f) continue;
            /* The same line in precise space too. */
            if (fabsf(pgxp_snap_cross(fex, fey, ex, ey)) > PGXP_SNAP_PARALLEL * fpl * epl) continue;
            const float tca = fminf(fmaxf(ta, 0.0f), 1.0f), tcb = fminf(fmaxf(tb, 0.0f), 1.0f);
            if (!pgxp_snap_depth_ok(iz[a], f->iza + (f->izb - f->iza) * tca) ||
                !pgxp_snap_depth_ok(iz[b], f->iza + (f->izb - f->iza) * tcb)) continue;
            /* Opposite sides of the shared line, each judged from its own
             * edge (a sliver's far edge, whose triangle lies on the owner's
             * side, never pairs). */
            const float sf = pgxp_snap_cross(fex, fey, f->cx - f->ax, f->cy - f->ay);
            const float sn = pgxp_snap_cross(ex, ey, px[o] - px[a], py[o] - py[a]);
            const float par = fex * ex + fey * ey;            /* same direction: + */
            const float d = fmaxf(pgxp_snap_gap(f, px[a], py[a]), pgxp_snap_gap(f, px[b], py[b]));
            if (!(sf * sn * par < 0.0f)) {                    /* same side */
                if (d <= PGXP_SNAP_OVERLAY_PX && (hi - lo) * fl >= 0.75f * el) { overlay = 1; break; }
                continue;
            }
            if (d > PGXP_SNAP_MAX_PX) continue;
            if (d > best) best = d;
        }
        if (best > e[k]) e[k] = best;
    }
    if (overlay) e[0] = e[1] = e[2] = 0.0f;

    /* Vertex rule: a vertex on (or just past the end of) an earlier edge in
     * integer space with a gap in precise space (a T-junction). The two
     * edges at the vertex grow so the corner reaches the line. */
    for (int v = 0; v < 3 && usable && !overlay; v++) {
        if (offgrid[v]) continue;
        float best = 0.0f, bnx = 0.0f, bny = 0.0f;
        for (int c = 0; c < ncand[v]; c++) {
            if (cdv[v][c] > PGXP_SNAP_ON_EDGE_TOL) continue;
            const PgxpSnapEdge *f = &s->edges[cand[v][c]];
            if (f->ka == key[v] || f->kb == key[v]) continue;
            const float fdx = f->ibx - f->iax, fdy = f->iby - f->iay;
            const float fl = f->il, fl2 = fl * fl;
            const float t = (((float)ix[v] - f->iax) * fdx + ((float)iy[v] - f->iay) * fdy) / fl2;
            /* Interior, or just past an end along the same line (the next
             * piece of a row that overhangs its neighbour). */
            if (t * fl < -PGXP_SNAP_EXTEND_PX || (1.0f - t) * fl < -PGXP_SNAP_EXTEND_PX) continue;
            if (fabsf(t * fl) < 1.0f || fabsf((1.0f - t) * fl) < 1.0f) continue;
            const float gap = pgxp_snap_gap(f, px[v], py[v]);
            if (gap <= 0.0f || gap > PGXP_SNAP_MAX_PX || gap <= best) continue;
            /* The rest of this triangle lies on the far side (on the line
             * counts) and not all of it on the line: the T's stem, not an
             * overlap with the owner or a sliver along its edge. */
            const int o1 = (v + 1) % 3, o2 = (v + 2) % 3;
            const float g1 = pgxp_snap_gap(f, px[o1], py[o1]), g2 = pgxp_snap_gap(f, px[o2], py[o2]);
            if (g1 < -0.25f || g2 < -0.25f || fmaxf(g1, g2) < 0.25f) continue;
            const float tc = fminf(fmaxf(t, 0.0f), 1.0f);
            if (!pgxp_snap_depth_ok(iz[v], f->iza + (f->izb - f->iza) * tc)) continue;
            best = gap; bnx = -f->nx; bny = -f->ny;          /* toward the line */
        }
        if (best <= 0.0f) continue;
        const int kp = (v + 2) % 3;          /* edges kp (into v) and v (out) */
#ifndef PSX_PGXP_SNAP_EAGER_NORMALS
        /* Most triangles never reach this rule. Only normalize an edge when
         * a qualifying T-junction actually needs its outward direction. */
        const int needed[2] = {kp, v};
        for (int j = 0; j < 2; j++) {
            const int k = needed[j];
            if (normal_done[k]) continue;
            const int b = (k + 1) % 3;
            const float ex = px[b] - px[k], ey = py[b] - py[k];
            const float l = sqrtf(ex * ex + ey * ey);
            const float sg = area > 0.0f ? 1.0f : -1.0f;
            if (l > 1e-6f) { on[k][0] = sg * ey / l; on[k][1] = -sg * ex / l; }
            normal_done[k] = 1;
        }
#endif
        const float ep = best * (on[kp][0] * bnx + on[kp][1] * bny);
        const float en = best * (on[v][0] * bnx + on[v][1] * bny);
        if (ep > e[kp]) e[kp] = fminf(ep, PGXP_SNAP_MAX_PX);
        if (en > e[v]) e[v] = fminf(en, PGXP_SNAP_MAX_PX);
    }

    /* Index this triangle's own (PGXP) edges for the triangles after it. */
    const float cx = (px[0] + px[1] + px[2]) * (1.0f / 3.0f);
    const float cy = (py[0] + py[1] + py[2]) * (1.0f / 3.0f);
    for (int k = 0; k < 3 && fabsf(area) > 1e-4f; k++) {
        const int a = k, b = (k + 1) % 3;
        if (offgrid[a] || offgrid[b]) continue;
        PgxpSnapEdge ed;
        ed.ax = px[a]; ed.ay = py[a]; ed.bx = px[b]; ed.by = py[b];
        ed.iax = (float)ix[a]; ed.iay = (float)iy[a];
        ed.ibx = (float)ix[b]; ed.iby = (float)iy[b];
        ed.iza = iz[a]; ed.izb = iz[b];
        ed.cx = cx; ed.cy = cy;
        ed.ka = key[a]; ed.kb = key[b];
        pgxp_snap_add_edge(s, &ed);
    }
    const int n = (e[0] > 0.0f) + (e[1] > 0.0f) + (e[2] > 0.0f);
    if (n) { s->snapped_tris++; s->snapped_edges += (uint64_t)n; }
    return n;
}

#endif /* PGXP_SEAM_SNAP_H */
