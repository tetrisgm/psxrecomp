# Guest execution phases

Owner plan: R4 docs/FRAME_ARCHITECTURE.md, Part 2 (2026-10-29).
Framework phases branch independently from the accepted fork r4/main tip.
All measurements use the supervisor bisect.py, fresh grid slot 11, hidden
8K, all features enabled except FPS, rewind and Car Effects enabled, 30 s.

## G1 (accepted)

Merged at 3d7fedc8. See GUEST_HOTPATH.md for implementation and evidence.
Completed grid: 60.04 VBlanks/s, zero starved samples, zero packet allocations,
37.319 ms maximum wall interval, 21437.5 / 1801 = 11.903 ms guest CPU/VBlank.
This passed the phase-specific gate, not the final CPU/wall-time budget.

## G2: static timing segments

The emitters partition cached straight-line arithmetic at cache-line boundaries,
entry labels, memory operations, GTE/HI/LO operations, traps, control flow and
mod hooks. They compose the R3000A load-overlap interlock into one segment
charge, with literal dependency masks. Cache tags remain dynamic: a constant-PC
inline hit test invokes the shared refill model only on a miss. Uncached code
retains every fetch charge. A deadline margin guards each segment so callbacks
cannot observe registers from a different instruction; precise/replay/COSIM
paths retain instruction sequencing. PSX_CODEGEN_STATIC_BLOCK_TIMING=0 at
regeneration selects the previous per-instruction implementation.

This is not a constant flat cost that discards pending loads or cache state.
The closed-form composition handles the incoming load commit and first later
use of its destination, preserving the dummy slot and fractional cycle carry.
Overlay DLLs continue to use the host cache callback, not private cache globals.

Validation:
- 200,000 randomized segment/interlock comparisons: all CPU state and effective
  cycles/fraction identical, including all 64 scale settings and batch boundaries.
- 120-frame grid A/A reference and G1/G2 comparison: bit-identical cyc, mmio, mc,
  sp, sc, wc, ws, qc, wr and pc; zero tolerance and zero dispatch/segment misses.
  Only the isolated fixture's codegen-hash header was migrated. Guest payload
  SHA256: bea89c0a8cca4ba28220e54a77bb6ba2bcf8c7e6f7f90c133220794edffd2617.
- R4 CTest 23/23; three emitter C++ tests; seven cache/segment/hash emission
  checks and fp_identity; overlay segment compile/load check after repairing
  stale fixture callback wiring and its one-artifact assumption.
- Original and compiled cache helper fixtures, native raster fixtures, and
  40-step hidden smoke with 40 image receipts and zero dispatch misses pass.
- OpenBIOS regenerated using regen_bios.sh and stamp checked; R4 regenerated
  through tools/regen.sh. Fingerprint runner now arms write tracking at boot
  (PSX_FRAME_FINGERPRINT=1), so its default proof cannot omit writes.

The first broad recompiler CTest invocation overlapped its unfinished build and
is superseded by focused runs. That build also exposed unrelated stale fixture
wiring (ws_cull_edge_interp) and unrelated source-guard failures; no claim of a
clean full recompiler suite is made. Root project CTest and phase tests are green.

Completed candidate before forced cache-helper inlining: 59.99 VBlanks/s,
zero starved/packet allocations, 36.606 ms maximum interval,
21000 / 1803 = 11.647 ms guest CPU/VBlank. Profile: PGXP load/ALU/store
2.8/2.1/2.0%, cyc_charge 1.5%, native push 1.4%, counter 1.3%, capture 1.0%.
The compiler retained cache-hit helper calls (0.2%); forced inlining removes
that unintended call (no symbol remains in the Release executable).
Final completed phase: harness 59.94 VBlanks/s (cadence 60.048), zero starved
samples, zero packet allocations, 37.381 ms maximum wall interval,
18140.625 / 1805 = 10.050 ms guest CPU/VBlank. The final executable again
matches all 120 reference frames exactly. G2's timing identity gate passes;
the final CPU/tail-latency gate remains outstanding.

G3 is implemented on fix/inline-memory; G4 follows on fix/inline-pgxp. The final average <8 ms / heaviest <12 ms / wall
interval <20 ms target remains outstanding. Windows GetThreadTimes is quantized
in this harness, so individual CPU intervals need a finer source before a
sub-12-ms maximum can be claimed.

