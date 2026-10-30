# Integrated presentation and audio audit, 2026-10-30

Acceptance **FAILS**. Three independently testable defects were fixed, but the
completed integrated lap achieves 49.50 submitted images/s, with 2,303 held
60 Hz slots and 47,073 starved audio samples. The fixes are pushed on topic
branches; they have not been merged into `r4/main`. This is not a completed
60 FPS certification and the measured improvement is not a controlled speedup.

## Findings

### Guest geometry and interpolation

The guest produces approximately 30 geometry endpoints/s (29.79 in the final
lap), while guest VBlanks run at 59.61/s. Interpolation does generate intermediate
images: the final lap's presenter counters advance by 5,789 in-between samples
and 609 endpoints. Only two of 2,517 audited pairs fail matching, with a peak
of two unmatched sources; the render snap counter advances once. This rules
out a reject path firing on half the samples as the explanation for the held
slots. These are different counters with different lifetimes, not a claim that
every sampled image reached physical scanout.

`packet_interpolation_stream_snapped()` refuses partial samples for snapped,
unmatched, ambiguous, invalid, unexplained, failed or missing geometry. That
guard stays intact. The rare aggregate failures were not captured with enough
per-pair detail to attribute each to a particular source or feature. Once-per-
second polling can miss transient unmatched diagnostics. There is no evidence
that rewind or Car Effects systematically triggers this guard in the integrated
configuration. The requested explanation of the historical phase-4-only versus
integrated difference is consequently not fully established.

Two presentation defects were established directly:

* Immutable packet replay could re-enter CPU native raster authority through
  `cpu_raster_required()` when HD native authority was enabled. Exact native
  inputs already belong to the packet; replaying an interpolated presentation
  sample into that shadow is incorrect redundant work. Packet replay now exits
  this policy before the native-authority fallback. The focused regression
  fails six of eleven checks with the old policy and passes eleven of eleven
  with the fix. Direct rendering retains its native-raster obligations.
* Windows presentation deadlines used the timeout of `WaitForSingleObject`.
  Requests of 1, 2, 4 and 8 ms all took approximately 15.5 ms on this host.
  Guest-clock buffering introduces delivery deadlines between packet arrivals,
  making this coarse wait consequential. An owned high-resolution waitable
  timer now supplies the deadline while the retained event interrupts it.
  Measured means were 1.49, 2.50, 4.46 and 8.40 ms respectively. No global timer
  resolution or guest pacing policy is changed.

The earlier CPU trace measured 3.648 s preparing pairs and 12.642 s drawing
979 samples in roughly 20 s, with only 0.526 ms total GPU-slot waiting. The
final lap still shows missed deadlines and late packets. The above defects
are real, but these observations do not establish that they explain every
remaining missed slot.

### Hidden presentation

No fixed hidden-window 30 Hz swap cap was observed. A quieter short run had
0.53 ms average / 4.07 ms maximum swap duration without queue waits or refusal.
The final busy lap had 7.35 ms average / 103.42 ms maximum, zero refused
presents and zero present queue waits. The Windows deadline defect exists
outside SDL/GL and was reproduced in a standalone fixture.

A visible-window comparison was not performed: the owner requires hidden
windows only. Thus equivalence to visible presentation is **not proven**.
GPU/driver scheduling under background load remains an unresolved contributor.

### Audio clock and occupancy

The previous default target was 180 ms, so 200+ ms occupancy was partly an
explicit policy, not just accidental queue growth. Queue-only proportional
rate correction saturated at +/-0.5%. A 20 s observation measured about
44,182 generated samples/s versus 43,798 callback samples/s: approximately
0.88% discrepancy, beyond that authority. SDL's dummy callback cadence uses
integer-millisecond waits; the nominal device rate need not equal its actual
wall-clock consumption rate.

The callback-owned transport now measures device consumption against monotonic
time in half-second-or-longer windows, filters that independent clock estimate,
and applies the existing bounded queue feedback around it. The default target
is 50 ms. Feedback accounts for the callback's half-buffer so the target means
midpoint occupancy. Guest SPU production and guest timing are unchanged.
Backward timestamps and long callback gaps reset the measurement window.

Simulation of +/-1.5% device skew with alternating +/-1 ms callback jitter
passes with zero overflow/starvation; post-pull fill settles around 38-39 ms,
corresponding to approximately 50 ms midpoint fill. A live 20 s run reached
45.99 ms mean occupancy, 33.3-64.7 ms sampled range, and zero drops/starvation.
The complete lap has zero new overflow drops but starvation and wide occupancy
excursions. The small stable buffer requirement is therefore not certified for
the integrated workload. Boot/turbo counters are excluded using before/after
deltas; lifetime drops are not falsely reported as lap drops.

## Measurements and limitations

