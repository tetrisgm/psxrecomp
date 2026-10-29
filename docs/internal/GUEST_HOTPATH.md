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
| Supervisor baseline | 54-56 | ~100000 | Not supplied | Owner measurement, quiet PC |
| Initial baseline | 53.34 | 140681 | 63.013 | Build overlapped harness exit; repeat for clean comparison |
| SPSC | 55.87 | 85445 | 58.489 | Quiet measurement; zero grid dispatch/segment misses |
| Debug guards + direct packet fill | 58.10 | 33668 | 54.306 | Quiet measurement; zero grid dispatch/segment misses |
| Storage pooling / off-thread cleanup | 60.12 | 0 | 88.780 | Cadence span reports 59.999; interval 1 includes host_profile query |
| Bounded pacing spin / hash profile aggregation / final guards | 59.68 | 1527 | 49.720 | Quiet measurement; zero grid dispatch/segment misses |

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

Acceptance is NOT achieved: the final measured run misses 60.0/zero/<20 ms,
and runtime-wide zero allocations remain unproven beyond the focused steady-state
fixtures. Debug guard tests, CTest and smoke pass as detailed below. A clean
baseline retry timed out before frame one (the pre-existing loader startup stall).
The initial baseline is retained with its contamination caveat; the owner
baseline is the clean reference. No integration into r4/main was performed.

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

Final measured window: main_cadence reports 59.644 Hz across 1791 intervals,
30028 ms elapsed and 21671 ms quantized guest CPU. The harness frame-count
measurement reports 59.68 VBlanks/s. Worst interval 386 was 49.720 ms, including
14.32 ms preceding pacing, 0.0092 ms reserve and 0.2516 ms native completion;
there was no native queue-space or mutex wait. Other 40-42 ms intervals had
31.25-46.875 ms quantized guest CPU, with negligible native queue waits. Removing
queue overhead has not eliminated heavy emulation frames or host scheduling tails.
SDL counter samples fell from 5.0% in the pooled run to 0.9%; disabled debug hooks,
Block resize, sample clear and allocation no longer appear among the reported
leading hotspots. PGXP remains enabled. Remaining capture_write work copies
rewind state / writes emulated SPU capture RAM and cannot be disabled as debug.

Standalone draw-isolation and hold-rewind tests now provide the new globals
normally owned by memory/boot_state. Assertions verify isolation preserves an
independently armed write-trace bit and five captures read no profiling clock
when timing is disabled. Both tests pass. The worktree was returned to
fix/guest-hotpath and the candidate executable rebuilt after the baseline retry.

A second candidate verification retry also timed out before frame one; its
output is analysis/guest-hotpath-verify.txt. It supplies no performance result.
CTest was rerun on the restored candidate and passed 23/23. The Windows sampler
source explicitly warns that opt-in caller unwinding while suspending the target
can deadlock on the process function-table lock; this is a plausible explanation
for startup stalls, not a verified diagnosis or a change made by this branch.

## G1 continuation under Frame Architecture Part 2

The owner supplied Part 2 (R4 commit 2d322b5). G1 requires grid 60.0/zero
starvation and removal of hooks/allocations; the final G4 budget is average
CPU <8 ms and heaviest <12 ms. G2-G4 remain gated behind G1 integration.

The first completed G1 continuation gate failed: 58.46 VBlanks/s, 30184 starved,
55.552 ms max, 23093.75 ms quantized CPU over 1757 intervals, and **35 packet
allocation events**. Receipt: analysis/guest-execution-G1.{txt,json}. This was
not an exploratory benchmark; the new counter found an acceptance defect.

Corrections after that gate:
- Packet buffers now split/coalesce a preallocated arena; slot envelopes are
  prepared at queue/history setup. Arena capacity covers the history budget,
  three maximum live packets and a growth scratch packet. Unused slot buffers
  are reclaimed before reporting bounded exhaustion. Leases retain their arena.
  No per-frame host allocation or exception is needed for buffer growth.
- Metadata registration prepares 4096 snapshots, covering the runtime's 3600
  rewind cap plus presentation. Frame capture/release allocate nothing; false
  captures remain absent. Registration-before-main initialization is tested.
- Presentation-input envelopes are pooled. Nested native/HD/metadata cleanup
  retires with immutable native inputs onto the cleanup worker.
- DMA scopes batch native publication, publishing partial batches for reads,
  checkpoints and end-of-transfer. Producer indices cache the consumer head;
  no per-command queue-tail exchange is needed inside a burst. Native counter
  publication no longer uses an unnecessary read-modify-write.
- Packet and DMA timing reads require an explicit diagnostic query. Native
  display history defaults off; display_ring_stats enabled=1 or startup
  PSX_DISPLAY_RING=1 enables it. enabled=0 stops capture. Main cadence reports
  packet_allocation_events for its measured window.
- Windows caller sampling caches retained PE unwind directories while the guest
  runs, rather than acquiring the process function-table lock while suspended.
  Raw samples remain available when a new module's caller is not yet cached.

Validation: 2000 varying-size history frames allocate zero from frame zero;
4096 retained metadata snapshots plus 10000 reuse cycles allocate zero; native
raster recorded Beetle fixtures, nested batches/read barriers, ordering, queue
wrap and off-guest retirement pass. DMA CPU hold passes O0/O2; GP0 history,
memory guards and render-pass guards pass. Hidden GL complete-frame fixture:
111 checks, zero failures. CTest: 23/23. Corrected smoke verifies all 40 PNGs,
zero dispatch/segment misses, and display-history enable/disable. The original
smoke script ignored failed screenshot responses; tools/smoke.py now saves each
response and fails on capture errors. No phase is marked accepted by these
correctness checks alone; the next completed-phase grid gate is pending.
