# Threaded draw contexts

Status: **work in progress** (branch `feat/threaded-contexts`, not merged into
`r4/main`). This file is the design, the inventory it rests on, what works,
what was measured, and the exact remaining steps.

Current qualification (2026-10-23): `7b11578f` handles the exact live OpenBIOS
A0 memcpy/memset bodies with private memory and matching register/PGXP effects.
`tools/test_dctx_bios.py` compares both against unchanged generated BIOS bodies,
serially and on four threads. Course closure now runs in R4; complete Grand Prix
coverage remains in progress. `9939ae48` makes `ctx_draw full=1` compare the first
main execution's GPU words and immutable precision receipts with the private run;
later repetitions are timing only. This is separate from whole-race isolation.
Snapshots now include GTE presentation config and live PGXP RAM shadows, and GPU
receipts preserve precision at consumption time (`a4982b8d`, `d842fc4f`, `8cac15a9`).
Older starting-point measurements below are historical, not current validation.

## Goal

Run a game's recompiled *draw* functions on several host threads at once
(draw-only passes for the real frame and for interpolated 60/120/144 Hz
frames, later split track rendering) while game logic keeps running on the
main thread. The main guest context must keep byte-identical behaviour and
performance.

## Design in one paragraph

The main context is untouched: same generated code, same globals. A draw pass
runs **lean draw clones**: the generator emits, for the `[threaded_draw]`
roots of `game.toml` and their static callee closure, a second copy of each
function (`dctx_func_XXXXXXXX`) into `generated/<stem>_draw.c`. That file is
compiled without `PSX_ENABLE_BLOCK_CYCLES` / `PSX_COSIM`, with
`PSX_DRAW_CTX_BUILD`, and every runtime service the emitter calls is remapped
by `runtime/include/psx_dctx_codegen.h`, so the clones carry no cycle,
I-cache, slice, interrupt, debug, cosim or mod-hook code at all, and every
piece of state they touch hangs off `cpu->ctx` (a `PsxExecCtx`, appended to
`CPUState`; NULL on the main context). A context is reset from an immutable
**snapshot** of the main machine and then owns private RAM, scratchpad, CPU
and GTE registers, a GTE side state, a PGXP engine, DMA/GPUSTAT/INTC/timer
registers and a GPU **command capture**. Any number of contexts can run
concurrently from one snapshot; a context never writes main-context state.

## Inventory: process globals the recompiled code touches, and what a draw clone does instead

| Main-context state (where) | Draw clone |
|---|---|
| Main RAM `g_psx_ram`, inline loads in `psx_cyc_load_word/half` (psx_cyc.h) | `psx_dctx_lw/lh/lb` on the context's private RAM copy (psx_dctx_mem.h) |
| Stores via `cpu->write_*` → `psx_write_*` (memory.c: dirty/text/overlay guards, PGXP untracked-store, render-pass journal, lockstep hooks) | emitter rewrites to inline `psx_dctx_sw/sh/sb`; `cpu->write_*` pointers of a context CPUState are per-slot thunks (16 slots) for the inline lwl/swl helpers |
| Scratchpad (memory.c static) | private 1 KB copy, inline |
| BIOS ROM | shared, read-only |
| MMIO: GPU GP0/GP1 (gpu.c), DMA (dma.c), I_STAT/I_MASK, timers, SPU/CD/SIO/MDEC | GP0/GP1 writes and GPU DMA (block + linked list) captured with source addresses; OTC DMA fills private RAM; DMA/DICR/DPCR/GPUSTAT/INTC private copies; timer reads = snapshot + 1 per 64 reads; other device writes dropped and counted; other device reads **fault** |
| Cycle model: `psx_cyc_step`, `psx_cyc_charge`, batch/defer globals, `psx_icache_fetch`, `psx_slice_block`, mult/div + GTE stalls (psx_cyc.h, psx_cycles.c, psx_icache.c) | compiled out (`PSX_ENABLE_BLOCK_CYCLES` undefined in the draw TU) |
| Interrupt checks `psx_check_interrupts_at` → `psx_check_interrupts` → **savestate_poll / debug_server_poll / netplay / rewind** (interrupts.c) | no-op |
| `g_debug_last_store_pc` breadcrumb, `debug_server_log_call_entry`, `debug_server_cyc_observe`, cosim | removed (emitter strips the store-PC write; macros) |
| GTE side state in gte.cpp: projection caller RA, NCLIP provenance, geometry-correction cache, frame-gen sources, projection/INTPL rings, dome probe, ws SZ stats, last-projection globals, counters | `PsxGteCtx` (gte_ctx.h): own caller RA + NCLIP state and position-fallback cache; PGXP routed to the context's engine. Presentation configuration (widescreen squash, FOV scale, render pose) is frozen in the snapshot; host stats/rings skipped |
| PGXP engine (pgxp.cpp): RAM/scratch/register shadows, generation, stats, rings, render-pass journal | the same engine body instantiated per context (`pgxp_engine.inc` included into `struct PgxpCtxEngine`, pgxp_ctx.cpp); config + register/scratch shadows imported from the snapshot, RAM shadow starts empty (O(1) generation bump) |
| Mod hooks `psx_mod_function_entry`, `psx_mod_instruction`, `psx_mod_memory_address` (mod_runtime.cpp) | stripped (counted), except entry hooks registered with `psx_ctx_register_entry_hook` |
| Stale-static text guard `psx_game_text_native_ok` (live text bitmaps) | `psx_dctx_text_ok`: exact compare of the clone's code ranges against the compiled reference image, on the context's RAM, cached per row |
| Widescreen cull helpers (gpu.c, read `psx_ws_x_margin()`) | margin captured in the snapshot; slti/bltz/bgez/clip_edge/lui_hi/bltz_hi/xclip inline; other ws helpers still call gpu.c (read-only config) |
| Data shards, vsync HLE, ws sprite tag / bg stage / nclip branch, game-option store, `gte_ws_set_suppress`, syscalls other than Enter/ExitCriticalSection, break/overflow/address error, BIOS calls | **fault** (fail closed) |

