/* psx_cyc.h — R3000A load-delay pipeline interlock (ReadAbsorb / ReadFudge /
 * LDAbsorb / LDWhich).  TIMING ONLY — the load VALUE delay is handled by the
 * existing load-delay correctness path; this models only the CPU cycle cost.
 *
 * FAITHFUL_TIMING_PLAN.md / accuracy/load_readfudge_ldabsorb.md. Transcribed
 * (facts, not code) from the in-tree Beetle oracle
 * psxrecomp/beetle-psx/mednafen/psx/cpu.cpp (RunReal §1 base 795-798, GPR_DEPRES
 * 702-705, DO_LDS 800, ReadMemory 364-451) and libretro.cpp (main-RAM read wait
 * = +3, libretro.cpp:884). Every backend — the dirty-RAM interpreter and BOTH
 * static emitters (code_generator.cpp, full_function_emitter.cpp/strict_translator.cpp)
 * — accounts guest CPU cycles THROUGH THESE HELPERS so they can never disagree.
 *
 * Model (per retired instruction, in program order):
 *   §1   base : if(ReadAbsorb[ReadAbsorbWhich]) ReadAbsorb[ReadAbsorbWhich]--;
 *               else timestamp++;            -- a pending load's give-back is
 *               consumed INSTEAD of the +1 base (pipeline write-back overlap).
 *   deps      : for each GPR this opcode reads/writes, ReadAbsorb[reg]=0 (ends a
 *               give-back when the loaded value is used). ReadAbsorb[0] preserved.
 *   DO_LDS    : commit the PREVIOUS instruction's pending load TIMING:
 *               ReadAbsorb[LDWhich]=LDAbsorb; ReadFudge=LDWhich; ...; LDWhich=0x20.
 *   load only : ReadMemory — clear the current give-back slot, charge the +2 fudge
 *               iff the predecessor committed no load (ReadFudge==0x20), charge the
 *               region wait (main RAM +3) + completion (+2 CPU / +1 LWC2); the
 *               (region+completion) becomes this load's LDAbsorb give-back, armed on
 *               LDWhich=rt for the NEXT instruction's DO_LDS to commit.
 */
#ifndef PSX_CYC_H
#define PSX_CYC_H

#include <stdint.h>
#include <string.h>
#if defined(_MSC_VER)
#include <intrin.h>       /* MSVC intrinsics: _BitScanForward (no __builtin_ctz) */
#endif
#include "cpu_state.h"   /* CPUState (guard-safe: cpu_state.h includes us last) */
#include "psx_cycles.h"  /* inline psx_advance_cycles */
#include "psx_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load-charge batching (MotK VLC): cache the nearer of the next device
 * deadline or a 64-cycle soft publication limit once per batch. Generated
 * GCC/Clang blocks may defer to their IRQ edge; MMIO also flushes. Pipeline
 * state still updates per instruction, and guest totals at barriers match. */
enum { PSX_CYC_BATCH_SOFT = 64u };

#if defined(PSX_OVERLAY_DLL_BUILD)
/* The overlay shim already batches cycle charges until a host callback edge. */
static inline void psx_cyc_bb_defer_begin(void) { }
static inline void psx_cyc_bb_defer_end(void) { }
static inline void psx_cyc_bb_defer_flush(void) { overlay_flush_cycles(); }
static inline void psx_cyc_local_begin(uint32_t *acc) { (void)acc; }
static inline void psx_cyc_local_end(void) { }
#else
static inline void psx_cyc_bb_defer_begin(void) { g_psx_cyc_bb_defer++; }
static inline void psx_cyc_bb_defer_end(void) {
    if (g_psx_cyc_bb_defer > 0) g_psx_cyc_bb_defer--;
    if (g_psx_cyc_bb_defer == 0) psx_cyc_batch_flush();
}
static inline void psx_cyc_bb_defer_flush(void) { psx_cyc_batch_flush(); }

