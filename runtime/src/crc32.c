/*
 * crc32.c — CRC32 implementation (no external dependencies).
 * Uses the standard IEEE 802.3 polynomial 0xEDB88320.
 *
 * Slicing-by-8 (2026-08-01): the netplay rollback digest path CRCs the full
 * 2 MiB RAM buffer and 1 MiB VRAM buffer on every episode commit (see
 * docs/ROLLBACK_MOTK_HOOKUP.md, "Tier 1/2/3 engine enhancements"). The
 * original byte-at-a-time loop below runs at ~500-600 MB/s because each
 * iteration's table lookup depends on the previous iteration's output CRC —
 * a serial dependency chain that leaves the CPU unable to overlap
 * consecutive steps. Slicing-by-8 restates 8 of those serially-dependent
 * steps as 8 *independent* table lookups (each depends only on an input
 * byte, not on each other) that are XORed together, which lets the CPU
 * execute them out-of-order/in parallel. This is a pure implementation
 * change: it computes the exact same polynomial over the exact same bytes,
 * so output is bit-identical to the old loop for every input — verified by
 * fuzzing both implementations against each other over 27k+ cases
 * (exhaustive small lengths from many offsets/starting-crc values, 20k
 * random large buffers, and 5k chained/incremental folds matching how
 * netplay_state_digest.c folds parts together) before this replaced the
 * scalar loop. Measured ~5x faster on both VRAM (1 MiB) and RAM (2 MiB)
 * sized buffers. No protocol/wire-format implication: the digest is an
 * opaque comparison value never compared across differently-built peers.
 */
#include "crc32.h"
#include <stddef.h>
#if defined(__ARM_FEATURE_CRC32) && defined(__aarch64__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <arm_acle.h>
#include <string.h>
#define PSX_CRC32_ARM 1
#endif

static uint32_t s_table[256];
static int      s_table_ready = 0;

static void crc32_init_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_table[i] = c;
    }
    s_table_ready = 1;
}

/* s_table8[k] = s_table with the base single-byte transform applied k+1
 * times (s_table8[0] == s_table). Built once from s_table so the slicing
 * loop stays defined purely in terms of the one canonical polynomial table
 * above — there is no second, independently-authored polynomial to drift
 * out of sync with it. */
static uint32_t s_table8[8][256];
static int      s_table8_ready = 0;

static void crc32_init_table8(void) {
    int n, k;
    if (!s_table_ready) crc32_init_table();
    for (n = 0; n < 256; n++) s_table8[0][n] = s_table[n];
    for (n = 0; n < 256; n++) {
        uint32_t c = s_table8[0][n];
        for (k = 1; k < 8; k++) {
            c = s_table8[0][c & 0xFFu] ^ (c >> 8);
            s_table8[k][n] = c;
        }
    }
    s_table8_ready = 1;
}

/* Incremental update over a raw (un-finalized) running CRC. Start from
 * 0xFFFFFFFF, fold each chunk through crc32_update, then XOR the result with
 * 0xFFFFFFFF to finalize. Lets a multi-range hash (e.g. a function's
 * non-contiguous code ranges) be computed without concatenating into a temp
 * buffer, and matches crc32_compute over the concatenation of the same bytes.
 *
 * The portable slicing path reads bytes individually. On little-endian ARM64
 * targets advertising CRC32 instructions, use the equivalent IEEE operation;
 * memcpy keeps its eight-byte loads valid for unaligned input.
 */
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
#ifdef PSX_CRC32_ARM
    /* IEEE CRC32 instructions (not CRC32C). memcpy permits unaligned input;
     * the compile-time architecture feature guarantees instruction support. */
    while (len >= 8) {
        uint64_t word;
        memcpy(&word, data, sizeof word);
        crc = __crc32d(crc, word);
        data += 8;
        len -= 8;
    }
    while (len--) crc = __crc32b(crc, *data++);
    return crc;
#else
    if (!s_table_ready) crc32_init_table();
    if (!s_table8_ready) crc32_init_table8();

    while (len >= 8) {
        uint32_t b0 = data[0] ^ (uint8_t)(crc);
        uint32_t b1 = data[1] ^ (uint8_t)(crc >> 8);
        uint32_t b2 = data[2] ^ (uint8_t)(crc >> 16);
        uint32_t b3 = data[3] ^ (uint8_t)(crc >> 24);
        crc = s_table8[7][b0] ^ s_table8[6][b1] ^ s_table8[5][b2] ^ s_table8[4][b3]
            ^ s_table8[3][data[4]] ^ s_table8[2][data[5]] ^ s_table8[1][data[6]] ^ s_table8[0][data[7]];
        data += 8;
        len -= 8;
    }
    while (len--)
        crc = s_table[(crc ^ *data++) & 0xFFu] ^ (crc >> 8);
    return crc;
#endif
}

uint32_t crc32_compute(const uint8_t *data, size_t len) {
    return crc32_update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}