## G3 inline compiled memory

Generated stores and unaligned read/modify/write helpers use forced-inline
psx_mem_read/write helpers. Ordinary RAM, scratchpad and BIOS use direct
storage. Kernel dirty transitions and text-reference divergence bits are
updated inline; watched/self-modifying code retains loader invalidation.
Cache isolation, overlay callbacks, render passes, replay/capture and legacy
vector-store compatibility preserve their existing slow paths. Disabled parity
and card write observers are checked before calling their observer functions.
Timed byte/half/word and LWC2 loads share direct RAM/scratch/ROM access with
unchanged interlock, region wait, fraction and event ordering. Draw clones
rewrite the new helpers to their private-memory operations.

- Actual memory.c differential store fixture: identical bytes/counters and
  observers across aliases, with zero memory callbacks for ordinary stores.
  Also checks IsC, ROM, KSEG2, code dirty/evidence, kernel dirty transitions
  and expanded RAM.
- 30,000 randomized load cases against actual memory.c slow helpers: same
  values, pipeline state, effective clock/fraction and device-service ordering.
- Final LWC2 runtime and compact revision: all 120 grid frames bit-identical
  to G1, no tolerance, zero dispatch/segment misses.
- Project CTest 23/23, ten focused emitter/overlay checks, native fixtures and
  40-step hidden smoke pass. OpenBIOS stamp current.
- Correctness fixture migration now also updates the execution-source cookie:
  cpu_state.h participates in the execution identity even though its serialized
  layout is unchanged. Prior cookie 1a13ca4e verified against the retained G2
  executable; RAM/mod layout bits and every guest payload byte remain intact.
  Runtime production savestate validation is unchanged.

First completed G3 candidate (guest-execution-G3-complete): 58.25 VBlanks/s,
34,668 starved samples, 52.060 ms maximum wall interval, zero packet allocations,
23046.875 / 1750 = 13.170 ms guest CPU/VBlank. This regresses G2's 10.050 ms;
not accepted or merged. Executable grew from 44,210,176 to 77,652,480 bytes.
Compiled RAM callbacks disappeared; remaining psx_read_word_raw samples are
runtime consumers. This is not a claim that all memory activity is I/O.

Compact revision shares exceptional store observers and lets the compiler
share load timing independently of inline memory access. Executable 69,935,616
bytes; randomized stores/loads, exact grid fingerprints and root CTest pass.
Repeat smoke and completed-phase supervisor gate pending. G4 not started.

Final compact G3 gate: 60.01 VBlanks/s (cadence 60.043), zero starved samples,
zero packet allocations, 36.667 ms max wall, 21250 / 1802 = 11.792 ms average
CPU. This is 17.3% above G2; final performance acceptance is NOT met.
The compiled ordinary-memory callback gate is proven by differential tests;
profile load/store CPU callback helpers disappear. Remaining raw memory reads
(0.3% psx_read_word_raw, 0.2% psx_read_word) also serve actual runtime DMA/GPU
consumers; they are not proof that every runtime memory access is I/O.
Precise load timing remains shared (0.7%) independently of direct memory access.
All 120 grid frames match G1, root CTest 23/23, native fixtures, ten focused
emitter checks and repeated 40-step smoke (zero misses) pass. OpenBIOS restamped.
Merge G3 as the compiled-memory phase; G4 owns the still-unmet final CPU budget.

## G4 in progress (not measured or accepted)

Branch fix/inline-pgxp begins at G3 merge 1ae4b9f5. A C-compatible host-only
precision-state view shares the main engine's actual registers/scalars and
RAM/scratch shadow arrays; private draw engines remain independent. Generated
hooks inline loads/stores, moves, constants, comparisons and no-precision ALU
cases. Scalar stores, complex arithmetic and checkpoint journaling retain the
reference engine. Store diagnostic recording is explicitly enabled through
pgxp_store_ring enabled=1; disabled stores allocate/copy/record nothing for it.

300,000 randomized operations compare every state byte (including stale
projection bytes) against original engine functions; checkpoint rollback and
trace enable/disable tested. Existing PGXP and word-shadow fixtures pass.
Baseline grid stream captured from retained G3 executable; 120 exact guest
fingerprints and 143,487 ordered triangle records over 59 drawing frames.
G4 runtime build, stream comparison, smoke/CTest and final grid gate pending.
Main cadence also tracks elapsed work excluding the pacing bracket, a
conservative upper bound on non-pacing guest CPU (includes waits/descheduling).
No final average/heaviest/wall budget claim is made.