All valid audit runs use Release Clang, PGO disabled, LLE, full PGXP,
`SDL_AUDIODRIVER=dummy`, a hidden window, defaults plus explicit 60 FPS, and
the same restored fresh-grid state. Feature arms run for 20 s. Car Effects,
rewind, guest-clock buffering and phase-3 surfaces were each disabled separately
against the corresponding current control. Code-fix runs are separately named;
they are not single-feature bisect arms. Rewind and effects removal did not
restore 60 FPS; legacy surfaces and clock bypass also failed to do so.

| 20 s observation | Images/s | Mean audio ms | Starved samples |
|---|---:|---:|---:|
| Current non-PGO baseline | 51.59 | 225.36 | 0 |
| Car Effects disabled | 51.66 | 238.23 | 0 |
| Rewind disabled | 48.90 | 253.64 | 0 |
| Audio clock fix | 54.70 | 45.99 | 0 |
| Guest-clock bypass | 45.02 | 78.13 | 22,858 |
| Legacy surface path | 29.51 | 77.43 | 19,587 |

Host load was not controlled. The runtime/compiler exclusion lock missed media
processing. With the game stopped, the machine still used about 63% of 16 logical
CPUs; mediamediad and three ffmpeg processes consumed roughly seven CPU cores,
while GPU utilization was 37-54%. Final-lap CPU samples ranged from 50.7% to
100%, with media processes present. Those processes were not started by this
audit and were not stopped. The harness now records background CPU/media load.
Its older `interference: []` field only excludes competing game/compiler
processes and must not be interpreted as a quiet-host claim. The feature table
cannot establish a reliable causal performance ranking under this load.

| Full-lap metric | Reported starting lap | All fixes, non-PGO |
|---|---:|---:|
| Duration, seconds | 129.627 | 129.968 |
| Submitted images/s | 31.30 | 49.50 |
| Consecutive identical images | 0 | 1 |
| Held slots / derived 60 Hz slots | 3,850 / 7,776 | 2,303 / 7,795 |
| Guest VBlanks/s | 60.018 | 59.607 |
| Audio mean occupancy, ms | 211.91 | 62.52 |
| Audio sampled range, ms | 161.0-274.1 | 0.4-177.9 |
| Audio overflow drops | 1,088 | 0 |
| Audio starved samples | 0 | 47,073 |
| Estimated input budget, ms | 50.02-83.35 | 50.02-83.35 |

Full-image hashes resolve completely. The final distinct submission rate is
49.497/s after subtracting the one consecutive repeat. Held slots are projected
from timestamped submissions onto a 60 Hz grid, not measured physical scanout.
The input budget is an analytical estimate, not measured input-to-photon latency.
It remains unchanged because video buffering and endpoint delay remain intact.
Not all held slots have been attributed to genuine guest stalls.

The final lap completed all 523 course segments, had zero dispatch misses and
verified 25 subsequent rewind restores with zero differences or missing packet
replays. PGXP remained enabled. Initial stale-GCC preflights and an interrupted
lap are invalid and excluded. The starting lap's different build conditions and
uncontrolled host load prohibit attributing the before/after difference entirely
to these fixes. PGO is not enabled in the audit build; no clear benefit was
established here.

## Tests and reproducibility

* 24/24 permitted R4 CTests pass. The PGXP-off boot test is excluded.
* Immutable packet GL fixture: 915 checks, zero failures, hidden/full PGXP.
* Native raster boundary fixture: 11 checks pass; old policy fails six.
* Audio transport/reference parity and concurrent-wrap tests pass.
* Audio clock skew/jitter tests pass with zero drops/starvation.
* Presenter and interpolation-stream fixtures pass.
* Wake fixture passes retained/coalesced notification, interrupt, cancellation,
  deadline-not-early, and Windows short-wait resolution checks (1.52 ms mean).

Framework topics forked from `74219cbc`:

* `fix/integrated-presentation-rate`: `37df03e9`.
* `fix/audio-device-clock`: `5d854bb1`.
* `fix/presentation-deadline-wake`: `b785a801`.

Combined validation branch: `audit/integrated-presentation-validation`.
R4 harness: `tools/audit_integrated_presentation.py`; invoke with
`--build build-clock-clang --name NAME --port 4868 --seconds 20 --load-grid`.
Omit `--seconds` for the complete circuit. `--load-grid` requires the local
baseline slot; without it the harness boots a fresh grid. Feature switches are
`--no-rewind`, `--disable r4.enhancement.car-effects/car-effects`,
`PSX_PRESENT_GUEST_CLOCK=0`, and `PSX_GL_LEGACY_SURFACE_TEST=1`.
Check the actual mod identifier in the staged catalog before reproducing a
feature arm. Restore default environment between arms. The harness records
executable hash, framework revision, configuration, counters and load samples.

Compact counter receipts are in `INTEGRATED_PRESENT_AUDIT_RECEIPTS.json`.
Full local traces, screenshots and private saves remain under
`analysis/present-guest-clock/` in the audit R4 worktree. No proprietary guest
assets or saves are committed. Existing generated source changes were preserved.

Next acceptance work requires a comparable quiet-host control/feature matrix
and a complete lap passing all gates. Do not merge these topics as an asserted
60 FPS fix based on this report.
