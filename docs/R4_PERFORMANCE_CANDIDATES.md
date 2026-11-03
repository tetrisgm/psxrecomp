# R4 performance candidates: separate local evaluation

Owner request: evaluate the fork's performance/threading work separately and
keep it on local branches. Tracked by `beads-r4recomp.2`.

All branches below were prepared in `F:/Projects/r4-psxrecomp-recovery` and
are being published, at the owner's request, to the same named branches in
[`tetrisgm/psxrecomp`](https://github.com/tetrisgm/psxrecomp). They remain
**parked candidates**, separate from the recovered game.
The six replayed candidates each start at the recovered game's frozen upstream
engine, `c1f5cafd4102daa57b7342bec7c432aaca8981ce`. None is integrated into
the game. Its engine pin, executable, settings, saves and running owner preview
were left untouched. This is component evaluation; no new gameplay performance
measurements were made during the owner preview.

## Candidates replayed on current upstream

Every branch name in this table starts with `perf/r4-` and ends with
`-20261102`. For example, `render-ring` means
`perf/r4-render-ring-20261102`. Full identities are in
[R4_PERFORMANCE_CANDIDATES.json](R4_PERFORMANCE_CANDIDATES.json).

| Branch middle / head | What was separated | Evaluation and disposition |
|---|---|---|
| `render-ring` / `9ff94310` | Publication batching, cached consumer position, separate cache lines and burst accounting from `f6d573e5` (equivalent topic change `aeb4deaf`). No renderer or draw-context dependencies. | Existing ring test: 8,843 checks, zero failures, both default batching and `PSX_RENDER_THREAD_PUBLISH=1`. Hidden GL output checks pass. **Keep for measured gameplay trial.** |
| `gl-state-cache` / `d820cfb7` | Blend/stencil capability cache from `f6d573e5`. Removed the unrelated host-sampler include and retarget callback, avoiding that API dependency. | Compiles and passes the hidden GL fixture in the combined candidate; output matches with `PSX_GL_CAP_CACHE=0`. The fixture does not qualify R4's full PGXP depth path or launcher context transitions. **Keep for measured and visual trial.** |
| `gl-samplers` / `cc413ceb` | Fixed sampler-unit uniforms once per shader program, replayed from `6eead0f2`, including its fixture addition. | Hidden GL output matches with `PSX_GL_SAMPLER_UNITS_ONCE=0`. **Keep for measured gameplay trial.** |
| `disc-prefetch` / `81862e0a` | The independent-reader, 24-sector read-ahead / 64-slot cache from `ed730388`; omitted sampler/debug-server changes. Includes a small regression test. | Synthetic raw BIN/CUE bytes match the synchronous reader through sequential reads, seeks, cache collisions, EOF and close/reopen. Media overrides and post-read patches stay effective. Default run: 130 hits / 6 misses; disabled run: 0 / 0. **Keep as experimental; CHD, mapped CD-DA, mount lifecycle and useful runtime/I/O benefit need qualification.** |
| `hd-miss-path` / `b7f91c6e` | Allocation-free broad-phase queries and delayed draw leases from `dfc39ed6`, plus the palette-hash early exit from `78ce6940`. | Existing HD pack scan/hash/tracker test passes; changed GPU wrapper compiles. Dropped conflicting fork-only tracking-generation/invalidation-query APIs, which upstream does not use. **Keep for a real HD HUD draw/visual and performance trial.** |
| `render-priority` / `c1523113` | Windows `THREAD_PRIORITY_ABOVE_NORMAL` change from `07dca860`; portable path remains unchanged. | Existing ring transport test: 8,843 checks, zero failures. This does not prove a scheduling benefit. **Park at low priority:** compare guest progress, audio and presentation under controlled host load before considering the higher default priority. |

The disc test is registered as `iso_prefetch_test` and
`iso_prefetch_off_test` on its branch, with assertions enabled in Release.
It uses a disposable generated fixture and fake media hooks; it does not certify
the complete mod system, CHD decompression or the game's audio path.

## Larger threading systems preserved with their original dependencies

These branches retain historical fork snapshots. They are **not rebased
candidates** and must not be merged wholesale into the recovered game.

| Local branch | Frozen head | Assessment |
|---|---|---|
| `perf/r4-threaded-contexts-original-20261102` | `9f34cf894989caf15920936bb19cada4b4f1283a` | Integrated private draw contexts and bounded draw workers. The existing four-worker test passes: payload ownership, backpressure, epochs, expiry and teardown. This is a substantial CPU/GTE/PGXP snapshot and code-generation architecture, with draw-only device/hook restrictions. Its own design record leaves course/GP coverage, hook effects and complete identity/performance gates unresolved. Worker determinism alone cannot establish correct R4 frames. **Preserve for design/reference; no automatic replay.** |
| `perf/r4-presentation-workers-original-20261102` | `84d6ca22259ad16d9dd5e13f390514e575cb8d10` | The game's actual original engine pin, including later packet/presentation scheduling and blocking-wait work. Existing call-gate test passes: ordering, live stack lifetime, refusal, timeout, exceptions and close. The earlier draw pool was deleted in `2e39cbe6` as the design changed. Neither that old pool nor the passing transport test qualifies the later whole renderer. **Keep the original implementation isolated from recovery.** |
| `perf/r4-rewind-worker-original-20261102` | `6cb8b57dc7fda4878067806aa16db5235c570bb7` | Initial pooled rewind/checkpoint publication package. Existing worker test passes: one outstanding job, 1,000 reuses and teardown draining. It depends on the fork's boot-state, native-raster and rewind contracts; its surrounding diff is much larger than a thread helper. Later wait changes are retained in the final presentation snapshot above. **Preserve separately; do not replace upstream rewind merely to recover threading.** |

The old raw `fork/feat/threaded-contexts` ref is an earlier WIP tip,
`27ea6673`; the preserved integrated head above contains the more developed
implementation. The stale "not merged" prose in its design document should
not be mistaken for the actual integration history.

Two other small fork changes, triangle/setup record grouping (`6ea95b0f`)
and display-layout caching (`3c151afd`), remain preserved in the final original
snapshot. The developer's own later Release assessment reported 5.73 versus
5.74 ms/frame with roughly 10% run variation and a sampled ceiling under 1%.
Those are historical, unreproduced figures and support **neutral/inconclusive**,
not a proven win. They are lower priority than the candidates above. Earlier
DuckStation pack changes `008d3d4a` and `7bc8663e` were already covered by
upstream behavior and replayed as empty changes in the recovery work.

## What the new checks establish

`perf/r4-render-checks-20261102` at
`e457f75a92a7650f988c47317ed763b16d8cadd8` combines only ring batching,
the GL capability cache and fixed sampler units. It is a component-check branch,
not an integration recommendation. Individual candidate branches remain separate.

The existing hidden OpenGL fixture passed at fixed 1x and 4x, and dynamic 4x,
with render threading both disabled and enabled. Its four output digests match:
guest readbacks, native VRAM, internal-resolution surface and widescreen surface.
At fixed 4x with threading enabled, disabling each optimization separately,
and then all three together, also preserves those outputs. Ten fixture runs
used 80 synthetic frames each. There were no game launches, screenshots of new
gameplay or frame-rate claims in this follow-up.

The installed MSYS2 static SDL3 needed `-liconv` in addition to the fixture
driver's Windows libraries. The initial link failed, then the same objects
linked successfully with that dependency. No renderer fix was needed for this.

Raw local evidence is under
`../probes/r4-perf-20261102/` (ignored): named test logs, binaries and hashes,
the original GL build receipt, and `gl/rth-62tdx41y/qualified-receipt.json`
with exact commands, environment overrides and output digests. The JSON beside
this document records branch/source identities and the limits of qualification.

## Constructive assessment and next decisions

The small host optimizations have sensible motivations: avoid driver queries
that can synchronize, reduce cross-core publication traffic, avoid allocations
for texture misses, and read immutable disc bytes ahead of demand. Their value
should be judged against today's much faster upstream guest execution, rather
than discarded because the larger fork developed visual problems.

The problem was allowing those small optimizations to travel with much broader
draw isolation, packet dropping, interpolation and checkpoint changes. Evidence
for one layer then became difficult to attribute to the implementation actually
running. A parallel worker test can prove ownership and cancellation while a
title adapter still loses precision, order, device effects or visible geometry.
This review does not establish that threading itself caused the reported holes.

For future work, keep a current upstream baseline, a narrow topic branch and a
written caller contract. Record a measured bottleneck before changing the
architecture, and keep a passing component check separate from a passing game.
Historical FPS, Debug profiles, microbenchmarks and combined-package results
must retain their configuration and attribution limits.

After the owner preview finishes, use isolated candidate builds and disposable
current-build states to compare against this exact recovered baseline. Start
with the small GL/ring package, then HD misses and disc prefetch. Preserve
individual attribution when measuring each branch. Use matched inputs, mods,
resolution, PGXP settings and host conditions, recording raw guest progress,
frame times, CPU cost and audio starvation. Inspect moving screenshots through
straights, bends, tunnels and scene transitions for culling, draw distance,
seams, popping and HUD errors. Disc trials must include CHD and mapped CD-DA;
GL trials must include PGXP depth and launcher/context transitions. A neutral
result is acceptable; a reproducible per-game regression is not. Broader
consumer qualification is required before shared-engine promotion.

Keep these branches separate until those decisions are supported. No performance
candidate is accepted for the game or release on the strength of this component
evaluation. The remaining gameplay qualification is tracked in the open issue.

Retained task data: the source-only `F:/Projects/r4-perf-evaluation` worktree
on the render-check branch, and about 23 MiB of ignored component evidence.
The task's temporary compiler directory is empty. These are retained for the
next qualification step; no extra disc copy or delivery archive was created.
