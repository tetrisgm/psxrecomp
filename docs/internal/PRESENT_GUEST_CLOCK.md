# Guest-clock presentation

Packets carry `guest_time_ns`, stamped by the producer at the scheduled guest
VBlank edge. History copies preserve it; epoch invalidation discards the
consumer buffer before preparing another pair. These are host-only packet
envelopes, not additions to serialized guest state.

`PacketDeliveryClock` anchors guest time once to the monotonic host clock.
Arrival jitter does not move that mapping. An eight-packet consumer buffer
retains immutable leases until their delivery deadlines, before collecting
geometry and pairing Car Effects metadata. The presenter samples those pairs
on one persistent display-rate grid and skips expired display deadlines.

`PSX_PRESENT_DELAY_MS` configures the extra jitter buffer: default one guest
VBlank (about 16.7 ms NTSC), range 0-100 ms. Existing endpoint interpolation
also incurs one geometry interval (about 33.3 ms for a 30 Hz title). This is
presentation latency only: guest pacing, native GPU results, SPU output and
input execution are unchanged. Buffering is not adaptive or unbounded. The first
exact endpoint after a reset displays immediately: repeatedly restored rewind
epochs cannot be stranded behind a delay longer than their lifetime. Subsequent
forward samples use the fixed mapping.

If an endpoint misses the buffer, the last image holds; unverified geometry
is never extrapolated. A 100 ms overrun, backwards timestamp, epoch change,
rate change or buffer overflow resets interpolation. Sampled guest time never
moves backwards within an uninterrupted timeline. Load/rewind epoch invalidation and shutdown
release all retained envelopes; pause retains the final endpoint. Audio keeps its existing callback-owned ring
and drift correction; the added video offset stays explicit and bounded.

## Verification

- Presenter tests compare complete output vectors for regular arrivals and
  periodic 25 ms spikes, using 33.3 ms buffering. The 16.7 ms default cannot
  fully conceal a 25 ms producer outage.
- A 70 ms outage test verifies monotonic sampled guest time through holds.
- Clock tests cover late arrivals without remapping, long-stall resync,
  epochs, reversed timestamps and delay bounds.
- Mailbox tests verify original guest stamps survive rewind history replay
  even when the transport sequence is replaced, plus existing lifetime,
  concurrency and bounded-allocation checks.
- Sampling tests retain the 60/90/120/144 Hz and immutable metadata coverage.

## Integrated baseline and method

A fresh-grid, scripted full circuit (523 course segments) used the R4 defaults,
explicit 60 FPS, rewind, Car Effects, full PGXP, LLE implementations and
`SDL_AUDIODRIVER=dummy`. The runtime used `--hidden-window` at real speed.
Default detected rendering was 9x / 2160 internal lines, 5120x2880 output.
No runtime/build interference was detected during the lap. The instrumented
training lap and the final PGO rerun use the same route and settings.

The baseline completed in 129.627 seconds: 60.018 guest VBlanks/s, 31.298
presented images/s, zero consecutive identical submitted image hashes, zero
starved audio samples, and a 35.132 ms maximum guest interval. Projecting the
actual presentation trace onto a 60 Hz output grid gives 3,850 held slots out
of 7,776. These missed display opportunities count as repeats for the requested
PGO trigger; an all-distinct set of swaps alone is not a 60 FPS result.

The present trace hashes every pixel using GPU block hashes, then resolves
asynchronously. The CSV is analyzed after clean process exit so its buffered
records are complete. `timeline.json` aligns presentation events, input
requests and audio/presenter samples to the same monotonic host clock.
The derived 60 Hz held-slot count is not a physical scanout measurement.

Motion variance is the variance of rendered guest-time sample steps, in ms^2;
presentation variance separately describes time between completed presents.
Audio starvation is counted in output samples. Occupancy is sampled once per
second. The input-latency estimate is the nominal buffer + interpolation +
input/display sampling budget, excluding driver/GPU and input-device delay.