## Main-context identity (why main behaviour cannot change)

* Generated main code is unchanged: regenerating R4 on this branch changes only
  line endings of `<stem>_full_NN.c` / `_dispatch.c` / `_decls.h`; the new
  output is the separate `<stem>_draw.c`.
* `pgxp.cpp` now includes `pgxp_engine.inc` with `PGXP_S=static`,
  `PGXP_API=extern "C"`: the object code of the main engine is byte-identical
  to before (objdump compared) apart from the appended `pgxp_main_export`.
* gte.cpp: the main path gains `gte->ctx` tests (always NULL on main) and the
  NCLIP state moved into a struct; behaviour identical.
* gpu.c / dma.c: pure peeks added (`gpu_peek_gpustat`, `dma_peek_channel`);
  `gpu_read_gpustat` now returns `gpu_peek_gpustat()` after its poll logic.
* `CPUState.ctx` appended at the end (offsets unchanged).
* Note: cpu_state.h / gte.cpp / pgxp.cpp / pgxp_engine.inc are in the
  execution contract (`PSX_EXECUTION_COOKIE`), so **savestates made by an
  older build do not load** in this one (expected for any engine change).

## The prior "skip the cycle model" breakage, explained

Savestate application, the debug-socket pump, netplay snaps and rewind are all
serviced from `psx_check_interrupts` (interrupts.c, `savestate_poll(...)` on the
fast paths every 0x4000 block edges), which compiled code reaches through
`psx_check_interrupts_at` at every block exit, and device time advances from
the cycle charges. A main-context build that stripped those calls never
reached a service point, so a staged load never applied. The lean clones strip
them only in the draw TU; the main context keeps them.

## Code map

* `recompiler/src/draw_ctx_emitter.{h,cpp}` — closure (direct calls, alias
  bodies, constant CPS transfers and constant links), clone text transform
  (rename, stores → context, drop store-PC, `PSX_DCTX_TICK()` at every block
  label), sorted row table with code ranges, self-registering constructor.
  Called from `main_psx.cpp`; config `[threaded_draw] roots / exclude /
  max_functions` (config_loader).
* `runtime/include/psx_exec_ctx.h`, `runtime/src/psx_exec_ctx.c` — snapshots,
  contexts, trampoline (`psx_ctx_call`), slow memory path, DMA, capture,
  faults (longjmp), budgets, fault trail.
* `runtime/include/psx_dctx_mem.h`, `psx_dctx_codegen.h` — clone environment.
* `runtime/include/gte_ctx.h` + gte.cpp; `runtime/include/pgxp_ctx.h`,
  `runtime/src/pgxp_ctx.cpp`, `pgxp_engine.inc`, `pgxp_engine_types.h`.
