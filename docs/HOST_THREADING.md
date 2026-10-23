# Host work off the game thread (feat/host-threading, 2026-10-22)

Which host-side stages run on the emulation (game) thread, which moved, and
which cannot move without changing what the guest or the screen sees.
Measured on R4 (Windows, RTX 4080 SUPER, 9800X3D), race savestate, 18x
(`PSX_INTERNAL_RESOLUTION=8k PSX_DYNRES=0`, hidden window).

## What changed (all default on, each with an off switch)

| Change | Thread it helps | Off switch |
|---|---|---|
| GL_BLEND / GL_STENCIL_TEST enable state cached in `gpu_gl_renderer.c` instead of `glIsEnabled` per batch (`depth_restore_ex`). With NVIDIA's threaded optimization each query waited for the driver thread: ~45% of the render thread's wall time at 18x. | render | `PSX_GL_CAP_CACHE=0` |
| Render-thread ring (`render_thread.c`): producer publishes `wpos` every 32 records / 16 KiB (and always before waiting, at frame end, drain, sync point) instead of per record; producer caches `rpos`; positions and each side's counters on separate cache lines; consumer times busy per burst, not per record. Record stream unchanged. | game + render | `PSX_RENDER_THREAD_PUBLISH=1` (per-record publish) |
| Disc read-ahead (`iso_reader_c.cpp`): a worker with its own ISOReader reads the next 24 raw sectors into a 64-slot cache; the game thread copies hits and reads misses itself. Mod extents/patches still applied after the base read. Debug: `cd_prefetch` (hits/misses). R4 race: 4936 hits, 1 miss. | game | `PSX_CD_PREFETCH=0` |
| Diagnostics: `PSX_HOST_SAMPLER_THREAD=render` samples the render thread; `PSX_HOST_SAMPLER_CALLERS=1` charges driver/OS samples to their exe caller (`=2`: every sample's caller); `host_profile` `"phase":4` keeps only GP0 processing. `tools/host_profile.py --callers --phase N`. Pass `--nm` with MinGW's nm on Windows (another nm on PATH mis-bins symbols). | - | debug-tools only |

## Results (release build, 18x, slot 9, accelerate held; mean of 2 runs each)

| Window | base fps | base game-thread ms/tick | base render busy | new fps | new game-thread ms/tick | new render busy |
|---|---|---|---|---|---|---|
| Fly-by (2-17 s) | 36.5 | 11.0 | 95% | 60.3 | 11.8 | 38% |
| Race (20-40 s) | 59.5 | 9.4 | 52% | 60.1 | 7.0 | 16% |

Game-thread ms/tick = `emu_busy` x window / frames (frame report; excludes
pacer and queue waits). In the base fly-by the guest ran at 36 Hz because the
render thread was saturated; it now holds 60 Hz and the game thread is the
next limit (~11-12 ms of 16.7 in the fly-by). Debug build, GP0 phase
(`host_profile phase 4`) in a driven race: ~0.85 ms/tick before the ring
change, ~0.6 after.

Identity: `fp_identity.py` base/base and base/new, 12000 frames
`--headless-opengl`: IDENTICAL, 0 dispatch misses. Race from slot 9: 600
presented-image-ring frames and 64 full-VRAM display-ring frames
byte-identical base vs new. `render_thread_test` passes (also with
`PSX_RENDER_THREAD_PUBLISH=1`). Headless smoke boot-to-race: 0 dispatch /
segment misses.

## Stages that stay on the game thread, and why

- **GP0 parse/record (DrawOTag DMA).** Runs at guest-cycle points during the
  DMA and reads state the guest keeps changing: the PGXP shadow
  (`pgxp_get_precise_vertex`, keyed by packet address, ~20% of GP0 time),
  widescreen hook tags and guest RAM (`psx_ws_prim_is_tagged`,
  `ws_explicit_hud_anchor`, `ws_view_current`, mod predicates, ~30%), and
  GPUSTAT-visible draw state. Deferring it to a worker would read later
  values; capturing them per primitive is most of the cost. Only the
  record overhead was cut (above). Remaining candidates: one record per
  triangle instead of ~6 (dead `set_*(0)` records before each draw), memoize
  `gpu_get_display_info` (pure function of GP1 state, ~6% of GP0).
- **PGXP seam strips.** Not on the game thread: `seam_snap_measure` runs in
  the GL backend, i.e. on the render thread whenever it runs (R4 default).
  ~1.3 ms per guest tick there. Each triangle's measure depends on every
  earlier measured edge and its result decides batch flush points, so a
  parallel or pipelined version would change strip/batch order (not
  identical output) unless the prep stage replicates the backend's gating
  exactly.
- **HD texture decodes.** Already off the game thread: preload uses a
  thread pool; on-demand decodes go to a background worker with native
  fallback until ready (both pack formats); lookups run on the render
  thread. R4 preloads all 36 images: 0 on-demand decodes.
- **SPU / XA.** Guest-clocked and guest-visible (ENDX, ADSR levels, IRQ,
  capture and reverb writes into SPU RAM); `spu_catch_up` ~0.2% and
  `sdl_audio_pump` ~1% of the game thread. XA ADPCM decode feeds the same
  guest-visible path. Host output already crosses to SDL's audio thread.
- **Guest execution** (recompiled code, PGXP hooks, device scheduling) is
  ~90% of game-thread samples; out of scope here.
