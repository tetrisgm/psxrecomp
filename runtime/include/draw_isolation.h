/* draw_isolation.h - debug gate for moving a title's draw code off the
 * emulation thread (docs/DRAW_ISOLATION.md).
 *
 * A title plugin wraps spans of guest code it believes are "draw" (for
 * example every call of a frame's draw functions) in psx_draw_iso_enter() /
 * psx_draw_iso_leave(). Inside a span the live machine is a private draw
 * context: main RAM, the scratchpad and the GTE register file hold the draw
 * context's copy, so the span's stores never reach the main context. Outside
 * spans the live machine is the main context.
 *
 * RAM and scratchpad are merged per byte by last writer, so the run
 * reproduces the stock machine exactly while the main context never reads a
 * draw store:
 *
 *   - entering a span, every byte the main context stored since the last
 *     span (logic stores, device DMA, interrupt handlers: seen exactly by the
 *     debug server's write hook, so PSX_DEBUG_TOOLS builds only) replaces the
 *     draw copy's byte; bytes the draw context wrote last keep the draw value;
 *   - leaving it, the bytes the span changed are marked draw-owned and the
 *     main context gets its own (stale) value back, or a poison value with
 *     PSX_DRAW_ISO_POISON;
 *   - psx_draw_iso_handoff() publishes a range to the main context (the
 *     frame's ordering tables and packets before the game submits them).
 *
 * The GTE register file is not merged: the draw context and the main
 * context each keep their own (as each thread of a threaded build would), so
 * a GTE value set on one side and used on the other shows up as a
 * divergence.
 *
 * If the main context never reads a draw-owned byte, the merged image (main
 * with draw-owned bytes from the draw copy) equals the stock run's state at
 * every point, and so do the guest's writes, device traffic and frames. A run
 * that diverges from the stock run names a dependency: main code read draw
 * state. Interrupts taken inside a span run on the main context (the hook in
 * psx_check_interrupts), as they would on the emulation thread of a threaded
 * build. Cycles are not isolated: spans run in place on the guest clock.
 *
 * Debug tool: off unless a plugin calls psx_draw_iso_begin(); while off the
 * runtime pays one predictable branch per exception delivery and MMIO access.
 */
#ifndef PSXRECOMP_DRAW_ISOLATION_H
#define PSXRECOMP_DRAW_ISOLATION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* psx_draw_iso_begin() flags */
#define PSX_DRAW_ISO_POISON      0x1u  /* main context sees draw-owned bytes inverted */
#define PSX_DRAW_ISO_POISON_REGS 0x2u  /* poison caller-saved GPRs after a span (plugin) */

/* Segments of the isolated image. */
#define PSX_DRAW_ISO_SEG_RAM  0u
#define PSX_DRAW_ISO_SEG_SPAD 1u
#define PSX_DRAW_ISO_SEG_GTE  2u   /* cpu->gte_data[0..31], gte_ctrl[0..31] */
#define PSX_DRAW_ISO_SEGS     3u

typedef struct PsxDrawIsoStats {
    uint64_t spans;               /* enter/leave pairs */
    uint64_t exception_swaps;     /* interrupts taken inside a span */
    uint64_t mmio_writes;         /* device-register stores inside spans */
    uint64_t mmio_reads;          /* device-register loads inside spans */
    uint32_t mmio_last_write_addr, mmio_last_read_addr;
    uint32_t mmio_last_write_pc;
    uint64_t draw_bytes[PSX_DRAW_ISO_SEGS];       /* bytes changed by spans (cumulative) */
    uint64_t logic_overwrites[PSX_DRAW_ISO_SEGS]; /* draw-owned bytes later written by main */
    uint64_t handoff_bytes;       /* draw-owned bytes published by handoff */
    uint32_t owned_now[PSX_DRAW_ISO_SEGS];        /* draw-owned bytes at the query */
    uint64_t mmio_writes_by_class[8]; /* see psx_draw_iso_mmio_class() */
} PsxDrawIsoStats;

