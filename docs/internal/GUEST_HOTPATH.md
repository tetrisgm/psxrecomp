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
