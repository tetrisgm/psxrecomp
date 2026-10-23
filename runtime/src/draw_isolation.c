/* draw_isolation.c - private draw context for the draw-isolation gate.
 * See include/draw_isolation.h and docs/DRAW_ISOLATION.md. */
#include "draw_isolation.h"

#include <stdlib.h>
#include <string.h>

#include "cpu_state.h"
#include "psx_memory.h"

extern uint8_t *memory_get_ram_ptr(void);
extern uint8_t *memory_get_scratchpad_ptr(void);
extern uint32_t g_debug_last_store_pc;
/* debug_server.c: every guest RAM / scratchpad store (CPU and DMA). */
extern void (*g_psx_ram_write_observer)(uint32_t phys, uint32_t width);

typedef struct IsoSeg {
    uint32_t size;      /* bytes, multiple of 8 */
    uint64_t *d;        /* draw context copy */
    uint64_t *snap;     /* main as last left by a span */
    uint64_t *save;     /* main while a span runs */
    uint64_t *own;      /* 0xFF per draw-owned byte */
    uint64_t *pub;      /* 0xFF per byte handed to main at every leave */
    uint64_t *span;     /* 0xFF per byte the current span changed */
    uint64_t *mstore;   /* 0xFF per byte main stored to since the last fold */
} IsoSeg;

static struct {
    int on;
    int live;           /* live machine is the draw context */
    int split;          /* a span was split by an interrupt */
    int suspended;      /* explicit main-context side effect */
    uint32_t flags;
    uint32_t ram_size;
    uint64_t poison;
    struct CPUState *cpu;
    IsoSeg seg[PSX_DRAW_ISO_SEGS];
    PsxDrawIsoStats st;
} S;

static uint8_t *seg_live(uint32_t i) {
    switch (i) {
    case PSX_DRAW_ISO_SEG_RAM: return memory_get_ram_ptr();
    case PSX_DRAW_ISO_SEG_SPAD: return memory_get_scratchpad_ptr();
    default: return (uint8_t *)&S.cpu->gte_data[0];
    }
}

/* 0xFF in every byte lane of x that is nonzero. */
static inline uint64_t lanes(uint64_t x) {
    x |= x >> 4;
    x |= x >> 2;
    x |= x >> 1;
    x &= 0x0101010101010101ull;
    return x * 0xFFu;
}
static inline uint32_t lane_count(uint64_t m) {
    return (uint32_t)__builtin_popcountll(m & 0x0101010101010101ull);
}

static void seg_free(IsoSeg *g) {
    free(g->d); free(g->snap); free(g->save); free(g->own); free(g->pub);
    free(g->span); free(g->mstore);
    memset(g, 0, sizeof *g);
}

static int seg_alloc(IsoSeg *g, uint32_t size) {
    size_t words = size / 8u;
    g->size = size;
    g->d = calloc(words, 8); g->snap = calloc(words, 8); g->save = calloc(words, 8);
    g->own = calloc(words, 8); g->pub = calloc(words, 8); g->span = calloc(words, 8);
    g->mstore = calloc(words, 8);
    return g->d && g->snap && g->save && g->own && g->pub && g->span && g->mstore;
}

/* Main's stores since the last fold: the write observer's exact marks, plus
 * any value change it could not see (host-side pokes). */
static inline uint64_t main_stores_at(IsoSeg *g, const uint64_t *live, size_t w) {
    uint64_t m = g->mstore[w] | lanes(live[w] ^ g->snap[w]);
    g->mstore[w] = 0;
    return m;
}

/* Main live -> draw live. RAM and scratchpad merge by last writer; the GTE
 * register file is a separate context (each thread of a threaded build has
 * its own GTE), so nothing flows between the two GTE copies. */