First completed G4 gate: 59.99 VBlanks/s, zero starvation/packet allocations,
19859.375 / 1807 = 10.990 ms mean CPU, 34.451 ms max non-pacing elapsed,
39.552 ms max interval. PGXP load/store calls disappear, ALU falls to 0.7%;
cycle charge is 3.2%. G4 remains unaccepted/unmerged. 480 grid fingerprints
exact, 568,895 triangle records on 231 complete frames exact, six PGXP/context
fixtures and root 23 CTests/native/40-step smoke pass.

Next correction removes duplicated ALU/PGXP bodies from the static-timing
fast/deadline alternatives and inlines the common open-batch cycle charge.
300k differential charge cases and 200k segment cases pass. A review also
identified G3's idle-loop last-store metadata was incorrectly treating narrow
stores as word stores; narrow stores must invalidate its address and preserve
the previous word value. New regression assertions cover this and ROM stores;
move this frequently enabled tracker back inline, outside the rare debug path.
Build and final correction/verification pending; do not measure until done.

G4 compact completed gate (still rejected): 60.30 measured VBlanks/s,
938 starved samples, 40.167 ms maximum interval, zero packet allocations,
20953.125 / 1810 = 11.576 ms average guest CPU, 36.708 ms maximum
non-pacing elapsed. Profile aggregation previously truncated addresses before
function grouping. The tool now requests all 32,768 available bins and the
handler emits an unsorted full histogram in linear time. This window returned
8,874 distinct addresses, zero unbinned samples and zero omitted samples.
Consequently percentages are not directly comparable to the truncated older
reports. Timing load 2.6%, general charge 2.4%, PGXP ALU 2.3%, instruction
interlock 1.2%, static segment 1.0%. The capture_write symbol also names real
SPU capture/rewind work; it must not be disabled as though it were debug-only.

Next completed candidate keeps exceptional cycle handling out of line and
forces small timing interlocks inline, folding literal dependency masks.
Fractional-only charges and opening ordinary batches also stay inline.
300,000 differential charges and 200,000 segments pass, including scaling,
replay/conservative/device-service/local-sink modes and overflow. Memory
regressions pass. Rebuilt correctness and final performance gates pending.

G4 interlock completed gate rejected: 59.35 VBlanks/s, 12,658 starved,
51.693 ms max interval, 23812.5 / 1782 = 13.363 ms mean CPU, 41.605 ms
max non-pacing elapsed. Executable 196,705,280 bytes. Timing helper calls
vanished, but code growth regressed execution. Correctness: 480 exact frames,
586,644 exact triangles on 238 complete frames, root23/framework7/native/
stamp/40-step smoke all pass. Do not confuse removal of call symbols with
removal of their work. No merge or final-budget claim.

The next revision accumulates raw CPU costs inside a block whose static upper
bound fits strictly before the next device deadline. Common interlocks add to
that accumulator; scaling/publication happens once at an observation barrier.
Near deadlines, replay, diagnostics, frozen time and local-sink modes retain
the original general path. Cache refills, MMIO, GTE/muldiv, dispatch and clock
publication disarm accumulation. Freeze/replay preserve pending raw state;
restore discards it. Host-only state, no serialized CPU layout change.
50k randomized blocks match original clocks, fractional carry and services at
observations; 30k load cases exercise guarded accumulation and deadlines that
mutate loaded RAM. Freeze/uncharged tests and ten focused emitter tests pass.
Broad recompiler build exposed unrelated standalone GPU/interpreter fixture
link wiring (their whole included GPU sources retain unresolved dependencies);
not a claim that the entire framework suite passes. Runtime rebuild and exact
race/PGXP/smoke checks pending before another completed-phase measurement.

G4 block candidate completed: 60.02 VBlanks/s, zero starved/packet allocations,
16796.875 / 1807 = 9.295 ms mean CPU, max interval 39.480 ms and max
non-pacing elapsed 32.066 ms. Still rejected against the final budget.
480 fingerprints and 586,644 ordered triangles on 238 complete frames match;
root 23 CTests, focused framework/emitter/native and zero-miss smoke pass.

