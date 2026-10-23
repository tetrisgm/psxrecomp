#include "pgxp_seam_batch.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

static PgxpSnap baseline, serial, mutated;
static PgxpSnapBatch batch[4];

static PgxpSnapBatchInput make_tri(const float x[3], const float y[3]) {
    PgxpSnapBatchInput in = {0};
    for (int i = 0; i < 3; i++) {
        in.ix[i] = (int32_t)floorf(x[i] + 0.5f);
        in.iy[i] = (int32_t)floorf(y[i] + 0.5f);
        in.fx[i] = (int32_t)lrintf(x[i] * 65536.0f);
        in.fy[i] = (int32_t)lrintf(y[i] * 65536.0f);
        in.z[i] = 1000.0f;
    }
    return in;
}

int main(void) {
    const float xa[3] = {0, 40, 40}, ya[3] = {10, 10, 19.6f};
    const float xb[3] = {0, 40, 0}, yb[3] = {10, 19.6f, 19.6f};
    const float xc[3] = {0, 40, 40}, yc[3] = {20.2f, 20.2f, 30};
    const float xd[3] = {20, 30, 10}, yd[3] = {20.1f, 30, 30};
    PgxpSnapBatchInput input[4] = {
        make_tri(xa, ya), make_tri(xb, yb),
        make_tri(xc, yc), make_tri(xd, yd)
    };
    float expected[4][3];
    float gap[3];
    int expected_count[4];
    pgxp_snap_reset(&baseline);
    pgxp_snap_batch_copy_index(&serial, &baseline);
    for (int i = 0; i < 4; i++)
        expected_count[i] = pgxp_snap_triangle(&serial, input[i].ix, input[i].iy,
                                               input[i].fx, input[i].fy, input[i].z,
                                               input[i].word, expected[i]);
    assert(expected_count[2] == 1 && expected_count[3] == 2);
    for (int worker = 0; worker < 4; worker++) {
        float got[3]; int count;
        assert(pgxp_snap_batch_prepare(&batch[worker], &baseline, 41, input, 4));
        assert(pgxp_snap_batch_preflight(&batch[worker], &baseline, 41, input, 4));
        assert(!pgxp_snap_batch_next(&batch[worker], 1, &input[1], got, &count));
        for (int i = 0; i < 4; i++) {
            assert(pgxp_snap_batch_next(&batch[worker], (size_t)i, &input[i], got, &count));
            assert(count == expected_count[i]);
            assert(memcmp(got, expected[i], sizeof got) == 0);
        }
        assert(!pgxp_snap_batch_next(&batch[worker], 4, &input[3], got, &count));
        assert(batch[worker].index->n_edges == serial.n_edges);
        assert(batch[worker].index->n_links == serial.n_links);
        assert(pgxp_snap_batch_index_checksum(batch[worker].index) ==
               pgxp_snap_batch_index_checksum(&serial));
    }
    /* A stale, altered or reordered stream must fall back before drawing. */
    assert(!pgxp_snap_batch_preflight(&batch[0], &baseline, 42, input, 4));
    PgxpSnapBatchInput changed[4];
    memcpy(changed, input, sizeof changed);
    changed[2].fy[0]++;
    assert(!pgxp_snap_batch_preflight(&batch[0], &baseline, 41, changed, 4));
    memcpy(changed, input, sizeof changed);
    changed[1] = input[2]; changed[2] = input[1];
    assert(!pgxp_snap_batch_preflight(&batch[0], &baseline, 41, changed, 4));
    assert(pgxp_snap_batch_preflight(&batch[0], &baseline, 41, input, 4));
    assert(pgxp_snap_batch_next(&batch[0], 0, &input[0], gap, &expected_count[0]));
    assert(!pgxp_snap_batch_next(&batch[0], 1, &changed[1], gap, &expected_count[1]));
    assert(!batch[0].verified); /* discard, never resume a partial batch */
    pgxp_snap_batch_copy_index(&mutated, &baseline);
    (void)pgxp_snap_triangle(&mutated, input[0].ix, input[0].iy, input[0].fx,
                             input[0].fy, input[0].z, input[0].word, gap);
    assert(!pgxp_snap_batch_preflight(&batch[0], &mutated, 41, input, 4));
    /* After failed preflight the baseline is intact for the serial path. */
    assert(pgxp_snap_batch_index_checksum(&baseline) !=
           pgxp_snap_batch_index_checksum(&serial));
    for (int i = 0; i < 4; i++) pgxp_snap_batch_release(&batch[i]);
    puts("pgxp_seam_batch: ok");
    return 0;
}