static void seg_enter(uint32_t i) {
    IsoSeg *g = &S.seg[i];
    uint64_t *live = (uint64_t *)seg_live(i);
    uint64_t overwrites = 0;
    const size_t n = g->size / 8u;
    if (i == PSX_DRAW_ISO_SEG_GTE) {
        for (size_t w = 0; w < n; w++) { g->save[w] = live[w]; live[w] = g->d[w]; }
        return;
    }
    for (size_t w = 0; w < n; w++) {
        uint64_t l = live[w];
        uint64_t ms = main_stores_at(g, live, w);
        uint64_t o = g->own[w];
        if (ms & o) overwrites += lane_count(ms & o);
        o &= ~ms;
        g->own[w] = o;
        uint64_t d = (g->d[w] & o) | (l & ~o);
        g->d[w] = d;
        g->save[w] = l;
        live[w] = d;
    }
    S.st.logic_overwrites[i] += overwrites;
}

/* Draw live -> main live. */
static void seg_leave(uint32_t i) {
    IsoSeg *g = &S.seg[i];
    uint64_t *live = (uint64_t *)seg_live(i);
    const int poison = (S.flags & PSX_DRAW_ISO_POISON) != 0;
    const uint64_t p = S.poison;
    uint64_t changed = 0;
    const size_t n = g->size / 8u;
    if (i == PSX_DRAW_ISO_SEG_GTE) {
        for (size_t w = 0; w < n; w++) {
            uint64_t dw = lanes(live[w] ^ g->d[w]);
            if (dw) { changed += lane_count(dw); g->span[w] |= dw; g->own[w] |= dw; }
            g->d[w] = live[w];
            live[w] = g->save[w];
        }
        S.st.draw_bytes[i] += changed;
        return;
    }
    for (size_t w = 0; w < n; w++) {
        uint64_t x = live[w];
        uint64_t dw = lanes(x ^ g->d[w]);
        if (dw) { changed += lane_count(dw); g->span[w] |= dw; }
        g->d[w] = x;
        uint64_t o = g->own[w] | dw;
        uint64_t m = g->save[w];
        uint64_t pub = o & g->pub[w];
        m = (m & ~pub) | (x & pub);
        o &= ~pub;
        g->own[w] = o;
        if (poison) m = (m & ~o) | ((x ^ p) & o);
        live[w] = m;
        g->snap[w] = m;
    }
    S.st.draw_bytes[i] += changed;
}

static void on_exception_edge(int entering) {
    if (!S.on) return;
    if (entering && S.live) {
        for (uint32_t i = 0; i < PSX_DRAW_ISO_SEGS; i++) seg_leave(i);
        S.live = 0;
        S.split = 1;
        S.st.exception_swaps++;
    } else if (!entering && S.split) {
        for (uint32_t i = 0; i < PSX_DRAW_ISO_SEGS; i++) seg_enter(i);
        S.live = 1;
        S.split = 0;
    }
}

static void on_ram_write(uint32_t phys, uint32_t width) {
    IsoSeg *g;
    uint32_t off;
    if (S.live) return;               /* draw stores: found by the leave diff */
    phys &= 0x1FFFFFFFu;
    if (phys >= 0x1F800000u && phys < 0x1F800400u) {
        g = &S.seg[PSX_DRAW_ISO_SEG_SPAD]; off = phys - 0x1F800000u;
    } else if (phys < 0x00800000u) {
        g = &S.seg[PSX_DRAW_ISO_SEG_RAM]; off = phys & (S.ram_size - 1u);
    } else {
        return;
    }
    for (uint32_t k = 0; k < width && off + k < g->size; k++)
        ((uint8_t *)g->mstore)[off + k] = 0xFFu;
}

uint32_t psx_draw_iso_mmio_class(uint32_t a) {
    a &= 0x1FFFFFFFu;
    if (a >= 0x1F801810u && a < 0x1F801818u) return 1;
    if (a >= 0x1F801080u && a < 0x1F801100u) return 2;
    if (a >= 0x1F801070u && a < 0x1F801078u) return 3;
    if (a >= 0x1F801100u && a < 0x1F801130u) return 4;
    if (a >= 0x1F801800u && a < 0x1F801804u) return 5;
    if (a >= 0x1F801C00u && a < 0x1F802000u) return 6;
    if (a >= 0x1F801040u && a < 0x1F801060u) return 7;
    return 0;
}