A freeze regression now compiles with block accumulation enabled: restoring
scale gates must preserve saved pending raw charges and fractional carry,
without publishing them under a temporary render-pass scale. The new assertion
fails before the correction and passes after it. Supported optimized runtime
builds now select IPO automatically, with supported linker workers bounded to
four; OFF remains explicit and unsupported AUTO falls back. Isolated IPO
configuration/link/run tests pass. Runtime correctness and grid gate pending.

G4 IPO completed gate: 60.01 VBlanks/s, zero starvation/packet allocation,
15234.375 / 1803 = 8.449 ms mean guest CPU, 37.966 ms max interval,
26.260 ms max non-pacing elapsed. Still rejected. Root23/framework9/native/
stamps/40-step smoke pass; exact480 fingerprints and586644 triangle records.
The next correction rechecks guarded accumulation after emitted cache fetches:
refill costs publish normally, then the containing block bound must fit before
the next device deadline. Active blocks keep their existing proof and charges.

G4 guarded restart completed gate: 59.98 harness / 60.014 cadence VBlanks/s,
zero starvation/packet allocations, 14296.875 / 1802 = 7.934 ms mean CPU,
38.396 ms max interval,23.615 ms max non-pacing elapsed. Average now meets
budget; spikes still reject the phase. Exact480 fingerprints/584109 triangles
on237 complete frames, root23/framework9/native/stamp/smoke pass. Optional
post-window profile neighborhoods use the existing sample ring only (+/-2
frames because cadence ordinals and frame queries are separate snapshots).

Next revision inlines precision-bearing shifts and bitwise half propagation,
preserving dormant destination bytes and falling back for precise scalar
shifts. Differential tests now alternate CPU mode and projection preservation;
300k operations and both original/compiled PGXP suites pass. Restart admission
uses the remaining static bound instead of the whole containing block bound.
Rebuild and complete correctness/performance gate pending.

Before measuring the fields revision, review found function-entry tracing was
still unconditionally reading register/TCB state and filling diagnostic rings.
Compiled unarmed entries now only stamp the provenance function address;
replay and stack guards retain their semantics. Dispatch diagnostic bookkeeping
is also opt-in; actual dispatch counts/current-function stamps remain live.
Use debug_hotpath calls=1/0 to arm/disarm; trace configuration/clear commands
also arm, while fn_stats polling does not. A unit fixture proves 10000 unarmed
entries make zero observer calls, and verifies arming/replay/disarming. Hidden
runtime verification also checks trace counters advance only when armed.

G4 fields/call-hook completed gate: 59.93 harness / 60.029 cadence,
zero starvation/packet allocations,13015.625/1807=7.203ms average CPU,
maxwall35.960/non-pacing22.640. Still rejected. Exact480fingerprints/584205
triangles on237frames; root23/framework10/emitter10/native/stamps/40-step smoke
and live diagnostic arming/disarming pass. Two stale standalone interpreter
fixtures now wire direct RAM and inactive override state; both pass with GCC
LTO. Clang's whole-GPU fixture link limitation remains configuration-specific.
Assembly inspection: the788-byte guest transform function becomes164864 host
bytes. The next revision shares inactive/direct-load timing machinery out of
line and retains only exact RAM pipeline updates/raw charges on the proven
active block path. No guest state/model change; differential gates pending.


G4 compact-load completed gate rejected: 59.98 harness /60.051 cadence,
zero starvation/packet allocations,20015.625/1803=11.101ms average CPU,
maxwall37.871/non-pacing33.470. Exact480fingerprints/571542 ordered triangles
on232 complete frames, root23/framework10/native/stamps/40-step smoke pass.
The smaller147MB executable and131KB transform did not improve measured cost.
Restored the fields candidate's inline timing and bitwise paths. A separate
compiler assembly proof identified byte-array aliasing: writes through the
ReadAbsorb uint8 array force unrelated flag reloads. Replaced each element
with a distinct one-byte struct (compile-time size check; identical serialized
layout), updating runtime/overlay headers and differential tests. 30000 load
cases, static timing/cache tests and300000 PGXP comparisons pass. Regenerating
and building; no further performance claim and G4 remains unmerged.


