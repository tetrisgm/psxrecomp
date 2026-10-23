#ifndef PSXRECOMP_PSX_DCTX_CODEGEN_H
#define PSXRECOMP_PSX_DCTX_CODEGEN_H

/* The lean draw-clone environment (docs/THREADED_CONTEXTS.md, "Lean draw
 * clones"). Included by the generated <stem>_draw.c AFTER its <stem>_decls.h,
 * with PSX_DRAW_CTX_BUILD defined and PSX_ENABLE_BLOCK_CYCLES / PSX_COSIM
 * undefined before any include, so:
 *   - every cycle, I-cache, slice, mult/div and GTE stall call the emitter
 *     guards with PSX_ENABLE_BLOCK_CYCLES is gone;
 *   - loads/stores go to the draw context's private memory (psx_dctx_mem.h,
 *     psx_cyc.h's PSX_DRAW_CTX_BUILD branch);
 *   - the function-like macros below replace, for the clone bodies only, every
 *     runtime service the emitter calls: interrupt checks, debug hooks and
 *     stale-static guards become context operations or nothing; GTE and PGXP
 *     go to the context's own engines; mod hooks are stripped except the
 *     draw-context entry hooks registered with psx_ctx_register_entry_hook;
 *     anything a draw pass cannot do faults the call (fail closed).
 * Main-context code never includes this header. */

#ifndef PSX_DRAW_CTX_BUILD
#error "psx_dctx_codegen.h is for PSX_DRAW_CTX_BUILD translation units only"
#endif

#include "psx_dctx_mem.h"
#include "gte_ctx.h"
#include "pgxp_ctx.h"
#include "ws_cull_edge.h"

#define PSX_DCTX_FAULT_UNSUPPORTED 5
#define PSX_DCTX_FAULT_EXCEPTION   6
#define PSX_DCTX_FAULT_MISS        1

/* ---- control flow / interrupts / debug ---------------------------------- */
#undef  debug_server_log_call_entry
#define debug_server_log_call_entry(a)       ((void)0)
#undef  debug_server_cyc_observe
#define debug_server_cyc_observe(a)          ((void)0)
#define psx_check_interrupts_at(c, pc)       ((void)0)
#define psx_restore_state_escape()           ((void)0)
#define psx_game_text_native_ok(a)           psx_dctx_text_ok(cpu, (a))
#define psx_unknown_dispatch(c, a, p) \
    psx_dctx_fault((c), PSX_DCTX_FAULT_MISS, (a), "unknown dispatch")
#define psx_native_bad_entry(c, owner, pc) \
    psx_dctx_fault((c), PSX_DCTX_FAULT_MISS, (pc), "bad native entry")
#define psx_syscall(c, code)                 psx_dctx_syscall((c), (code))
#define psx_break(c, code, pc) \
    psx_dctx_fault((c), PSX_DCTX_FAULT_EXCEPTION, (pc), "break")
#define psx_arith_overflow(c) \
    psx_dctx_fault((c), PSX_DCTX_FAULT_EXCEPTION, 0u, "arithmetic overflow")
#define psx_unaligned_access(c, a, pc) \
    psx_dctx_fault((c), PSX_DCTX_FAULT_EXCEPTION, (pc), "address error")
#define psx_rfe_mark_escape() \
    psx_dctx_fault(cpu, PSX_DCTX_FAULT_UNSUPPORTED, 0u, "rfe")

/* ---- memory ------------------------------------------------------------- */
#define psx_cyc_load_byte(c, a, rt, m)       psx_dctx_lb((c), (a))
#define psx_cyc_lwc2_read(c, a)              psx_dctx_lw((c), (a))

/* ---- GTE ---------------------------------------------------------------- */
#define gte_execute(c, cmd)                  gte_ctx_execute((c)->ctx->gte, (c), (cmd))
#define gte_write_data(c, r, v)              gte_ctx_write_data((c)->ctx->gte, (c), (r), (v))
#define gte_precision_store_word(a, r)       gte_ctx_precision_store_word(cpu->ctx->gte, (a), (r))
#define gte_nclip_exact_sign(m, pc)          gte_ctx_nclip_exact_sign(cpu->ctx->gte, (m), (pc))
/* gte_read_data / gte_read_ctrl / gte_write_ctrl touch only the CPUState. */

/* ---- PGXP: the context's engine ------------------------------------------ */
#undef PGXP_LOAD
#undef PGXP_STORE
#undef PGXP_ALU
#undef PGXP_MULDIV
#undef PGXP_COP2
#if defined(PSX_PGXP) && PSX_PGXP
#define PGXP_LOAD(i, a, v) \
    (cpu->ctx->pgxp_active ? pgxp_ctx_load(cpu->ctx->pgxp, cpu, (i), (a), (v)) : (void)0)