static void on_mmio(uint32_t addr, int is_write) {
    if (!S.live) return;
    if (is_write) {
        S.st.mmio_writes++;
        S.st.mmio_last_write_addr = addr;
        S.st.mmio_last_write_pc = g_debug_last_store_pc;
        S.st.mmio_writes_by_class[psx_draw_iso_mmio_class(addr)]++;
    } else {
        S.st.mmio_reads++;
        S.st.mmio_last_read_addr = addr;
    }
}

int psx_draw_iso_begin(struct CPUState *cpu, uint32_t flags) {
    if (S.on) return 1;
    if (!cpu || psx_ram_live_bytes() % 8u) return 0;
    memset(&S, 0, sizeof S);
    S.cpu = cpu;
    S.flags = flags;
    S.ram_size = psx_ram_live_bytes();
    if (!seg_alloc(&S.seg[0], S.ram_size) || !seg_alloc(&S.seg[1], 1024u) ||
        !seg_alloc(&S.seg[2], 256u)) {
        for (uint32_t i = 0; i < PSX_DRAW_ISO_SEGS; i++) seg_free(&S.seg[i]);
        return 0;
    }
    for (uint32_t i = 0; i < PSX_DRAW_ISO_SEGS; i++) {
        memcpy(S.seg[i].d, seg_live(i), S.seg[i].size);
        memcpy(S.seg[i].snap, seg_live(i), S.seg[i].size);
    }
    S.poison = 0xA5A5A5A5A5A5A5A5ull;
    S.on = 1;
    g_psx_exception_edge_observer = on_exception_edge;
    g_psx_mmio_observer = on_mmio;
    g_psx_ram_write_observer = on_ram_write;
    return 1;
}

void psx_draw_iso_end(void) {
    if (!S.on) return;
    if (S.live) psx_draw_iso_leave(S.cpu);
    psx_draw_iso_sync();
    g_psx_exception_edge_observer = NULL;
    g_psx_mmio_observer = NULL;
    g_psx_ram_write_observer = NULL;
    /* Publish everything: the live machine becomes the merged image (the
     * main GTE context stays: the draw context's GTE is dropped). */
    for (uint32_t i = 0; i < PSX_DRAW_ISO_SEGS; i++) {
        IsoSeg *g = &S.seg[i];
        uint64_t *live = (uint64_t *)seg_live(i);
        if (i != PSX_DRAW_ISO_SEG_GTE)
            for (size_t w = 0, n = g->size / 8u; w < n; w++)
                live[w] = (g->d[w] & g->own[w]) | (live[w] & ~g->own[w]);
        seg_free(g);
    }
    S.on = 0;
}

int psx_draw_iso_enabled(void) { return S.on; }
int psx_draw_iso_live(void) { return S.live; }

int psx_draw_iso_enter(struct CPUState *cpu) {
    if (!S.on || S.live || S.split || S.suspended || cpu != S.cpu) return 0;
    if (psx_ram_live_bytes() != S.ram_size) { psx_draw_iso_end(); return 0; }
    for (uint32_t i = 0; i < PSX_DRAW_ISO_SEGS; i++) {
        memset(S.seg[i].span, 0, S.seg[i].size);
        seg_enter(i);
    }
    S.live = 1;
    S.st.spans++;
    return 1;
}

void psx_draw_iso_leave(struct CPUState *cpu) {
    (void)cpu;
    if (!S.on) return;
    if (S.split) { S.split = 0; return; }
    if (!S.live) return;
    /* A different poison per span, so a value main happens to compute from
     * one poison pattern is not the next one. */
    S.poison = 0xA5A5A5A5A5A5A5A5ull ^ (0x0101010101010101ull * (S.st.spans & 0x3Fu));
    for (uint32_t i = 0; i < PSX_DRAW_ISO_SEGS; i++) seg_leave(i);
    S.live = 0;
}