The first lap exposed four pairs with failed matching diagnostics (peak two
unmatched sources). The final code explicitly rejects partial interpolation
for unmatched, ambiguous, invalid or unexplained geometry and uses an exact
endpoint instead. The diagnostic failures remain visible; they are not relabeled
as successful matches. A regression test proves the partial sample is refused.
The PGO comparison therefore includes this safety guard and immediate restored
endpoint delivery as well as PGO.

After the baseline lap, 30 rewind restores verified with zero differences,
zero missing packet replays and PGXP still enabled. Car Effects executed 4,113
draws. The 24 permitted R4 CTests passed; the PGXP-off boot test was excluded.
Presenter, interpolation, stream, mailbox and audio-transport fixtures pass.

Local receipts: `analysis/present-guest-clock/baseline-lap/` in the R4 worktree.
The earlier `baseline/` directory is an explicitly invalid stationary harness
preflight, not a lap result. No measured performance run preceded unit testing.

## Final PGO rerun

Clang 22.1.8 instrumented the runtime with `PSX_PGO=generate`. One fresh-grid
training circuit produced 5,922,976 bytes of raw profile, covering 25,114
functions and 318,147 blocks. `llvm-profdata merge -o pgo/default.profdata
pgo/lap-8420.profraw` merged it; reconfiguration with `PSX_PGO=use` and a full
rebuild applied `-fprofile-instr-use`. All 24 permitted CTests passed again.
The instrumented training timings are not a performance comparison.

| Metric | Baseline | Final PGO |
| --- | ---: | ---: |
| Circuit time (s), 523 segments | 129.627 | 130.361 |
| Guest VBlanks/s | 60.018 | 60.010 |
| Presented images/s | 31.298 | 27.010 |
| Identical consecutive submitted images | 0 | 0 |
| Held slots on derived 60 Hz grid | 3850 / 7776 | 4357 / 7816 |
| Present interval variance (ms^2) | 693.644 | 634.858 |
| Rendered guest-time step variance (ms^2) | 1671.567 | 2084.971 |
| Starved audio output samples | 0 | 0 |
| Audio occupancy min / mean / max (ms) | 161.0 / 211.9 / 274.1 | 179.1 / 223.8 / 284.0 |
| Audio overflow-dropped samples | 1088 | 4299 |
| Maximum guest interval (ms) | 35.132 | 31.346 |
| Input latency budget at last endpoint (ms) | 50.02-83.35 | 83.35-116.68 |
| Failed matching pairs (exact endpoint fallback in final) | 4 | 4 |
| Verified rewind restores / differences | 30 / 0 | 12 / 0 |

The latency budget uses the last observed pair interval (33.3 ms baseline,
66.7 ms final), not a measured input-to-photon latency distribution. Both
runs have zero dispatch/segment misses and zero missing rewind packet replays.
Full PGXP remained enabled. Final Car Effects draw count increased by 3,579
from the fresh grid. No external runtime/build interference was recorded.
The final run has zero buffer overflows; matcher peak unmatched remains two,
with unsafe partial samples rejected rather than counted as valid interpolation.

This does **not** establish steady integrated 60 FPS: the final delivery rate
and motion variance are worse. The controlled jitter tests establish the clock
mapping property, not the renderer's throughput. Zero starvation also does not
mean perfect audio delivery: overflow drops above remain, and dummy audio does
not validate audible synchronization. No further performance rerun was made.

Final executable SHA-256:
`c3d3984912660ccc4841f59c3acd512746eba7c414a95b66383c23621fe46a48`.
Local full traces and screenshots are under
`analysis/present-guest-clock/{baseline-lap,pgo-train,pgo-final}/` in the R4
worktree. The final provenance file's `pgo_profile: null` means no profile-output
argument was passed to the harness; CMakeCache and build.ninja confirm profile
**use**. Compact receipts accompany this report; binaries/profiles are local.