#define PGXP_STORE(i, a, v) \
    (cpu->ctx->pgxp_active ? pgxp_ctx_store(cpu->ctx->pgxp, cpu, (i), (a), (v)) : (void)0)
#define PGXP_ALU(i, r, s1, s2) \
    (cpu->ctx->pgxp_active ? pgxp_ctx_alu(cpu->ctx->pgxp, cpu, (i), (r), (s1), (s2)) : (void)0)
#define PGXP_MULDIV(i, hi, lo, s1, s2) \
    (cpu->ctx->pgxp_active ? pgxp_ctx_muldiv(cpu->ctx->pgxp, cpu, (i), (hi), (lo), (s1), (s2)) : (void)0)
#define PGXP_COP2(i, v, a) \
    (cpu->ctx->pgxp_active ? pgxp_ctx_cop2(cpu->ctx->pgxp, cpu, (i), (v), (a)) : (void)0)
#else
#define PGXP_LOAD(i, a, v)              ((void)0)
#define PGXP_STORE(i, a, v)             ((void)0)
#define PGXP_ALU(i, r, s1, s2)          ((void)0)
#define PGXP_MULDIV(i, hi, lo, s1, s2)  ((void)0)
#define PGXP_COP2(i, v, a)              ((void)0)
#endif

/* ---- mod hooks: stripped, except registered draw-context entry hooks ---- */
#define psx_mod_function_entry(c, a) \
    ((c)->ctx->entry_hooks ? psx_dctx_entry_hook((c), (a)) : 0)
#define psx_mod_instruction(c, pc, instr)    psx_dctx_note_stripped_hook(c)
#define psx_mod_memory_address(c, pc, instr, ea) (psx_dctx_note_stripped_hook((CPUState *)(c)), (ea))

/* ---- widescreen cull terms: the snapshot's margin ----------------------- */
#define psx_ws_x_margin()                    (cpu->ctx->ws_x_margin)
#define psx_ws_cull_slti(sx, imm) \
    (((int32_t)(sx) < (int32_t)(imm) + psx_ws_x_margin()) ? 1 : 0)
#define psx_ws_cull_slti_lower(sx, imm) \
    (((int32_t)(sx) < (int32_t)(int16_t)(uint16_t)(imm) - psx_ws_x_margin()) ? 1 : 0)
#define psx_ws_cull_bltz(v)                  (((int32_t)(v) < -psx_ws_x_margin()) ? 1 : 0)
#define psx_ws_cull_bgez(v)                  psx_ws_cull_bgez_value((int32_t)(v), psx_ws_x_margin())
#define psx_ws_clip_edge_x(v, w)             psx_ws_clip_edge_x_value((v), (w), psx_ws_x_margin())
#define psx_ws_cull_lui_hi(imm)              psx_ws_cull_lui_hi_value((imm), psx_ws_x_margin())
#define psx_ws_cull_bltz_hi(v)               psx_ws_cull_bltz_hi_value((v), psx_ws_x_margin())
#define psx_ws_xclip_bound(vanilla)          (psx_ws_x_margin() > 0 ? 0x7FFFFFFFu : (vanilla))

/* ---- services a draw pass may not use: fault ----------------------------- */
#define PSX_DCTX_UNSUPPORTED(name) \
    (psx_dctx_fault(cpu, PSX_DCTX_FAULT_UNSUPPORTED, 0u, name), 0)
#define psx_datashard_enter(c, k)            PSX_DCTX_UNSUPPORTED("datashard")
#define psx_datashard_ret(c)                 ((void)PSX_DCTX_UNSUPPORTED("datashard"))
#define psx_vsync_query_hle_enter(c, f, a, b, d, e) PSX_DCTX_UNSUPPORTED("vsync hle")
#define psx_ws_sprite_tag(c)                 ((void)PSX_DCTX_UNSUPPORTED("ws sprite tag"))
#define psx_ws_mmx6_bg_stage_init()          ((void)PSX_DCTX_UNSUPPORTED("ws bg stage"))
#define psx_ws_nclip_branch(pc, i, m, v)     PSX_DCTX_UNSUPPORTED("ws nclip branch")
#define psx_game_option_store(a, v)          PSX_DCTX_UNSUPPORTED("game option store")
#define gte_ws_set_suppress(on)              ((void)PSX_DCTX_UNSUPPORTED("gte ws suppress"))

#endif
