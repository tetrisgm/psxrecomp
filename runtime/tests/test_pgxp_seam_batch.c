#include "pgxp_seam_rth_stream.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

static PgxpSnap baseline, serial, mutated;
static PgxpSnapBatch batch[4];

static PgxpRthSeamEvent test_map(uint16_t op) {
    return op == 1 ? PGXP_RTH_PRECISE :
           op == 2 ? PGXP_RTH_DEPTH :
           op == 3 ? PGXP_RTH_FLAT_TRI : PGXP_RTH_UNSUPPORTED;
}

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
    PgxpRthSeamStream stream;
    pgxp_rth_seam_stream_init(&stream, 1, 1, 100.0f, 0);
    for (int i = 0; i < 4; i++) {
        int32_t precise[7] = {1};
        int32_t depth[4] = {1};
        int32_t tri[7] = {input[i].ix[0],input[i].iy[0],input[i].ix[1],
                          input[i].iy[1],input[i].ix[2],input[i].iy[2],0};
        for (int k = 0; k < 3; k++) {
            precise[1 + 2*k] = input[i].fx[k];
            precise[2 + 2*k] = input[i].fy[k];
        }
        memcpy(depth + 1, input[i].z, sizeof input[i].z);
        assert(pgxp_rth_seam_stream_feed(&stream, PGXP_RTH_PRECISE, precise, 7));
        assert(pgxp_rth_seam_stream_feed(&stream, PGXP_RTH_DEPTH, depth, 4));
        assert(pgxp_rth_seam_stream_feed(&stream, PGXP_RTH_FLAT_TRI, tri, 7));
    }
    assert(stream.count == 4);
    for (int i = 0; i < 4; i++)
        assert(pgxp_snap_batch_input_equal(&stream.input[i], &input[i]));
    int32_t semi[2] = {1, 0};
    assert(pgxp_rth_seam_stream_feed(&stream, PGXP_RTH_SEMI, semi, 2));
    int32_t precise[7] = {1,0,0,0,0,0,0}, depth[4] = {1};
    memcpy(depth + 1, input[0].z, sizeof input[0].z);
    int32_t tri[7] = {0};
    assert(pgxp_rth_seam_stream_feed(&stream, PGXP_RTH_PRECISE, precise, 7));
    assert(pgxp_rth_seam_stream_feed(&stream, PGXP_RTH_DEPTH, depth, 4));
    assert(pgxp_rth_seam_stream_feed(&stream, PGXP_RTH_FLAT_TRI, tri, 7));
    assert(stream.count == 4); /* semitransparent triangles do not snap */
    semi[0] = 0;
    assert(pgxp_rth_seam_stream_feed(&stream, PGXP_RTH_SEMI, semi, 2));
    const PgxpRthSeamEvent kinds[3] = {
        PGXP_RTH_GOURAUD_TRI, PGXP_RTH_TEX_TRI, PGXP_RTH_SHADED_TEX_TRI
    };
    const size_t width[3] = {9, 15, 19};
    const size_t pos[3][6] = {
        {0,1,3,4,6,7}, {0,1,4,5,8,9}, {0,1,5,6,10,11}
    };
    for (int kind = 0; kind < 3; kind++) {
        int32_t words[19] = {0};
        for (int k = 0; k < 3; k++) {
            words[pos[kind][2*k]] = input[0].ix[k];
            words[pos[kind][2*k+1]] = input[0].iy[k];
        }
        assert(pgxp_rth_seam_stream_feed(&stream, PGXP_RTH_PRECISE, precise, 7));
        assert(pgxp_rth_seam_stream_feed(&stream, PGXP_RTH_DEPTH, depth, 4));
        assert(pgxp_rth_seam_stream_feed(&stream, kinds[kind], words, width[kind]));
        assert(stream.count == (size_t)5 + (size_t)kind);
    }
    /* Near geometry is deliberately excluded by the renderer's depth gate. */
    float near_z[3] = {10.0f, 1000.0f, 1000.0f};
    memcpy(depth + 1, near_z, sizeof near_z);
    assert(pgxp_rth_seam_stream_feed(&stream, PGXP_RTH_PRECISE, precise, 7));
    assert(pgxp_rth_seam_stream_feed(&stream, PGXP_RTH_DEPTH, depth, 4));
    assert(pgxp_rth_seam_stream_feed(&stream, PGXP_RTH_FLAT_TRI, tri, 7));
    assert(stream.count == 7);
    assert(!pgxp_rth_seam_stream_feed(&stream, PGXP_RTH_UNSUPPORTED, NULL, 0));
    assert(!stream.valid);
    pgxp_rth_seam_stream_release(&stream);
    pgxp_rth_seam_stream_init(&stream, 1, 1, 100.0f, 0);
    int32_t grouped[2+7 + 2+4 + 2+7] = {
        1,7, 1,0,0,0,0,0,0,
        2,4, 1,0,0,0,
        3,7, 0,0,40,0,40,20,0
    };
    float group_z[3] = {1000.0f, 1000.0f, 1000.0f};
    memcpy(&grouped[12], group_z, sizeof group_z);
    assert(pgxp_rth_seam_stream_feed_group(&stream, grouped,
                                           sizeof grouped / sizeof grouped[0], test_map));
    assert(stream.count == 1);
    assert(!pgxp_rth_seam_stream_feed_group(&stream, grouped,
                                            sizeof grouped / sizeof grouped[0] - 1, test_map));
    pgxp_rth_seam_stream_release(&stream);
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
