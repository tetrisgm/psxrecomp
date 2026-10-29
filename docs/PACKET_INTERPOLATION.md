# Packet interpolation development status

Phase 4 is **not accepted**. Production rendering has not switched to this
sampler. Guest redraws, transactions and private workers remain until the
replacement meets the match and visual gates.

`packet_interpolation.h` is the renderer-independent pair consumer. It owns
copies of its inputs, builds an exact identity map once, and samples at any
fraction without allocation or guest access. It interpolates view-space
coordinates and projection constants, then projects. Colours, UVs and 2D
primitives come from packet N. Unmatched geometry also takes N. Explicit
discontinuities, timeline changes and configured large motion snap the pair.
Absolute rational deadlines cover 60/90/120/144 Hz without period truncation
drift. It does not provide visibility transitions, effects metadata or a
presenter schedule by itself.

PGXPProjection now transports pre-projection MAC coordinates, H, OFX/OFY,
horizontal projection scale, model coordinates and projecting caller. This
uses the existing value-validated full-word shadow and its invalidation and
checkpoint lifetime. Packet records contain copies, never guest pointers.
Tests prove transport through two distinct DMA buffer addresses; this proves
transport stability, **not unique primitive identity in a race**.

Set `PSX_PACKET_INTERPOLATION_AUDIT=1` before starting a hidden runtime and
query `{"cmd":"packet_interpolation"}`. The candidate is a 128-bit hash of
ordered model vertices and callers, independent of packet addresses, draw
order, colour and UV. Repeated mesh instances can collide semantically.
Duplicate candidates now pair by occurrence for equal counts and by gated
minimum-cost view-centroid assignment otherwise. Counters distinguish resolved
occurrence groups, resolved assignment groups, and unresolved primitives.
`duplicate_equal` diagnoses repeats with identical view-space geometry; they
do not imply ambiguity after deterministic assignment. `partial_provenance` includes depth-backed polygons
without complete projection provenance, rather than silently calling them HUD.

The diagnostic compares successive nonempty polygon packets. Empty VBlank
packets occur between 30 Hz draws and do not erase the preceding sample. This
is an audit policy, not a blank-frame/clear policy for a production presenter.
Latest stats are copied under a short diagnostic mutex; no guest or GL state
is read by the TCP command.

## Evidence, 2026-10-29

- Isolated R4 Release build with debug tools: passed; 32/32 R4 CTests passed.
- Sampler and stream fixtures, PGXP transport, native-wide projection and
  render-pass provenance: 5/5 CTests passed.
- Hidden complete-frame GL fixture: passed.
- Hidden boot-to-race smoke through guest frame 9225: zero dispatch and
  segment misses; screenshots inspected.
- Moving-race candidate audit (160 sampled packets): up to 332 unmatched,
  32 ambiguous, zero partial-provenance samples. A subsequent sample has
  32 ambiguous with zero equal-geometry duplicates: these are distinct
  placements sharing the candidate, not merely repeated identical draws.
- The explicit runtime gate subsequently failed 18 of 45 samples, with up
  to 652 unmatched and 66 ambiguous candidates. The gate exits nonzero.
- An 18x diagnostic interval verified full PGXP, Rewind enabled and Car
  Effects draw counter increasing by 329. Up to 304 unmatched were observed.
  This used the old presenter and is not a distinct-image-rate acceptance
  measurement. No 60/120 generic-output or no-pop claim is made.

## Remaining implementation

Resolve instance-level provenance and prove the identity census across
double-buffered moving packets. Distinguish genuine appearance/disappearance
from identity loss with independent evidence. Integrate the sample output and
cadence into packet replay, carry/interpolate title effects metadata, verify
visibility transitions and cuts with captures, then switch the title FPS
feature and remove guest re-execution machinery. Only then measure distinct
images/s, VBlanks/s and starved samples at 18x and 60/120 output.

The module intentionally has no dependency on the phase-3 surface allocator.

## Continuation: stronger provenance and duplicate resolution

Packet identity includes original vertex RAM origin (preserved through exact
scratchpad copies), matrix RAM origin when available, ordered model coordinates,
projecting caller, primitive type, texpage and CLUT. The GTE also transports a
rotation/translation value fingerprint for diagnosing model invocations; values
change with motion and therefore are not exact temporal keys. No guest DMA
packet address enters identity. Equal-count duplicate groups pair k-th to k-th;
unequal counts use rectangular Hungarian assignment with normalized view-space
centroid gating and dummy unmatched columns. Deterministic tie breaking follows
packet order. `appeared_candidates` remains part of `unmatched`: absence of a
key alone does not prove visibility change.

The debug command accepts `capture:1` to freeze the next pair and `detail:1`,
`older:0|1`, `start:N` to read eight provenance records per page. Capture data
is owned, read under a short mutex, and does not access live guest or GL state.
The audit remains opt-in; this is not presenter acceptance.
