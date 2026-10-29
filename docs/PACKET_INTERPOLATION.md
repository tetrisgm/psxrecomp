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
ordered model vertices, original RAM origins, matrix RAM origin, callers,
primitive type, texpage and CLUT; guest DMA packet addresses are excluded. Repeated mesh instances can collide semantically.
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


## Inclusive census follow-up

The near-plane/projective GPU path bypassed the first-pass census. It now
records both triangle halves with their original signed projection provenance.
The native-wide fixture verifies two records, complete validity masks and
retained negative depth. This is an audit fix, not signed-depth sampler support:
the sampler still requires positive view depth, and production clipping must
operate on interpolated homogeneous vertices rather than divide across z=0.

The last hidden 18x R4 race gate, with PGXP full, Rewind enabled and Car Effects
active, failed all 45 samples: max 1592 unclassified unmatched, zero ambiguous,
one invalid, zero partial provenance. This includes the first pairs after
leaving turbo navigation; it is not a controlled performance interval. Earlier
ordinary-path-only sampling failed 35/45, max 448 unmatched. Neither run proves
that all missing keys are genuine appearances; neither is acceptance evidence.
Three frozen ordinary-path pairs had 30/32/16 missing keys and no equal ordered
model-coordinate tuple under a different key. This narrows the investigation
to changing visible geometry/topology but does not establish no-pop behavior.

Original-data identity across animated/procedural geometry and model invocation
ordinals remain unproven in moving packets. The missing work also includes
signed-depth clipping, visibility transitions, complete replay integration,
packet-owned interpolated effects metadata, title FPS cutover, legacy removal,
and distinct-image/guest/audio measurements at 60 and 120 output. Do not infer
completion from zero duplicate ambiguity or relabel candidates as proven births.

Final validation: R4 build/32 CTests; five focused framework tests (sampler,
stream, PGXP, native-wide projection, render-pass provenance); hidden race
boot/navigation with zero dispatch/segment misses. The sampler tests include a
minimum-cost case that defeats greedy nearest-neighbour selection. Assignment
uses the smaller group as Hungarian rows to avoid cubic work for one-to-many
birth/death groups. Frozen debug captures clear on timeline reset.


## Vertex-source continuation (2026-10-29, not accepted)

The main PGXP engine now observes resolved RTPS/RTPT provenance before guest
culling. Optional audit packets own bounded transform-history attachments;
private draw contexts do not publish. Snapshot copies survive mailbox replacement
and producer advancement. Eight publication intervals are retained, with a
262144-record hard bound; failure rejects a packet rather than publishing a
partial table. PGXP timeline invalidation clears the producer history.

The separate sampler accepts source tables and interpolates each matched
vertex independently. RAM vertex origin, matrix origin, caller and transform
occurrence are candidate keys; model coordinate values are not identity. Signed
depth is accepted and homogeneous coordinates remain finite at zero depth. A
separate clipping helper clips in view space before projection, retaining N's
attributes and interpolating attributes only on newly clipped edges.

This remains an audit, not production frame generation. The collector chooses
transform groups referenced by submitted geometry from the packet-owned history,
so an entirely culled invocation is not yet represented in the resolved previous
table. Occurrence identity under changes to the set of visible instances remains
unproven. A missing key is reported as `new_source`, not certified as a spawn.
The runtime gate now rejects those unverified cases. Do not remove legacy redraw
or claim a performance result from these counters.

Fixtures cover invisible previous primitives with available source vertices,
signed/zero depth, near-plane clipping, mutable mesh coordinates, OT submission
across VBlanks, immutable attachments across twelve discarded publications, HUD
attributes, cuts, and rational 60/90/120/144 output deadlines. Live validation
before the final lifecycle/identity follow-up observed 45 driving pairs with
zero unresolved/ambiguous vertices, but three pairs had 19/31/36 new-source
vertices. That fails acceptance. Earlier zero-miss captures were paused and are
only stationary repeatability evidence. Renderer cutover, effects interpolation,
legacy deletion and 8K distinct-image measurements remain outstanding.


The ordinal follow-up distinguishes vertices streamed through a shared RAM
staging address by their pre-cull first-transform ordinal. Its fixture changes
model coordinates while preserving transform order. A broader steering census
(`analysis/interp-ordinal45.json` in R4) rejected 27/45 pairs: up to 617 unverified
new-source uses, 2394 matched uses, zero source-unmatched/ambiguous/invalid.
This is not acceptance: ordinal stability and entirely culled invocations still
need proof. That run predates the final diagnostic-only `source_moving` counter;
its gate used `--stationary` to disable motion-counter enforcement, despite
steering input. No motion/no-pop acceptance should be inferred from that option.
The final gate explicitly rejects stationary samples unless requested.

## Presenter continuation (work in progress)

The render owner now retains immutable packet leases, samples source vertices
and replays the packet at timed output deadlines through a separate
`packet_presenter` module. `packet_metadata` captures title-owned values on the
producer and blends them only on the consumer; it never reads guest RAM during
presentation. R4 wires its FPS option and camera/car metadata to these APIs.

Source-model appearance classification uses source origin, matrix origin, caller
and transform occurrence, excluding the streamed vertex ordinal. An absent
vertex within an existing invocation is reconstructed with its retained exact
GTE affine transform, rather than exempted as an appearance. Sampler fixtures
cover this distinction. This expands PGXP metadata and requires fresh local
savestates; old raw PGXP snapshots are not compatible.

Runtime acceptance, no-pop captures and performance measurements are pending.
Legacy code is retained until the generic rendered output has been checked.
