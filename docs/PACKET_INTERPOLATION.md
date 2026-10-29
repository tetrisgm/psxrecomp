# Generic frame-packet interpolation

The guest publishes complete immutable packets. `packet_presenter` retains the
newer packet by shared ownership and schedules samples using a rational display
clock. `packet_interpolation` consumes two owned geometry/source tables and emits
view-space interpolated vertices. Neither module executes guest code. Rates
60/90/120/144 are fixture-covered; missed deadlines are skipped without a backlog.

## Identity and visibility

PGXP carries source vertex and matrix RAM origins, caller, transform fingerprint,
model point, view position and H/OFX/OFY. Ordinary vertex addresses retain identity
through animation. An address observed emitting multiple model points is staging
storage across all callers: its vertices use model-point identity, not submission
order. Primitive identity hashes ordered source identities, excluding colour, UV,
texture animation, OT bucket and alternating packet addresses.

Pre-cull source tables retain vertices not emitted as polygons. Whole model
invocations are resolved through shared sources. A missing point in an existing
invocation is reconstructed using that invocation's retained exact GTE affine
matrix; ambiguous correspondence fails rather than guessing. A new-source vertex
is a verified appearance only when its entire model invocation is absent in the
older source table. Verified appearances take N from the first in-between image.
`unmatched`, `source_ambiguous` and `unexplained` are debug-server gate counters.
Zero counters alone do not prove visual correctness.

Matrices are observer-only data in the packet's pre-cull attachment, not in every
PGXP memory shadow. This avoids inflating guest snapshots and rewind copies.

## Presentation and metadata

The GL hook first collects the pair, then replays immutable records with sampled
geometry. It preserves N's colours, UVs and 2D/HUD commands. It clips signed-depth
samples before division. Presentation is deferred until draws complete and selects
the draw region, including the vertical half of a double-buffered wide surface.
Held VBlanks retain the geometry pair without making the next packet look lost.
Epoch changes clear retained packets and metadata. Local discontinuities take N;
scene-wide discontinuities snap the pair.

`packet_metadata` registers typed-size, opaque capture/blend providers. Capture
runs on the producer; interpolation and reads use packet-owned copies on the
render owner. R4 supplies camera/car/light data for Car Effects. Optional effects
never select a guest re-execution or serial interpolation path.

## Validation status

Sampler, stream, presenter/metadata and lease fixtures pass. R4's 45-pair driving
census has zero unexplained/unmatched/ambiguous uses. Runtime visual and 8K-rate
acceptance are tracked in R4 `docs/FRAME_INTERPOLATION_STATUS.md`; they must not be
inferred from the census. Final hidden full-PGXP 8K comparisons at `2e39cbe6`
show interpolation-only wall/roof gaps absent with generic rate 0. Requested
60/120 output produced 25.6/26.1 distinct images/s, with audio starvation in both
interpolated and stock runs. The presenter cutover is NOT accepted for integration.
A whole-model cut-threshold experiment worsened geometry and was reverted.
Compact-layout rewind restored 49 snapshots without failures and resumed a clean
45-pair census; the hidden complete-frame GL fixture passed 111 checks.


### Exclusive profiling follow-up (2026-10-29)

Transform attachment storage is now prepared and reclaimed by a CPU worker.
The guest appends into its private fixed-capacity block, seals a nine-tick
immutable history with reference increments, and returns attachments through
a lock-free list. Neither append, sealing, release nor reset allocates on the
guest. A 128-entry prepared queue absorbs worker scheduling delays; exhaustion
rejects the optional packet and increments `storage_misses`, without blocking
emulation or silently accepting a partial transform table. The pool retains
its high-water storage for reuse; raw spare arrays are not zero-initialized.
The worker also owns final block reclamation from rewind and presenter leases.

Primitive provenance is grouped with the following draw setup instead of
forcing an extra transport flush per triangle. Replay copies potentially
unaligned grouped provenance into an aligned local value. Pair matching,
departing topology, texture restoration and source tables remain render-owned.
`audited_pairs`, `failed_pairs` and `peak_unmatched` are cumulative session
counters, preserved across resets so polling cannot hide a transient miss.

The focused stream fixture checks zero producer allocations with 64 retained
attachments, immutable held history, reset, source matching and shared retiring
edges. Sampler, presenter, HD context and hidden GL packet fixtures also pass.
Performance acceptance awaits the exclusive 60-second A/B measurements recorded
in R4's FRAME_INTERPOLATION_STATUS.md; this is not a phase-4 acceptance claim.

The follow-up also leases the nine immutable transform blocks on the render
consumer, removing a complete history copy per consumed packet. Iteration order
and matching inputs are unchanged. The fixture releases both the source table
and producer history before consumer matching to verify lease lifetime.
