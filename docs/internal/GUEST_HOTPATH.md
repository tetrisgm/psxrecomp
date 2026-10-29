# Guest hot-path work (2026-10-29)

Branch fix/guest-hotpath starts at feat/frame-arch-4-interp HEAD. Owner authorizes
integration into fork r4/main only after all acceptance gates pass. Work is solo.
The referenced F:/ shared policy/Beads database is absent on this PC; repository
HLE_EXECUTION.md and this ledger record the work without creating another database.

Harness: supervisor bisect.py, BROOT=r4-wt-interp, BSLOT=11, BGRID=1,
BRUNS=<step>:1:1, BPROF=1, PSX_HOST_SAMPLER_CALLERS=1, hidden window, all features
except frame-rate. Each measured window is 30 seconds. PGXP remains enabled.

| Step | VBlanks/s | Starved samples | Max interval ms | Notes |
|---|---:|---:|---:|---|
| Initial baseline | 53.34 | 140681 | 63.013 | Build overlapped harness exit; repeat for clean comparison |
| SPSC | 55.87 | 85445 | 58.489 | Quiet measurement; zero grid dispatch/segment misses |
| Debug guards + direct packet fill | 58.10 | 33668 | 54.306 | Quiet measurement; zero grid dispatch/segment misses |
| Storage pooling / off-thread cleanup | 60.12 | 0 | 88.780 | Cadence span reports 59.999; interval 1 includes host_profile query |

SPSC replaces the mutex queue with 16384 fixed trivially-copyable records and a
4 MiB upload arena. Packet and legacy command payloads share a union. Upload
bytes are owned until execution; synchronous restore borrows until the barrier.
Release/acquire head and arena positions protect reuse. Sequentially consistent
parking/publication closes the empty-check/sleep race; retained events preserve
notifications. Producer backpressure signals only when parked. The independent
checkpoint slot retains stream sequence ordering, including its second-client
fallback and teardown drain. Ordinary GP0 needs no allocation or worker signal
unless the worker announced parking. A reusable event replaces promise allocation.

Validation: native_raster_test passes including recorded Beetle oracle fixtures,
GPUREAD, uploads, cache restoration, immutable inputs, ordered checkpoints,
blocked-consumer wrap and teardown. Added producer-thread allocation test executes
100 frames of 1000 GP0 packets plus uploads and reads, with zero C++ allocations.
The blocked-consumer fixture was updated to the explicit 16384-slot capacity.
An observer regression (ordinary arena upload incorrectly labelled restore) was
caught and corrected before committing.

Not accepted yet: runtime-wide zero allocations, debug hooks, clean baseline,
full ctest/smoke, and 60.0/zero/<20 ms target. No integration until these pass.

Debug hooks: memory stores check a plain guest-owned flag before calling or
reading old RAM. Draw isolation owns a separate bit and remains active even when
tracing is disabled. Block observation is inline in the shared CPU header, with
an ABI wrapper retained for old overlays. Explicit write/cycle debug commands
activate the corresponding hooks. `debug_hotpath` accepts `writes` and `cycles`
booleans to disarm/rearm; ordinary frame/audio/host-profile polling does not arm.
Launch-time write ranges, record-frame and PC-probe controls remain supported.
Guest-cycle sampling now defaults off; PSX_GCP_PERIOD or gcp_ring period=N arms it.
Rewind capture timing is separately opt-in via rewind_status profile=1; native
capture data and SPU capture-buffer writes are required work, not debug logging.

Allocation work: native texture input envelopes are acquired on the native
worker; releases use an embedded MPSC retirement list. A cleanup worker destroys
sample/tile references and recycles envelopes, including those outliving their
raster. Frame storage is reused across history eviction. Block growth does not
zero payload bytes that the producer overwrites; padding is still zeroed.
History envelopes use reserved vector storage rather than recurring deque-node
allocation. A fixture verifies zero C++ allocations for 1900 steady-state frames
with history retention/eviction after warming 100 frames. This does not yet prove
zero allocations for arbitrary new frame-size high-water marks or the complete
runtime guest thread.

The pooled run's next-worst interval was 37.333 ms (31.25 ms quantized guest CPU;
native completion 0.116 ms, reserve 0.005 ms). The 88.780-ms first interval was
62.5 ms guest CPU immediately after the harness's initial host_profile query.
That handler linearly searched up to 4096 bins per sample; it now uses a fixed
8192-slot hash table and avoids repeated executable-module lookups. This retains
its complete histogram semantics rather than suppressing measurement samples.

Pacing now uses microsecond sleeps with a 500-us final spin margin (formerly up
to 2 ms). Tests retain the existing deadline-underflow and period-cap checks and
cover the new decision. Required pacing clocks remain active; capture-profiling
clocks are opt-in. Neither real SPU capture writes nor rewind image copies are
silenced as if they were debug logging.

Validation after the timing changes: R4 CTest 23/23 passed. Full hidden smoke
(tools/smoke.py, port 4831) completed all 40 steps through frame 9243 with zero
dispatch/segment misses; final screenshot was inspected. Native raster, mailbox,
memory trace guard, render-pass guard and pacing tests pass.

Two attempted final benchmark starts stalled at frame zero. A host thread probe
of owned PID 38624 resolves the main-thread return into WIN_GL_InitExtensions;
this matches the pre-task analysis/wait-startup-stack-2.txt stack, whose main
thread is in the Windows loader below WIN_GL_InitExtensions. Native raster was
normally parked. An intervening launch completed the entire smoke. No unrelated
process was stopped. A separate retry hit WSAEADDRINUSE (10048); Windows showed
~16000 TIME_WAIT sockets mostly to an unrelated media service. Root tools/dbg.py
now has a bounded explicit-source-port fallback before sending any bytes, tested
with a forced connection error and exactly one successful request/response.
R4 tooling-only commit: d3f7cae (not pushed with the framework branch).
