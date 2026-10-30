# Restage pool gate, 2026-10-30

Result: FAIL; do not merge the adaptive/performance topic into r4/main.
Replay-rasterization and Windows deadline-wait fixes were already merged and
pushed to r4/main (d89a9248), as authorized separately.

Sequence skips now invalidate enhanced ownership while retaining allocations.
Reacquisition clears color/depth/stencil, including reveal margins, and seeds
native content. Unused allocations retire after eight render retirements.
Commit 455cc48c contains this change and regression coverage.

Back-to-back full 523-segment laps used the same Release executable,
SHA256 908a2358fe387eb877386b12cab1caa9e9eed16f1882c5ef9a672146e12da651,
empty PSX_PGO, hidden windows, dummy audio, PGXP enabled and 60 FPS mode.
Only PSX_SURFACE_REUSE changed (0 to 1). Both booted fresh with
PSX_HLE_SCHEDULER=0, PSX_BIOS_HLE=0, PSX_BIOS_HLE_KEEP_INTRO=1.
The LLE host-fiber scheduler rejects saved-state loads; these are scripted
fresh-boot grids, not byte-identical restored grids, and rewind restoration
is not certified. Background applications were left running. The measured
binary also includes concurrent native texture-residency changes still in the
worktree; those changes were preserved and are not part of this topic commit.

| Metric | Destroy/reallocate A | Pool reuse B |
| --- | ---: | ---: |
| Lap seconds | 129.516 | 129.740 |
| Completed presents/s (runtime counters) | 45.10 | 42.40 |
| Distinct presented images/s (complete trace) | 45.04 | Invalid: truncated trace |
| Starved audio output samples | 0 | 1743 |
| Audio overflow drops during lap | 0 | 0 |
| Held slots / total | 2444 / 7767 | Invalid: truncated trace |
| Process exit | 0 | 0xC0000005 |

B completed the lap and its counters were captured before TCP quit. Its
completed-presentation count is an upper bound on distinct images, so it
fails 58/s independently of the incomplete trace. Do not treat its buffered
trace as a valid acceptance result or attribute the A/B rate difference
solely to pooling under variable background load.

A held-slot attribution: render late 2113 (2057 draw, 56 prepare), no next
endpoint 301, interpolation failed 2, presenter deadline missed 28. These
are render-owner interval classifications on a derived 60 Hz grid, not
physical scanout measurements or exclusive CPU timings. Endpoint interval
was 33.333 ms; interpolation was producing in-betweens. Rejection frequency
does not explain the bulk of held slots. No visible-window comparison was
performed, per the owner's hidden-window-only requirement.

Shutdown evidence: exit origin tcp_quit; access violation in ntdll, with
packet_interpolation_pair / rth_packet_prepare on the captured stack.
handle_quit directly calls exit(0) without stopping the render owner;
global Stream destruction releases interpolation storage. This is a
shutdown lifetime race requiring separate correction, not an accepted run.

Validation: native-boundary fixture 26/26 and production surfaces 88/88
(1x, 4x, 9x, 18x), PGXP enabled. Adaptive audio simulation has zero drops or
starvation for recurring 40 ms scheduling gaps; a forced 250 ms outage
truthfully reports starvation. Device clock +/-1.5% tests, concurrent audio
transport parity, interpolation and interpolation-stream fixtures pass.
These tests do not override the failed busy-PC lap.

Local receipts: analysis/present-guest-clock/restage-lle-full-{a,b},
final-restage-tests, final-production-tests. No generated sources edited.
