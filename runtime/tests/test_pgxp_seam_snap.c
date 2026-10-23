/* PGXP seam snapping measurements (pgxp_seam_snap.h). The crest case is the
 * R4 one: two consecutive road rows the PS1 drew on the same integer line,
 * whose PGXP edges sit a fraction of a pixel apart. */
#include "pgxp_seam_snap.h"
#undef NDEBUG /* the Release test target must execute its assertions */
#include <assert.h>
#include <stdio.h>

static PgxpSnap s;

typedef struct { float x, y; } P;

/* One triangle: precise positions p, integer positions their rounding
 * unless ip is given, depth z everywhere. Returns the snapped edge count. */
static int tri_z(const P p[3], const P *ip, float z, float e[3]) {
    int32_t ix[3], iy[3], fx[3], fy[3];
    float zz[3] = {z, z, z};
    uint32_t w[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        const P q = ip ? ip[i] : p[i];
        ix[i] = (int32_t)floorf(q.x + 0.5f);
        iy[i] = (int32_t)floorf(q.y + 0.5f);
        fx[i] = (int32_t)lrintf(p[i].x * 65536.0f);
        fy[i] = (int32_t)lrintf(p[i].y * 65536.0f);
    }
    return pgxp_snap_triangle(&s, ix, iy, fx, fy, zz, w, e);
}
static int tri(const P p[3], const P *ip, float e[3]) { return tri_z(p, ip, 1000.0f, e); }

static int near(float a, float b) { return fabsf(a - b) < 1e-3f; }

/* Row A: x 0..40, y 10..19.6 (integer bottom 20). */
static void row_a(void) {
    float e[3];
    const P a1[3] = {{0, 10}, {40, 10}, {40, 19.6f}};
    const P a2[3] = {{0, 10}, {40, 19.6f}, {0, 19.6f}};
    assert(tri(a1, NULL, e) == 0);
    assert(tri(a2, NULL, e) == 0);
}

static void test_crest(void) {
    pgxp_snap_reset(&s);
    row_a();
    /* Row B below, its top on the same integer line (20) but at 20.2: a
     * 0.6 px gap on its top edge (0 -> 1 of b1). */
    float e[3];
    const P b1[3] = {{0, 20.2f}, {40, 20.2f}, {40, 30}};
    assert(tri(b1, NULL, e) == 1);
    assert(near(e[0], 0.6f) && e[1] == 0.0f && e[2] == 0.0f);
    /* Its second half has only a corner on that line: no edge, no T. */
    const P b2[3] = {{0, 20.2f}, {40, 30}, {0, 30}};
    assert(tri(b2, NULL, e) == 0);
}

static void test_shared_mesh(void) {
    pgxp_snap_reset(&s);
    row_a();
    /* Same precise vertices as row A's bottom: one mesh, no gap. */
    float e[3];
    const P b1[3] = {{0, 19.6f}, {40, 19.6f}, {40, 30}};
    assert(tri(b1, NULL, e) == 0);
}

static void test_overlap_not_gap(void) {
    pgxp_snap_reset(&s);
    row_a();
    /* B's top edge reaches past A's edge (at 19.4, inside A): an overlap,
     * not a gap. */
    float e[3];
    const P b1[3] = {{0, 19.4f}, {40, 19.4f}, {40, 30}};
    const P i1[3] = {{0, 20}, {40, 20}, {40, 30}};
    assert(tri(b1, i1, e) == 0);
}

static void test_decal(void) {
    pgxp_snap_reset(&s);
    row_a();
    /* A lane mark painted on row A, its bottom edge along A's (same side,
     * same precise line): a decal gets no strips, even with a gap on its
     * other edges. */
    float e[3];
    const P d1[3] = {{5, 12}, {35, 19.6f}, {5, 19.6f}};
    assert(tri(d1, NULL, e) == 0);
    assert(e[0] == 0.0f && e[1] == 0.0f && e[2] == 0.0f);
}

static void test_depth_and_size(void) {
    float e[3];
    /* Far behind row A (depth differs by far more than 12%): unrelated. */
    pgxp_snap_reset(&s);
    row_a();
    const P b1[3] = {{0, 20.2f}, {40, 20.2f}, {40, 30}};
    assert(tri_z(b1, NULL, 3000.0f, e) == 0);
    /* A gap wider than a native pixel is geometry, not rounding. */
    pgxp_snap_reset(&s);
    row_a();
    const P i2[3] = {{0, 20}, {40, 20}, {40, 30}};   /* integer still 20 */
    const P b2[3] = {{0, 20.5f}, {40, 20.5f}, {40, 30}};
    assert(tri(b2, i2, e) == 1 && near(e[0], 0.9f));
    pgxp_snap_reset(&s);
    row_a();
    const P b3[3] = {{0, 21.4f}, {40, 21.4f}, {40, 30}};
    assert(tri(b3, i2, e) == 0);
}

static void test_t_junction(void) {
    pgxp_snap_reset(&s);
    row_a();
    /* A subdivided row below: its apex sits on the interior of A's bottom
     * edge in integer space, 0.5 px below it in precise space. The two
     * edges at the apex grow so the corner reaches the line. */
    float e[3];
    const P c[3] = {{20, 20.1f}, {30, 30}, {10, 30}};
    assert(tri(c, NULL, e) == 2);
    assert(e[0] > 0.0f && e[2] > 0.0f && e[1] == 0.0f);
    assert(e[0] <= 0.5f + 1e-3f && e[2] <= 0.5f + 1e-3f);
    /* Edges at the apex lean ~45 deg: the corner needs 0.5 * cos(45). */
    assert(fabsf(e[0] - 0.5f * 0.7071f) < 0.02f);
}

static void test_silhouette(void) {
    pgxp_snap_reset(&s);
    float e[3];
    /* Nothing drawn before: no neighbour, no strip. */
    const P b1[3] = {{0, 20.2f}, {40, 20.2f}, {40, 30}};
    assert(tri(b1, NULL, e) == 0);
    /* Reset forgets the frame. */
    pgxp_snap_reset(&s);
    row_a();
    pgxp_snap_reset(&s);
    assert(tri(b1, NULL, e) == 0);
}

int main(void) {
    test_crest();
    test_shared_mesh();
    test_overlap_not_gap();
    test_decal();
    test_depth_and_size();
    test_t_junction();
    test_silhouette();
    printf("pgxp_seam_snap: ok\n");
    return 0;
}