* `runtime/src/psx_exec_ctx_bench.c` + debug command `ctx_draw`
  (debug_server.c): `{"cmd":"ctx_draw","calls":"fn:a0,a1;fn","writes":"addr=val;...",
  "threads":4,"reps":1,"full":0,"sp":"0x..","dump":"path"}` runs the sequence
  once serially and on N threads from one snapshot, reports per-run status,
  word count, hash, ms, fault PC/RA and the last 16 dispatched PCs, and
  `identical`. `full=1` times the same calls on the main context (zero guest
  time, RAM restored; throwaway session only).
* `runtime.cmake` links `<stem>_draw.c` when it exists beside the dispatch file.

## State (2026-10-22)

### Follow-up (2026-10-22, PC/Mac)

* `743f8e6f`: A0:2A memcpy runs on private context memory after matching the
  live vector, dispatch stub, table destination and complete OpenBIOS routine
  bytes. Patched/unknown code fails closed. Zero-length, unaligned, overlap,
  RAM aliases, scratchpad and budget cases compare against the actual
  recompiled BIOS function, serially and on four threads (PC and Mac pass).
  `83d0114d` also compares the exact PGXP hook event stream to that reference.
* The minimal ClearOTagR x2 / PutDrawEnv / DrawOTag sequence now completes,
  with serial and four parallel context command streams identical. This is
  context determinism, not a stock-frame identity result.
* `acf03128`: registered emitter closure/transform tests cover return links,
  unknown targets, bounded closure and private stores; fixed removal of the
  shared store-PC breadcrumb when no space follows its semicolon. PC and Mac
  CTests pass. BIOS regenerated; native BIOS C unchanged, stamp refreshed.
* `a4982b8d`: GTE presentation config is captured per snapshot. Four workers
  remain identical while the main thread changes projection config (PC/Mac).
* R4 context probe: `tools/draw_ctx_check.py --seq min|course|gp`. The first
  course miss was `0x80061078`, called from `0x80060F94`; this root plus the
  GP sequence produces 322 clone bodies / 2,987 rows. Main generated C is
  unchanged. Full course/GP completion and full-code/stock identity remain
  unverified; do not merge on command-stream parallel equality alone.

The earlier notes below describe the starting point, not the remaining BIOS
blocker. The active remaining work is course/GP coverage, complete PGXP
snapshot/command provenance, mod hook effects, and identity/performance gates.

Built and run on the PC against R4 (r4/main + this branch, local integration
branch `int/ctx-r4`, never pushed):

* R4 runtime with draw clones builds (MinGW GCC, no warnings in the draw TU);
  headless smoke `tools/smoke.py` to race: **0 dispatch misses**.
* R4 roots used (game.toml, not committed in R4):
  `0x8006F00C, 0x80093418 (ClearOTagR), 0x80093520 (DrawOTag), 0x80093590
  (PutDrawEnv)` plus the libgpu device table at `*0x800ABC70 = 0x800ABC30`
  (`0x80094D7C 0x80094DA0 0x800945A0 0x80094C8C 0x80094CC4 0x80094D04
  0x80094A0C 0x800947D0 0x80095050 0x80094CB0 0x800944C0 0x80094D4C
  0x800952B0 0x800944A8 0x80095400`) → 111 clones, 647 rows, 52.9k lines.
* From a race savestate, `ctx_draw` with the sequence ClearOTagR ×2 →
  PutDrawEnv → DrawOTag: both ClearOTagR (OTC DMA) complete and PutDrawEnv
  gets as far as a GPU DMA + GP1 write (7 words captured), then faults:
  **miss at 0x000000A0** (a BIOS A-table call from 0x80096620, ra
  0x80093634). The serial run and 4 concurrent contexts produce **identical**
  results (status, 7 words, hash `500e6c6cf5a02b1e`).
* Measured: snapshot 0.3 ms (2 MB RAM copy + PGXP registers); lean run of
  that partial sequence ≈0.5 ms per context including `psx_ctx_begin`
  (2 MB copy); 4 contexts in parallel 1.1–3.6 ms wall. Not yet measured: full
  vs lean for a complete draw, main-context speed A/B, `fp_identity`.

## Remaining steps, in order

1. **BIOS calls from draw code.** libgpu reaches the BIOS A-table (0xA0;
   from 0x80096620, likely memcpy/bzero for the DRAWENV packet). Either clone
   the recompiled BIOS functions draw code uses (OpenBIOS dispatch table, a
   small allow-list in `[threaded_draw]`) or map the handful of pure A0/B0
   string/memory calls to context implementations in psx_exec_ctx.c. Then
   rerun `ctx_draw` until ClearOTagR/PutDrawEnv/DrawOTag complete.
