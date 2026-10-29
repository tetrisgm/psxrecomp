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

G3 and G4 have not started. The final average <8 ms / heaviest <12 ms / wall
interval <20 ms target remains outstanding. Windows GetThreadTimes is quantized
in this harness, so individual CPU intervals need a finer source before a
sub-12-ms maximum can be claimed.