2026-10-29 owner acceptance update: merge G4 now; the approximately38ms maximum
interval predates G4 (G1 37.319ms) and is explicitly removed as a G4 gate.
G4 fields achieved7.203ms average guest CPU with0 starved samples. The final
ReadAbsorb alias revision preserves the byte layout and passed480 exact guest
frames/581682 ordered PGXP triangles, root23/framework10/native fixtures and
40-step zero-miss smoke. A final rebuild follows the header isolation and
single-source timing-body repairs; no final-revision performance claim yet.
The separate interval task is limited to three hours from2026-10-30 02:17 UTC.

Broader recompiler checks:135/149 executed tests passed before repairs (three
additional tests disabled). Fixed the Clang closed-fixture links, draw-context
header leak, stale compiled-fetch/store expectations, BOM-aware preprocessing,
and duplicate textual timing branches. Unrelated prior renderer/session/source
assertion failures are not claimed green. Required runtime/focused gates above
remain the acceptance evidence; follow-up interval work must preserve them.


2026-10-29 interval task, first completed candidate: restored host pacing before
the actual scheduler VBlank edge (before I_STAT, FIELD and mod hooks), based on
6f0dabcc, which the phase-3 final integration had omitted. Preserve the original
cadence probe. Headless/debug turbo, netplay and resimulation keep their policy;
frontend fast-forward transitions apply at the next event and cannot wait twice.
The scheduler fixture proves pre-IRQ ordering and unchanged guest cycles.

Integrated build,13 focused framework tests,22 permitted R4 CTests (exclude
PGXP-disabling boot fixture),current OpenBIOS stamp,native recorded-oracle
fixtures and40-step hidden zero-dispatch/segment-miss smoke pass. Exact480
guest fingerprints and581880 ordered PGXP triangles on236 complete frames.
Fresh supervisor slot11 all-on/FPS-off grid:59.94 harness/60.001 cadence,
0starved,0packet allocations,max28.284ms,meanCPU17796.875/1803=9.871ms.
Worst interval has0.0005ms pacing and0.0539ms native completion: the remaining
spike is guest work, not sleep debt or native completion. This does not pass20ms.
Receipt: analysis/guest-execution-interval-event.{json,txt} in R4 worktree.
The previous G4 fields maximum was35.960ms and G1 was37.319ms; this integrated
measurement also includes the fork's phase-3 surfaces, so it is not an isolated
A/B attribution for every changed component.

Second completed interval candidate: share precision-bearing bitwise propagation
with snapshots inside the helper, retain inline no-precision operations, and
give ordinary load/block paths compiler likelihood hints. Runtime159.091MB ->
144.687MB; linked transform span147392 ->130944bytes. Build/13focused/22permitted
R4/native/stamp/zero-miss40-step smoke pass. Exact480fingerprints and577170
triangles on234complete frames. Supervisor fresh slot11:59.94harness/60.007
cadence,0starved/packet allocations,max26.857ms,CPU16359.375/1803=9.073ms.
Worst pacing0.0005ms/native completion0.0779ms. Improvement, not a20ms pass.
Receipt: analysis/guest-execution-interval-layout.{json,txt}.

Third completed interval candidate: compiled completed-GTE accesses retain an
admitted block when there is no possible device event and no live GTE stall;
all conservative/scaled/in-flight cases retain the publishing reference path.
Direct memory timing uses the same proof; LWC2 static bound now includes its
load latency.101921 GTE comparisons at O0/O2 and30000 load cases pass. The PGXP
shared helper now has one runtime definition (fixes unused O0 COFF linkage).
Opt-in QueryThreadCycleTime supplements quantized GetThreadTimes telemetry.

Build,13 focused framework/22 permitted R4/3 emitter tests,native/stamp and
40-step zero-miss smoke pass.480 exact fingerprints/581634 PGXP triangles on
236 complete frames. Fresh slot11:60.00harness/60.006cadence,0starved/packet
allocations,max25.822ms,CPU15312.5/1802=8.498ms. Worst interval114888694thread
CPU cycles,0.0003ms pacing,0.0568ms native completion. Whole-window70897992312
thread cycles/15312.5ms CPU gives4.630Mcycles/ms as an aggregate diagnostic;
not a fixed-frequency guarantee. Remaining maxima are predominantly executing
guest work rather than waiting on native completion. Target still not met.
Receipt: analysis/guest-execution-interval-gte.{json,txt}.