void psx_draw_iso_sync(void) {
    if (!S.on || S.live || S.split) return;
    for (uint32_t i = 0; i < PSX_DRAW_ISO_SEGS; i++) {
        IsoSeg *g = &S.seg[i];
        const uint64_t *live = (const uint64_t *)seg_live(i);
        uint64_t overwrites = 0;
        if (i == PSX_DRAW_ISO_SEG_GTE) continue;
        for (size_t w = 0, n = g->size / 8u; w < n; w++) {
            uint64_t ms = main_stores_at(g, live, w);
            if (!ms) continue;
            overwrites += lane_count(ms & g->own[w]);
            g->own[w] &= ~ms;
            g->snap[w] = live[w];
        }
        S.st.logic_overwrites[i] += overwrites;
    }
}

int psx_draw_iso_suspend(struct CPUState *cpu) {
    if (!S.on || !S.live || S.split || S.suspended || cpu != S.cpu) return 0;
    for (uint32_t i = 0; i < PSX_DRAW_ISO_SEGS; i++) seg_leave(i);
    S.live = 0;
    S.suspended = 1;
    return 1;
}

void psx_draw_iso_resume(struct CPUState *cpu) {
    if (!S.on || !S.suspended || cpu != S.cpu) return;
    for (uint32_t i = 0; i < PSX_DRAW_ISO_SEGS; i++) seg_enter(i);
    S.suspended = 0;
    S.live = 1;
}

static int locate(uint32_t addr, uint32_t *seg, uint32_t *off) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys >= 0x1F800000u && phys < 0x1F800400u) {
        *seg = PSX_DRAW_ISO_SEG_SPAD; *off = phys - 0x1F800000u; return 1;
    }
    if (phys < 0x00800000u) {
        *seg = PSX_DRAW_ISO_SEG_RAM; *off = phys & (S.ram_size - 1u); return 1;
    }
    return 0;
}

void psx_draw_iso_handoff(uint32_t addr, uint32_t len) {
    if (!S.on || S.live || S.split) return;
    for (uint32_t k = 0; k < len; k++) {
        uint32_t seg, off;
        if (!locate(addr + k, &seg, &off)) continue;
        IsoSeg *g = &S.seg[seg];
        uint8_t *own = (uint8_t *)g->own, *d = (uint8_t *)g->d, *snap = (uint8_t *)g->snap;
        uint8_t *live = seg_live(seg);
        if (!own[off]) continue;
        live[off] = d[off];
        snap[off] = d[off];
        own[off] = 0;
        S.st.handoff_bytes++;
    }
}

void psx_draw_iso_set_public(uint32_t addr, uint32_t len, int is_public) {
    if (!S.on) return;
    for (uint32_t k = 0; k < len; k++) {
        uint32_t seg, off;
        if (!locate(addr + k, &seg, &off)) continue;
        ((uint8_t *)S.seg[seg].pub)[off] = is_public ? 0xFFu : 0u;
    }
}

uint32_t psx_draw_iso_read_word(uint32_t addr) {
    uint32_t v = 0;
    for (uint32_t k = 0; k < 4; k++) {
        uint32_t seg, off;
        uint8_t b = 0;
        if (locate(addr + k, &seg, &off)) {
            if (S.on && !S.live && ((uint8_t *)S.seg[seg].own)[off])
                b = ((uint8_t *)S.seg[seg].d)[off];
            else if (seg == PSX_DRAW_ISO_SEG_RAM)
                b = memory_get_ram_ptr()[off & (psx_ram_live_bytes() - 1u)];
            else
                b = memory_get_scratchpad_ptr()[off];
        }
        v |= (uint32_t)b << (8u * k);
    }
    return v;
}

