# PGXP stability audit

The renderer keeps PGXP enabled throughout this audit. The fixes are title
independent: precision coordinates and seam calculations use draw-area-local
coordinates, raster coordinates use a 1/256 output-pixel grid, and texture/colour
interpolation uses triangle-local barycentrics with reciprocal depth weights.
Both horizontal and vertical VRAM page translations are covered. Perspective
texture and colour correction remain active. Host GL dithering is disabled
because PS1 dithering is already implemented explicitly in the shaders.

The textured geometry shader transports the original triangle's positions and
weighted attributes as flat data. Each fragment evaluates them using its local
pixel position, avoiding the hardware interpolator's absolute-window-origin
rounding. The shader stage is an explicit link argument; the regression verifies
it is linked and checks nonzero textured output against an analytical colour
reference. This is not a previous-frame reuse or an image-difference filter.

The only presentation change is source sampling in `present_target_quad`:
interpolate local source coordinates, then add the integer texel origin in
`texelFetch`. Fractional bilinear/area-resolve weights must not change when an
identical image lives in a different VRAM page. Queues, presentation ownership,
frame generation and swap scheduling are unchanged.

## Debug commands

`pgxp_frame` reports a completed guest frame (an optional `frame` selects it):
precision lookups, integer fallbacks, validation failures, depth-only hits,
invalidation counts, and word-depth lookup outcomes. The `over_one_pixel`
count is split into `over_one_unclamped`, `over_one_derived`, and
`over_one_projected`. They are measurements, not suppression thresholds.

`pgxp_raster` executes on the render owner and opts into bounded stream/seam
comparison (8 MiB for two float streams):

- `sequence`, `floats`, `changed`, `first`, `a`, `b`: comparison with the
  previous nonempty draw-area domain. Only proven 3D vertices are recorded;
  page offsets have already been removed. Length differences count too.
- `overflow`: results are incomplete if either bounded diagnostic fills.
- `render_frame`: completed renderer presents; it is not the guest frame ID.
- `seam_matched`, `seam_changed`: same-ordinal primitives with matching
  normalized integer/precision coordinates and depth, compared bit for bit.
- `depth_resets`: per-render-frame counts in configuration, draw-area, fill,
  threshold and present order.
- `invalid_w`, `invalid_z`: invalid inputs rejected in the completed render
  frame; corresponding `_total` fields are context-lifetime counts.

`{ "cmd":"pgxp_raster", "depth_probe":1 }` additionally enables two occlusion
queries per opaque 3D batch, before its unbiased depth-write pass. It counts
fragments failing the colour pass's actual depth test, then repeats with a
normalized-depth offset of 2^-20. The difference is `depth_near_samples`;
`depth_failed_samples` is the full failed count. These are fragment samples,
not deduplicated screen coordinates. The controlled regression verifies both
near and clearly separated overlaps. Query readback is deliberately synchronous
and expensive: use `depth_probe:0` before measuring performance. Neither pass
writes colour, depth or stencil. Normal PS1 texture discards still apply.

A paused title may redraw only every second or third VBlank. Empty-frame
counters are expected; use actual drawn frames, and require nonempty vertex
streams. A zero change count with zero matched primitives is not proof.

## Accuracy and preserved projection

A universal one-native-pixel distance from integer SXY is incompatible with
preserving the GTE's discarded MAC/SZ fractions. For example, MAC1=4095,
MAC3=151*4096, H=300, OFX=0 produces IR1=0 and integer SXY.x=0, but the exact
projection is approximately 1.98627. Clamping it to one pixel, or selecting the
IR projection, would remove the requested precision recovery. Saturated integer
SXY is another distinct exception. The test checks this example against the
analytical projection; the existing projection tests cover fractional MACs,
widescreen scaling, saturation, and divide overflow. Report these exceptions
explicitly rather than claiming the strict one-pixel gate passed.

## Regression coverage

`pgxp_raster_origin_test` uses a hidden real GL 3.3 context. It exercises flat,
Gouraud, textured, and shaded-textured triangles at 1x, 4x and 9x; identical
streams at VRAM origins (0,0), (0,240) and (320,0); fractional presentation
resolve; a real white-texture upload; analytical perspective colour; invalid
reciprocal/depth input rejection; and calibrated near-epsilon depth counts.
All PGXP corrections remain enabled. It fails on shader fallback or a missing
geometry stage. On Windows its compiler needs LTO, as does the existing render
thread fixture runner (`PSX_PGXP_AUDIT_CC` can select that compiler).

`pgxp_test`, `pgxp_word_shadow_test`, `pgxp_seam_snap_test`,
`pgxp_seam_batch_test`, and `gte_register_access_test` cover value validation,
timeline invalidation, raw CPU restore, speculative/checkpoint rollback, frozen
extra-frame contexts, and exact translated seam decisions. Rewind and disk
savestate restore both reach `gte_canonicalize_cpu_state`, which invalidates
host-only precision provenance before the new timeline redraws it.

For a real title, start hidden with `SDL_AUDIODRIVER=dummy` and
`PSX_PRESENT_SHOT_BURST=8`. Pause in-game, save an isolated slot, and use the
R4 `tools/pgxp_static_audit.py` harness with `--expected-burst 8`,
`--replay-slot N`, and `--replays 2`. It compares every consecutive composed
image in each burst, native images, and nonempty normalized PGXP streams.
Transient load notifications must expire before capture. The R4
`--require-effects` gate also requires the Car Effects draw counter to advance.

The R4 harness also accepts `--particle-car 0` as a negative pause test. The
current title effect advances forced particles while paused, outside the generic
PGXP renderer; this produces image changes with identical native/PGXP streams.
That failure must not be suppressed by changing precision or holding the image.

An additional `PGXP_AUDIT_SCALE=18` fixture run covers the 8K internal scale.
The Windows test runner prepends the selected compiler's directory to PATH so
an explicit MinGW compiler cannot accidentally load another toolchain's DLLs.