2. Add the course call `0x8006F00C` (`--seq course` in the check script; it
   also needs `0x1F800078 = *0x800F2C14`, done by the script) and then the
   full GP sequence of `R4/src/mods/r4_interp.c seq_grand_prix`; add roots
   for every miss the trail reports (indirect jalr targets). Watch the
   `block budget` fault: it means a guest wait loop (VSync/DrawSync) — find
   who calls it from the trail and fault_ra.
3. **Unit test** (runtime/tests): a hand-written generated-style draw
   function in a `PSX_DRAW_CTX_BUILD` TU with a tiny table, run on 4 threads
   from one snapshot and serially, assert identical capture; plus emitter
   test for `draw_ctx_clone_text` and the closure.
4. Measurements: `ctx_draw full=1 reps=20` per-draw ms lean vs full on the
   race state; main-context speed A/B (headless turbo frames/s, baseline vs
   this branch, several runs); `psxrecomp/tools/fp_identity.py` baseline vs
   this branch (same seed, 12000 frames).
5. Regenerate R4 with the roots, commit `generated/SLUS_007.97_draw.c` and the
   `[threaded_draw]` block in R4; bump the pin after r4/main merge.
6. Merge into fork `r4/main` only after 4. passes (gte.cpp conflicts with
   r4/main's last-projection globals: keep both, gate the globals with
   `side_off`; resolution is in local `int/ctx-r4`).
7. Follow-ups: incremental (dirty-page) snapshots and copy-on-write context
   RAM; carry the PGXP RAM shadow into the snapshot for words projected before
   the pass; per-context render pose / widescreen config; draw-context
   versions of the R4 draw-pass plugin hooks; GPU-side consumer of
   `PsxCtxGpuWord` streams (other agent's gpu work) using
   `psx_ctx_precise_vertex`; worker pool; overlay (R4.BIN) code is not cloned
   (static EXE only).

## Gotchas

* Emitted links: a call's return address can be a *separate* compiled
  function (the emitter split after the jal); the closure follows constant
  `$ra` links, not only CPS targets.
* `VSync(-1)` re-reads Timer 1 until two reads agree; a per-read counter
  advance livelocks it (hence the 1-per-64-reads model).
* Intra-function loops never return to the trampoline; only the per-block
  `PSX_DCTX_TICK` budget (default 50 M blocks per call) stops them.
* The debug command runs on the emulation thread; the live `$sp` there may be
  the kernel's — the bench uses `0x801FFF00` unless `sp` is given.
* Old savestates do not load across this change (execution cookie).

### Geometry fallback snapshots

Draw snapshots retain the current main geometry-cache entries and frozen PGXP
position-fallback setting. Each context copies these into its private sparse
cache and records RTPS/RTPT projections with the same first-writer, ambiguity,
and saturation rules as main. Main render-pass checkpoints preserve fallback,
so disabling it in workers would change precision. The first capture scans the main table to initialize a live-slot index; later
captures visit only that index. Index allocation loss falls back to a full scan.
Checkpoint rollback restores its baseline index prefix, including generation
changes, so speculative projections cannot escape into later snapshots.
`PSX_CTX_GEOMETRY_INDEX=0` retains the full-scan comparison path. A Windows
synthetic corpus with 4,096 live entries and 100 captures measured 2.472 ms
per capture before indexing and 0.600 ms after (including the first scan);
these are cache microbenchmarks, not frame-rate claims. Allocation
failures reject the snapshot or draw pass. The four-thread GTE fixture checks
fallback while main mutates its cache, new projections, ambiguity, and reset.

### Candidate course-draw partition (not implemented)

For R4, local disassembly shows `0x8006F00C` is a 100-byte wrapper.
`0x8006EB58` builds count/block pointers at `0x8010E3C0` (pointer at
`0x8010E7C0`, block stride 80 from `0x800F3064`); setup then configures
scratch/GTE before `0x80060F94` consumes the list. That post-setup boundary
is a candidate for private list slices, not a proven independent partition.
The renderer chain uses `0x800A2454`/`0x800A2478`, iterates primitive type
first and blocks second, and mutates shared scratch, GTE, packet cursor and
OT heads (e.g. `0x80061494..0x800614AC`). Workers require private output
arenas and a stable type/block/primitive merge, not concatenated block output.
Position-fallback ambiguity spans the complete serial draw: separately valid
worker cache entries can become ambiguous together. An ordered geometry-event
reduction or equivalent two-phase design must preserve first-writer semantics.
No partition should be enabled before complete receipt equality, including
precision and OT ordering, is demonstrated.