uint64_t psx_draw_iso_hash(struct CPUState *cpu, uint32_t seg) {
    return psx_draw_iso_hash_excluding(cpu, seg, NULL, 0);
}

uint64_t psx_draw_iso_hash_excluding(struct CPUState *cpu, uint32_t seg,
                                     const uint32_t *ranges, uint32_t nranges) {
    const uint64_t *live;
    size_t n;
    uint64_t h = 0xCBF29CE484222325ull;
    if (seg >= PSX_DRAW_ISO_SEGS) return 0;
    if (seg == PSX_DRAW_ISO_SEG_RAM) {
        live = (const uint64_t *)memory_get_ram_ptr(); n = psx_ram_live_bytes() / 8u;
    } else if (seg == PSX_DRAW_ISO_SEG_SPAD) {
        live = (const uint64_t *)memory_get_scratchpad_ptr(); n = 1024u / 8u;
    } else {
        if (!cpu) return 0;
        live = (const uint64_t *)&cpu->gte_data[0]; n = 256u / 8u;
    }
    /* The GTE hash is the main context's own registers. */
    const int merged = S.on && !S.live && !S.split && seg != PSX_DRAW_ISO_SEG_GTE &&
                       S.seg[seg].size / 8u == n;
    for (size_t w = 0; w < n; w++) {
        uint64_t v = live[w];
        if (merged) v = (S.seg[seg].d[w] & S.seg[seg].own[w]) | (v & ~S.seg[seg].own[w]);
        for (uint32_t r = 0; r < nranges; r++) {
            uint32_t off = (uint32_t)w * 8u, lo = ranges[2 * r], hi = ranges[2 * r + 1];
            if (seg == PSX_DRAW_ISO_SEG_RAM) { lo &= 0x1FFFFFu; hi = lo + (hi - ranges[2 * r]); }
            if (off + 8u > lo && off < hi) v = 0;
        }
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

static uint32_t seg_phys(uint32_t seg, uint32_t off) {
    if (seg == PSX_DRAW_ISO_SEG_SPAD) return 0x1F800000u + off;
    if (seg == PSX_DRAW_ISO_SEG_GTE) return 0xFFFF0000u + off;
    return off;
}

static void visit(const uint64_t *mask, uint32_t seg, PsxDrawIsoRangeFn fn, void *user) {
    const uint8_t *m = (const uint8_t *)mask;
    uint32_t size = S.seg[seg].size, start = 0;
    int in = 0;
    for (uint32_t off = 0; off <= size; off++) {
        int set = off < size && m[off];
        if (set && !in) { start = off; in = 1; }
        else if (!set && in) { fn(user, seg_phys(seg, start), off - start); in = 0; }
        if (off + 8u <= size && !in && !mask[off / 8u] && (off & 7u) == 0) off += 7u;
    }
}

void psx_draw_iso_last_span_ranges(PsxDrawIsoRangeFn fn, void *user) {
    if (!S.on || !fn) return;
    for (uint32_t i = 0; i < PSX_DRAW_ISO_SEGS; i++) visit(S.seg[i].span, i, fn, user);
}

void psx_draw_iso_owned_ranges(PsxDrawIsoRangeFn fn, void *user) {
    if (!S.on || !fn) return;
    for (uint32_t i = 0; i < PSX_DRAW_ISO_SEGS; i++) visit(S.seg[i].own, i, fn, user);
}

void psx_draw_iso_get_stats(PsxDrawIsoStats *out) {
    if (!out) return;
    *out = S.st;
    for (uint32_t i = 0; i < PSX_DRAW_ISO_SEGS; i++) {
        uint32_t c = 0;
        if (S.on)
            for (size_t w = 0, n = S.seg[i].size / 8u; w < n; w++) c += lane_count(S.seg[i].own[w]);
        out->owned_now[i] = c;
    }
}
