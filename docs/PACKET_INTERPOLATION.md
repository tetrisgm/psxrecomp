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
inferred from the census. The latest memory-cost and cross-caller staging fixes
are undergoing fresh hidden-game validation.
