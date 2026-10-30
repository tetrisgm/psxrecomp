# Integrated presentation acceptance: surface restaging, 2026-10-30

Acceptance **FAILS** after the requested one additional fix and retry. No merge
or push to `r4/main` was made. Tested source is pushed on
`fix/pooled-restage-validation` through `58e0a03c`.

## Implementation

Restage invalidates canonical ownership while retaining allocated GL surfaces.
Reacquisition clears stale colour, depth and stencil before seeding native data.
Recently used surfaces survive retirement for eight draw cycles; obsolete
allocations still retire. Immutable HD replacement images retain their cache
across packet restaging, with pack generations remaining part of identity.

The first fresh-grid lap still attributed 2,526 of 3,533 held slots to rendering,
including 2,138 during drawing. Restage itself was only 263 ms total across
1,104 calls. The one further fix avoids redundant native texture uploads using
exact CPU byte comparisons against known resident row spans. Disjoint spans
never certify gaps. GPU raw writes, transaction restores, texture creation and
async target transitions invalidate residency. Guest/native authority and PGXP
are unchanged. Upload traffic fell from 16.08 GB / 118,118 calls to 7.79 GB /
61,308 calls, but the lap cadence did not improve. Different host load prevents
claiming a controlled performance speedup or regression.

## Full integrated laps

Both runs booted new private saves to a fresh grid (no `--load-grid`), drove all
523 segments at real speed, and retained R4 defaults plus explicit 60 FPS,
rewind, Car Effects and full PGXP. Both used hidden windows,
`SDL_AUDIODRIVER=dummy`, Release Clang, no PGO and LLE. Output was 5120x2880,
9x internal scale / 2160 lines. Boot/menu turbo was outside measurement;
turbo was disabled before countdown and throughout the measured lap.

| Metric | Pooled restage | Native residency retry |
|---|---:|---:|
| Duration (s) | 130.993 | 129.850 |
| Course segments | 523 | 523 |
| Submitted images/s | 35.483 | 35.071 |
| Consecutive identical submitted images | 4 | 1 |
| Starved audio output samples | 32,441 | 19,032 |
| Audio overflow drops | 0 | 0 |
| Guest VBlanks/s | 59.682 | 59.746 |
| Held slots / derived 60 Hz slots | 3,533 / 7,858 | 3,601 / 7,790 |
| Render-late held slots | 2,526 | 3,032 |
| Draw / prepare held slots | 2,138 / 388 | 2,562 / 470 |
| No-next-endpoint held slots | 988 | 553 |
| Audio occupancy min / mean / max (ms) | 8.0 / 112.94 / 217.4 | 10.5 / 105.78 / 199.5 |
| Host CPU min / mean / max (%) | 45.3 / 66.89 / 100.0 | 48.4 / 71.29 / 99.8 |
| Verified rewind restores / differences | 20 / 0 | 21 / 0 |
| Dispatch / segment / rewind packet misses | 0 / 0 / 0 | 0 / 0 / 0 |

The gate requires zero starvation, zero drops and at least 58 images/s. Only
zero drops passed. Rendering remains the leading held-slot category. These are
submission timestamps and a derived display grid, not physical scanout or an
audible-device validation. Background applications were left running. The
runtime exclusion lock serialized game/build activity; it does not imply a
quiet host. Full-image hashes resolved completely in both laps.

## Verification and provenance

- Hidden native-boundary/restage/residency fixture: 26 checks, zero failures.
  Tests poison all attachments, invoke real restage, check exact allocation
  reuse and stale-margin clearing, and verify byte changes, gaps and GPU-write
  invalidation of native residency.
- Production surface lifecycle: 88 checks across 1x/4x/9x/18x, zero failures;
  verifies detached ownership, retained storage and bounded retirement.
- R4 CTest: 24/24 permitted tests pass; PGXP-off boot fixture excluded.
- Audio exact transport/concurrent wrap, +/-1.5% device skew and adaptive jitter
  fixtures pass. The adaptive fixture also confirms prolonged outages remain
  visible in counters rather than being hidden by re-priming.
- Rebuilt runtime SHA-256 for retry:
  `908a2358fe387eb877386b12cab1caa9e9eed16f1882c5ef9a672146e12da651`.

Local complete receipts are under `analysis/present-guest-clock/` in the R4 audit
worktree: `acceptance-restage-fresh`, `acceptance-residency-retry`,
`native-residency-tests`, `production-reuse-tests`, and `audio-retry-tests`.
Compact counter receipts accompany this document.

Another task was actively editing the requested worktree. Its source changes
were preserved, and validated commits were created in the separate framework
worktree `psx-wt-restage-validation`; the original branch was not switched.
Byte equality of the modified source files with commit `58e0a03c` was checked
before the retry. Harness provenance consequently records original HEAD
`111b3824` plus dirty status; that is not the committed validation branch HEAD.

## Commits and integration decision

- `111b3824`: actual audio device consumption tracking (existing base).
- `95bffecf`: adaptive audio reserve and regression fixtures.
- `6032f3a8`: held-slot tracing, retained interpolation scratch and sampler switch.
- `58e0a03c`: pooled restage, native upload residency and GL regression tests.

The remote fork `r4/main` was already `d89a9248`, containing replay-rasterization
`37df03e9` and Windows deadline-wait `b785a801` before this task. It was left
unchanged because acceptance failed. No further tuning lap was run after the
single requested retry. The historical audit is available at commit `b01dcebf`;
`PRESENT_GUEST_CLOCK.md` retains its earlier baseline/PGO findings.
