# Native raster parity evidence

Reference: libretro/beetle-psx-libretro commit
`53246dc2d00f3c00cc56baa1061902da7a6651e4`. The reference is built separately;
no reference rasterizer source is included in this framework.

`native_raster_oracle_bridge.c` links against that reference and exposes its
GPU command entry point. It enables PGXP memory, CPU, GTE, vertex cache,
texture correction and NCLIP. These integer-only GP0 fixtures have no precise
vertex provenance. This does not alter application PGXP settings.

`run_native_raster_oracle.py` accepts separately built reference and native
shared libraries, DLL search directories and an output directory. It resets
both native VRAMs to the same deterministic pattern, sends E1-E6 state and
one original draw packet, then compares all 524288 halfwords. Its receipt
includes the first mismatches and the complete reproducing packet/state.
The native library exports the normal `native_raster_*` API and links
`native_raster.cpp`, `gpu_sw_renderer.c`, `gpu_vram_dirty.c`, plus the same
no-wide fixture globals used by `test_native_raster.cpp`.

On Windows the reference Makefile's static archive omits frontend-provided
libretro-common functions. Build a shared reference with `STATIC_LINKING=0`
and an import library, exporting internal symbols for the test bridge.
The bridge never calls a frontend, opens a window or loads a game.

2026-10-28 evidence in R4's local `analysis/perf-20261028/`:

| Check | Result |
| --- | --- |
| 2000 untextured packets, random destination masks and blend modes | exact |
| 10000 mixed triangle/quad/line/sprite packets, all three texture formats, flips, dithering, masks and feedback | exact |
| 10000 mixed packets with signed 11-bit offsets and coordinate stress | exact |
| Native unit fixture including 16 full-VRAM reference digests | pass |
| Partial A0 snapshot/resume and stale palette checkpoint restore | pass |

The mixed and coordinate receipts are `native-parity.json` and
`native-coordinate-gate/native-parity.json`. The retained digest cases in
`native_raster_oracle_fixtures.h` use the documented initial pixel formula
and FNV-1a over little-endian VRAM bytes. Digest checks are regression tests;
the live oracle runner compares every halfword, not only a hash.

This is component evidence, not phase-1 acceptance. Production GP0 capture,
guest read routing, GPU snapshot wire integration, interlaced draw gating,
and complete cache lifecycle integration must pass before promotion.