/* Install a function-local charge sink (MotK VLC leaves). Soft-cap publish
 * at 1<<28 so uint32 cannot wrap; IRQ/MMIO still go through batch_flush. */
static inline void psx_cyc_local_begin(uint32_t *acc) {
#if !defined(PSX_COSIM)
    if (acc) *acc = 0;
    g_psx_cyc_local_acc = acc;
#else
    (void)acc;
#endif
}
static inline void psx_cyc_local_end(void) {
#if !defined(PSX_COSIM)
    psx_cyc_local_publish();
    g_psx_cyc_local_acc = NULL;
#endif
}
#endif

#if defined(__GNUC__) || defined(__clang__)
static inline void psx_cyc_bb_defer_cleanup(int *guard) {
    (void)guard;
    psx_cyc_bb_defer_end();
}
static inline void psx_cyc_local_cleanup(uint32_t **guard) {
    (void)guard;
    psx_cyc_local_end();
}
#endif

/* Keep exceptional publication/replay machinery out of every instruction.
 * Literal dependency masks can then fold in the small inline interlocks. */
#if defined(_MSC_VER)
static __declspec(noinline) void psx_cyc_charge_slow(uint32_t cycles) {
#elif defined(__GNUC__) || defined(__clang__)
static __attribute__((noinline)) void psx_cyc_charge_slow(uint32_t cycles) {
#else
static void psx_cyc_charge_slow(uint32_t cycles) {
#endif
    if (cycles == 0u) return;
#if defined(PSX_OVERLAY_DLL_BUILD)
    psx_cpu_charge(cycles);
#else
    /* [timing] guest_cycle_scale: a scaled charge is the CPU charge itself
     * (the one place CPU work is scaled); the batch below is the scale-1
     * production path, unchanged. */
    if (PSX_GCS_ACTIVE()) {
        /* Opt-in ([timing] guest_cycle_scale_batch, PSX_GCS_BATCH=0/1): scale
         * here, exactly as psx_cpu_charge would (same fractional carry, same
         * order), then batch the scaled cycles like the scale-1 path instead
         * of publishing every 1-cycle step to the devices. The batch is
         * flushed at the same observation points (stores, MMIO, interrupt
         * checks, deadlines) as at scale 1. */
        if (!g_psx_gcs_batch) {
            psx_cpu_charge(cycles);
            return;
        }
        cycles = psx_gcs_scale(cycles);
        if (cycles == 0u) return;
#if !defined(PSX_COSIM)
        /* Publish on the guest clock alone: the device deadline or the soft
         * cap. The scale-1 path below also defers while g_psx_cyc_bb_defer is
         * nonzero and routes through the VLC local sink; both are host call-
         * stack state (longjmp escapes leak or reset the depth), so which
         * charge published the batch -- and so which device saw which clock
         * when -- differed between a Mac and a Windows peer, and the scaled
         * race split by a few cycles (R4 2P Mac<->PC: clk/tim fork at ~3296
         * with cpu/ram equal; matching with the batch off). */
        if (!g_ls_replay_active && !g_event_step_conservative &&
            !psx_in_device_service) {
            uint32_t prior = g_psx_cyc_batch;
            uint32_t sum = prior + cycles;
            if (sum >= prior) {
                if (prior == 0u) {
                    uint64_t room = 0u;
                    if (psx_next_service_cycle > psx_cycle_count)
                        room = psx_next_service_cycle - psx_cycle_count;
                    g_psx_cyc_batch_limit = room == 0u ? 1u :
                        (room < (uint64_t)PSX_CYC_BATCH_SOFT
                            ? (uint32_t)room : (uint32_t)PSX_CYC_BATCH_SOFT);
                }
                if (sum < g_psx_cyc_batch_limit) {
                    g_psx_cyc_batch = sum;
                    return;
                }
            }
            psx_advance_cycles(cycles);
            return;
        }
#endif
    }
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_expect(g_ls_replay_active | g_event_step_conservative, 0)) {
#else
    if (g_ls_replay_active || g_event_step_conservative) {
#endif
        psx_advance_cycles(cycles);
        return;
    }
    if (psx_in_device_service) {
        psx_cycle_count += (uint64_t)cycles;
        return;
    }
#if !defined(PSX_COSIM)
    /* Emitter-gated VLC path: stack local, no deadline probe. */
    if (g_psx_cyc_local_acc) {
        uint32_t prior = *g_psx_cyc_local_acc;
        if (cycles <= UINT32_MAX - prior && (prior + cycles) < (1u << 28)) {
            *g_psx_cyc_local_acc = prior + cycles;
            return;
        }
        *g_psx_cyc_local_acc = 0;
        {
            uint32_t *saved = g_psx_cyc_local_acc;
            g_psx_cyc_local_acc = NULL;
            if (prior) psx_advance_cycles(prior);
            g_psx_cyc_local_acc = saved;
        }
        *g_psx_cyc_local_acc = cycles;
        return;
    }
    {
        uint32_t prior = g_psx_cyc_batch;
        uint32_t sum = prior + cycles;
        if (sum >= prior) { /* no uint32 wrap */
            if (g_psx_cyc_bb_defer > 0) {
                g_psx_cyc_batch = sum;
                return;
            }
            if (prior == 0u) {
                uint64_t room = 0u;
                if (psx_next_service_cycle > psx_cycle_count)
                    room = psx_next_service_cycle - psx_cycle_count;
                g_psx_cyc_batch_limit = room == 0u ? 1u :
                    (room < (uint64_t)PSX_CYC_BATCH_SOFT
                        ? (uint32_t)room : (uint32_t)PSX_CYC_BATCH_SOFT);
            }
            if (sum < g_psx_cyc_batch_limit) {
                g_psx_cyc_batch = sum;
                return;
            }
        }
    }
#endif
    psx_advance_cycles(cycles); /* publishes any pending batch first */
#endif
}

/* Arm once per generated block using its static upper bound. A device,
 * cache refill, GTE/muldiv observation or ordinary publication disarms it.
 * No deadline can occur between the instructions accumulated here. */
static inline void psx_cyc_block_begin(uint32_t bound) {
#if defined(PSX_ENABLE_BLOCK_CYCLES) && !defined(PSX_OVERLAY_DLL_BUILD) && !defined(PSX_COSIM)
    extern int g_debug_cycle_hooks, g_psx_guest_time_frozen, g_ls_mode;
    extern volatile int g_ds_recording;
    psx_cyc_block_publish();
    if (!bound || bound > 1048576u || g_psx_cyc_batch > UINT32_MAX - bound ||
        g_ls_replay_active || g_event_step_conservative || psx_in_device_service ||
        g_debug_cycle_hooks || g_psx_guest_time_frozen || g_ls_mode || g_ds_recording ||
        g_psx_cyc_local_acc) return;
    uint64_t now = psx_cycle_count + g_psx_cyc_batch;
    if (psx_next_service_cycle > now && psx_next_service_cycle - now > bound)
        g_psx_cyc_block_active = 1;
#else
    (void)bound;
#endif
}

/* Recheck after a cache refill or another observation disabled accumulation.
 * A still-active block retains its original proof and pending raw charges. */
PSX_MEM_INLINE void psx_cyc_block_resume(uint32_t bound) {
#if defined(PSX_ENABLE_BLOCK_CYCLES) && !defined(PSX_OVERLAY_DLL_BUILD) && !defined(PSX_COSIM)
    if (!PSX_MEM_LIKELY(g_psx_cyc_block_active)) psx_cyc_block_begin(bound);
#else
    (void)bound;
#endif
}

/* Only an add on the guarded path. Keep general timing out of the generated
 * instruction bodies; outlining it also avoids a large instruction-cache tax. */
PSX_MEM_INLINE void psx_cyc_charge(uint32_t cycles) {
#if defined(PSX_ENABLE_BLOCK_CYCLES) && !defined(PSX_OVERLAY_DLL_BUILD) && !defined(PSX_COSIM)
    if (PSX_MEM_LIKELY(g_psx_cyc_block_active)) {
        g_psx_cyc_block_raw += cycles;
        return;
    }
#endif
    psx_cyc_charge_slow(cycles);
}

/* §1 base (Beetle cpu.cpp:795-798). */
PSX_MEM_INLINE void psx_cyc_base(CPUState* cpu) {
    uint8_t w = cpu->read_absorb_which;
    if (cpu->read_absorb[w].value) cpu->read_absorb[w].value--;
    else                     psx_cyc_charge(1u);
}

/* GPR_DEPRES (Beetle cpu.cpp:702-705): zero ReadAbsorb[n] for every source/dest
 * GPR of this instruction, preserving ReadAbsorb[0] (skipping bit 0 == Beetle's
 * save/restore of ReadAbsorb[0]). */
PSX_MEM_INLINE void psx_cyc_deps(CPUState* cpu, uint32_t reg_mask) {
    reg_mask &= 0xFFFFFFFEu;   /* never touch ReadAbsorb[0] */
    if (reg_mask && (reg_mask & (reg_mask - 1u)) == 0u) {
#if defined(_MSC_VER)
        unsigned long n;
        _BitScanForward(&n, reg_mask);
        cpu->read_absorb[(unsigned)n].value = 0u;
#else
        cpu->read_absorb[(unsigned)__builtin_ctz(reg_mask)].value = 0u;
#endif
        return;
    }
    while (reg_mask) {
#if defined(_MSC_VER)
        unsigned long _psx_ctz_idx;
        _BitScanForward(&_psx_ctz_idx, reg_mask);
        unsigned n = (unsigned)_psx_ctz_idx;
#else
        unsigned n = (unsigned)__builtin_ctz(reg_mask);
#endif
        cpu->read_absorb[n].value = 0u;
        reg_mask &= reg_mask - 1u;
    }
}

/* DO_LDS timing-commit (Beetle cpu.cpp:800). LDWhich==0x20 (no pending) writes the
 * dummy slot read_absorb[32] and sets read_fudge=0x20 (=> next load gets +2 fudge). */
PSX_MEM_INLINE void psx_cyc_lds(CPUState* cpu) {
    uint8_t lw = cpu->ld_which_t;
    cpu->read_absorb[lw].value    = (uint8_t)cpu->ld_absorb;
    cpu->read_fudge         = lw;
    cpu->read_absorb_which  = (uint8_t)(cpu->read_absorb_which | (lw & 0x1Fu));
    cpu->ld_which_t         = 0x20u;
}

/* Full per-instruction interlock for a NON-CPU-load instruction (ALU, shift,
 * branch, jump, store, COP control, LWC2/SWC2 pre-step, mult/div, mfhi/mflo, ...).
 * reg_mask from psx_cyc_dep_res_mask(). MUST be emitted BEFORE the instruction
 * body so §1 precedes any muldiv/GTE deadline stall in the body (Beetle order). */
PSX_MEM_INLINE void psx_cyc_step(CPUState* cpu, uint32_t reg_mask) {
    psx_cyc_base(cpu);
    psx_cyc_deps(cpu, reg_mask);
    psx_cyc_lds(cpu);
}

/* Static straight-line segments never cross an observation or a cache line.
 * Leave a strict margin before the next device deadline: a callback can inspect
 * registers, so moving a charge over that boundary would be observably wrong.
 * Diagnostic/replay builds retain instruction-by-instruction sequencing. */
PSX_MEM_INLINE int psx_cyc_segment_ready(uint32_t count) {
#if defined(PSX_OVERLAY_DLL_BUILD) || defined(PSX_COSIM)
    (void)count; return 0;
#else
    extern int g_debug_cycle_hooks;
    if (g_psx_cyc_block_active) return 1;
    if (g_ls_replay_active || g_event_step_conservative || psx_in_device_service ||
        g_debug_cycle_hooks || g_psx_cyc_local_acc) return 0;
    uint64_t now = psx_cycle_count + g_psx_cyc_batch;
    return psx_next_service_cycle > now && psx_next_service_cycle - now > count;
#endif
}

/* Closed-form composition of 2..4 non-load interlocks. The first commit may
 * change the active give-back slot; thereafter LDWhich is the dummy slot and
 * the first dependency on the active register ends its overlap. Each mask is
 * a compile-time constant supplied by the emitter. */
PSX_MEM_INLINE void psx_cyc_segment(CPUState *cpu, uint32_t count,
                                  uint32_t m0, uint32_t m1,
                                  uint32_t m2, uint32_t m3) {
    uint8_t w = cpu->read_absorb_which;
    uint32_t cost = cpu->read_absorb[w].value ? 0u : 1u;
    if (cpu->read_absorb[w].value) --cpu->read_absorb[w].value;
    psx_cyc_deps(cpu, m0);
    psx_cyc_lds(cpu);
    w = cpu->read_absorb_which;
    uint32_t rest = count - 1u;
    uint32_t bit = w ? (1u << w) : 0u;
    uint32_t stop = (m1 & bit) ? 1u :
        (count > 2u && (m2 & bit)) ? 2u :
        (count > 3u && (m3 & bit)) ? 3u : rest;
    uint32_t overlap = cpu->read_absorb[w].value;
    if (overlap > stop) overlap = stop;
    cost += rest - overlap;
    cpu->read_absorb[w].value = cpu->read_absorb[w].value > rest ?
        (uint8_t)(cpu->read_absorb[w].value - rest) : 0u;
    psx_cyc_deps(cpu, m1 | (count > 2u ? m2 : 0u) | (count > 3u ? m3 : 0u));
    cpu->read_absorb[32].value = (uint8_t)cpu->ld_absorb;
    cpu->read_fudge = 32u;
    psx_cyc_charge(cost);
}

/* The GPR dep+res bitmask used by psx_cyc_step lives in psx_instr_cost.h
 * (psx_cyc_dep_res_mask) — a standalone pure function shared by the emitters
 * (gen-time literal) and the interpreter (runtime), with no CPUState dependency. */

/* Main-RAM base + load-delay gate (memory.c). Inlined load helpers use these
 * so MotK VLC / decode hot paths avoid an out-of-line call per LW/LH. */
extern uint8_t *g_psx_ram;
extern int      g_psx_load_delay;
extern int      g_ls_mode;
extern volatile int g_ds_recording;
int psx_load_delay_enabled(void);
uint32_t psx_cyc_load_word_slow(CPUState* cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask);
uint16_t psx_cyc_load_half_slow(CPUState* cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask);

/* Overlay DLLs route timed loads through the host callback shim so they share
 * runtime timing and memory state. Normal generated code keeps the main-RAM
 * fast path inline. Lean draw clones (PSX_DRAW_CTX_BUILD, generated
 * <stem>_draw.c) load from their draw context's private memory with no
 * timing at all (docs/THREADED_CONTEXTS.md). */
#if defined(PSX_DRAW_CTX_BUILD)
#include "psx_dctx_mem.h"
static inline uint32_t psx_cyc_load_word(CPUState* cpu, uint32_t addr,
                                          uint32_t rt, uint32_t reg_mask) {
    (void)rt; (void)reg_mask;
    return psx_dctx_lw(cpu, addr);
}
static inline uint16_t psx_cyc_load_half(CPUState* cpu, uint32_t addr,
                                          uint32_t rt, uint32_t reg_mask) {
    (void)rt; (void)reg_mask;
    return psx_dctx_lh(cpu, addr);
}
#elif defined(PSX_OVERLAY_DLL_BUILD)
uint32_t psx_cyc_load_word(CPUState* cpu, uint32_t addr,
                           uint32_t rt, uint32_t reg_mask);
uint16_t psx_cyc_load_half(CPUState* cpu, uint32_t addr,
                           uint32_t rt, uint32_t reg_mask);
#else
/* Compiled data loads share the exact pipeline update, then read contiguous
 * storage directly. RAM word/half retain their established batched charge;
 * bytes and ROM use the original publishing charge. Scratchpad has no wait. */
PSX_MEM_INLINE void psx_cyc_direct_load_timing(
    CPUState *cpu, uint32_t phys, uint32_t rt, uint32_t mask, int batch_ram) {
    if (g_psx_load_delay < 0) (void)psx_load_delay_enabled();
    if (!g_psx_load_delay) return;
    psx_cyc_base(cpu);
    psx_cyc_deps(cpu, mask);
    if (cpu->ld_which_t == rt) cpu->ld_which_t = 0u;
    psx_cyc_lds(cpu);
    cpu->read_absorb[cpu->read_absorb_which].value = 0u;
    cpu->read_absorb_which = 0u;
    if (phys >= 0x1F800000u && phys < 0x1F800400u) cpu->ld_absorb = 0u;
    else {
        uint32_t cost = phys < PSX_MAIN_RAM_WINDOW_BYTES ? 5u : 2u;
        uint32_t fudge = (uint32_t)((cpu->read_fudge >> 4) & 2u);
        cpu->ld_absorb = cost;
        if (batch_ram && phys < PSX_MAIN_RAM_WINDOW_BYTES) psx_cyc_charge(fudge + cost);
        else psx_cpu_charge(fudge + cost);
    }
    cpu->ld_which_t = (uint8_t)rt;
}
extern uint8_t psx_cyc_load_byte_slow(CPUState*, uint32_t, uint32_t, uint32_t);
PSX_MEM_INLINE uint32_t psx_cyc_load_word(CPUState *cpu, uint32_t addr,
                                            uint32_t rt, uint32_t reg_mask) {
#ifdef PSX_ENABLE_BLOCK_CYCLES
    if (PSX_MEM_LIKELY(!g_ls_mode && !g_ds_recording)) {
        uint8_t *p = psx_mem_pointer(addr, 4u);
        if (PSX_MEM_LIKELY(p)) {
            uint32_t phys = addr & 0x1FFFFFFFu;
            psx_cyc_direct_load_timing(cpu, phys, rt, reg_mask, 1);
            uint32_t value; memcpy(&value, p, sizeof(value));
            if (phys < PSX_MAIN_RAM_WINDOW_BYTES && g_ram_read_watch_active)
                debug_server_trace_ram_read_watch(phys & g_psx_ram_mask, value);
            return value;
        }
    }
    return psx_cyc_load_word_slow(cpu, addr, rt, reg_mask);
#else
    (void)rt; (void)reg_mask;
    return psx_mem_read_word(cpu, addr);
#endif
}
PSX_MEM_INLINE uint16_t psx_cyc_load_half(CPUState *cpu, uint32_t addr,
                                            uint32_t rt, uint32_t reg_mask) {
#ifdef PSX_ENABLE_BLOCK_CYCLES
    if (PSX_MEM_LIKELY(!g_ls_mode && !g_ds_recording)) {
        uint8_t *p = psx_mem_pointer(addr, 2u);
        if (PSX_MEM_LIKELY(p)) {
            uint32_t phys = addr & 0x1FFFFFFFu;
            psx_cyc_direct_load_timing(cpu, phys, rt, reg_mask, 1);
            uint16_t value; memcpy(&value, p, sizeof(value));
            return value;
        }
    }
    return psx_cyc_load_half_slow(cpu, addr, rt, reg_mask);
#else
    (void)rt; (void)reg_mask;
    return psx_mem_read_half(cpu, addr);
#endif
}
PSX_MEM_INLINE uint8_t psx_cyc_load_byte(CPUState *cpu, uint32_t addr,
                                            uint32_t rt, uint32_t reg_mask) {
#ifdef PSX_ENABLE_BLOCK_CYCLES
    if (PSX_MEM_LIKELY(!g_ls_mode && !g_ds_recording)) {
        uint8_t *p = psx_mem_pointer(addr, 1u);
        if (PSX_MEM_LIKELY(p)) {
            uint32_t phys = addr & 0x1FFFFFFFu;
            psx_cyc_direct_load_timing(cpu, phys, rt, reg_mask, 0);
            uint8_t value; memcpy(&value, p, sizeof(value));
            return value;
        }
    }
    return psx_cyc_load_byte_slow(cpu, addr, rt, reg_mask);
#else
    (void)rt; (void)reg_mask;
    return psx_mem_read_byte(cpu, addr);
#endif
}
#endif
#if defined(PSX_OVERLAY_DLL_BUILD)
extern uint8_t psx_cyc_load_byte(CPUState*, uint32_t, uint32_t, uint32_t);
#elif defined(PSX_DRAW_CTX_BUILD)
static inline uint8_t psx_cyc_load_byte(CPUState *cpu, uint32_t addr, uint32_t rt, uint32_t mask) {
    (void)rt; (void)mask; return psx_dctx_lb(cpu, addr);
}
#endif

/* Charge the exact timing/pipeline effects of a 32-bit CPU load without
 * invoking the memory handler.  Enhancement HLE may use this only when static
 * analysis proves the loaded value and read side effects are unobservable. */
extern void psx_cyc_load_word_timing_only(CPUState* cpu, uint32_t addr,
                                           uint32_t rt, uint32_t reg_mask);

/* LWC2 (GTE load) ReadMemory timing only (completion +1, NO LDWhich arm — the dest
 * is a GTE register). Call AFTER psx_cyc_step(cpu,0) (§1+DO_LDS) and psx_gte_stall.
 * Returns the raw 32-bit value. */
#if defined(PSX_OVERLAY_DLL_BUILD) || defined(PSX_DRAW_CTX_BUILD)
extern uint32_t psx_cyc_lwc2_read(CPUState* cpu, uint32_t addr);
#else
extern uint32_t psx_cyc_lwc2_read_slow(CPUState* cpu, uint32_t addr);
PSX_MEM_INLINE uint32_t psx_cyc_lwc2_read(CPUState *cpu, uint32_t addr) {
    if (PSX_MEM_LIKELY(!g_ls_mode && !g_ds_recording)) {
        uint8_t *p = psx_mem_pointer(addr, 4u);
        if (PSX_MEM_LIKELY(p)) {
            uint32_t phys = addr & 0x1FFFFFFFu;
#ifdef PSX_ENABLE_BLOCK_CYCLES
            cpu->read_absorb[cpu->read_absorb_which].value = 0u;
            cpu->read_absorb_which = 0u;
            if (phys >= 0x1F800000u && phys < 0x1F800400u) cpu->ld_absorb = 0u;
            else {
                uint32_t cost = phys < PSX_MAIN_RAM_WINDOW_BYTES ? 4u : 1u;
                cpu->ld_absorb = cost;
                psx_cpu_charge(((cpu->read_fudge >> 4) & 2u) + cost);
            }
            cpu->ld_which_t = 32u;
#endif
            uint32_t value; memcpy(&value, p, sizeof(value));
            if (phys < PSX_MAIN_RAM_WINDOW_BYTES && g_ram_read_watch_active)
                debug_server_trace_ram_read_watch(phys & g_psx_ram_mask, value);
            return value;
        }
    }
    return psx_cyc_lwc2_read_slow(cpu, addr);
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* PSX_CYC_H */