/* Allocate the contexts and start isolating; the draw context starts as a
 * copy of the live machine. Returns 0 if allocation failed. */
int  psx_draw_iso_begin(struct CPUState *cpu, uint32_t flags);
void psx_draw_iso_end(void);
int  psx_draw_iso_enabled(void);
/* Nonzero while the live machine is the draw context (inside a span and not
 * in an interrupt it took). */
int  psx_draw_iso_live(void);

/* Switch the live machine to the draw context / back to main. Spans do not
 * nest; enter returns 0 (and does nothing) when already inside one. */
int  psx_draw_iso_enter(struct CPUState *cpu);
void psx_draw_iso_leave(struct CPUState *cpu);

/* Temporarily execute a synchronous side effect on main, retaining the
 * current span's write map. GPR arguments/results stay live; pointers must
 * refer to main-owned data. Resume after the main call returns. */
int psx_draw_iso_suspend(struct CPUState *cpu);
void psx_draw_iso_resume(struct CPUState *cpu);

/* Fold the main context's stores since the last span into the ownership
 * map (a byte main wrote is main's again). Call before handoff, merged reads,
 * hashes and range queries once main code has run since the last span. */
void psx_draw_iso_sync(void);

/* Publish [addr, addr+len) to the main context: draw-owned bytes there take
 * the draw value in main and stop being draw-owned. addr is a guest address
 * (KUSEG/KSEG0/KSEG1 RAM or scratchpad). Only outside spans. */
void psx_draw_iso_handoff(uint32_t addr, uint32_t len);

/* Ablation: bytes marked public are handed to the main context as soon as a
 * span leaves (they are not isolated). Default: everything private. */
void psx_draw_iso_set_public(uint32_t addr, uint32_t len, int is_public);

/* Read the merged (stock-equivalent) view: the draw value for draw-owned
 * bytes, else main's. Only outside spans. */
uint32_t psx_draw_iso_read_word(uint32_t addr);
/* 64-bit FNV-1a-style hash of the merged image of one segment (the whole
 * live RAM size for SEG_RAM). Without isolation it hashes the live machine,
 * so a stock run and an isolated run can be compared tick by tick. */
uint64_t psx_draw_iso_hash(struct CPUState *cpu, uint32_t seg);
/* The same, with the 8-byte words that overlap the given guest address
 * ranges (pairs lo, hi; RAM segment only) hashed as zero: scratch areas whose
 * contents are garbage by design (dead stack, the kernel's saved registers
 * of an interrupted context). */
uint64_t psx_draw_iso_hash_excluding(struct CPUState *cpu, uint32_t seg,
                                     const uint32_t *ranges, uint32_t nranges);

/* Visit the byte ranges the last span changed (merged across interrupt
 * splits), in address order. phys is RAM offset, 0x1F800000+ scratchpad, or
 * 0xFFFF0000 + byte offset into the GTE register file. */
typedef void (*PsxDrawIsoRangeFn)(void *user, uint32_t phys, uint32_t len);
void psx_draw_iso_last_span_ranges(PsxDrawIsoRangeFn fn, void *user);
/* Visit the currently draw-owned byte ranges (same address forms). */
void psx_draw_iso_owned_ranges(PsxDrawIsoRangeFn fn, void *user);

void psx_draw_iso_get_stats(PsxDrawIsoStats *out);

/* MMIO class of a device-register address: 0 other, 1 GPU (GP0/GP1),
 * 2 DMA, 3 IRQ (I_STAT/I_MASK), 4 timers, 5 CD, 6 SPU, 7 SIO/pad. */
uint32_t psx_draw_iso_mmio_class(uint32_t addr);

/* Runtime observers, owned by the files that call them so standalone test
 * targets need not link this module: interrupts.c calls the edge observer on
 * the outermost exception entry (1) and exit (0); memory.c calls the MMIO
 * observer on every device-register access. NULL unless isolating. */
extern void (*g_psx_exception_edge_observer)(int entering);
extern void (*g_psx_mmio_observer)(uint32_t addr, int is_write);

#ifdef __cplusplus
}
#endif

#endif
