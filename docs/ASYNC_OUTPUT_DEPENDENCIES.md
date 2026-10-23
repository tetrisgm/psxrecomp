# Async output topic provenance

`feat/async-render-admission` is rooted at synced upstream `2b511773` through
`feat/output-timestamp-plan` (`a863b7f7` through `12c1e5c4`). It contains no
`r4/main` or `int/output-r4` merge. Every runtime interface is game-independent.

## Explicit prerequisite commits

| Feature | Original source | Topic copy |
|---|---|---|
| HD/local-view pass transactions ([PR 727](https://github.com/RetroPortingToolKit/psxrecomp/pull/727) prerequisite) | `71ca2a71` | `ac081d1a` |
| Queued pass transactions and budget tests | `71cc5070`, `771ab7fd` | `5ca081a6`, `db5a7422` |
| Cached GL state and batched ring publication | `aeb4deaf` | `f6d573e5` |
| Render-thread sampler API used by that host-threading change | `c4d954b1` | `7fc93583` |
| Triangle record grouping | `feat/triangle-record-groups` at `6ea95b0f` | dependency merge `51236f17` |

These commits remain individually reviewable. The prior fork branch
`feat/render-pass-queued` includes unrelated fork integration history; it is
not a source base for this topic. Timed-slot/queued-pass merge resolution keeps
both nonblocking timed submission and GPU timer exclusion around slot waits.

## Async implementation and split receipt dependency

The source implementation commits from `int/output-r4` were ported individually:
`0aed27d8`, `6ba357d6`, `9e106322`, `3f69ae63`, `3ffb17f4`, `4e783032`,
`d5a340db`, `f0da52c9`, `9bcfec92`, `2217054e`, `de625ce1`, `ac00975d`,
plus the async grouping-boundary fix `cd857134`. Neither integration merge
`2f504863` nor `d2ca35de` is included.

`8c60c330` was split by ownership: GPU list replay/provenance and its tests are
`a84e93c1` here; context DMA capture/header/test/CMake changes are `eb69c9a2`
on `feat/threaded-contexts`. Integration-only duplicate tap/stub cleanup is
excluded. The provider API builds without contexts: a producer supplies words,
precision, ordered metadata and immutable memory. The context topic supplies
one such producer. Worker pool, draw service and game adapters are separate
integration dependencies, not hidden additions to this branch.

## Landing and verification

Merge the clean topic sources into the fork after integration checks. Never
merge `int/output-r4`. Minimal source refs are this async topic, the canonical
context/pool/service topics, and `feat/gpu-display-info-cache` (`3c151afd`). The
clean timestamp and triangle topics are already ancestors of this topic and
can be reviewed separately without importing them twice.

Standalone tests cover timestamp scheduling, private GPU provenance, bounded
ring admission, hidden-GL timed output, texture snapshot identity and actual
full-resolution presentation-source capture. The grouped ring corpus matches
native/high-resolution/wide/readback pixels with direct rendering and with its
off switch. Game capture equality and measured distinct-frame cadence are
integration evidence; standalone tests do not establish either result.

Off switches: `PSX_RENDER_THREAD_GROUPS=0` restores individual ring records;
`PSX_GPU_DISPLAY_CACHE=0` disables the separate display-layout cache. Optional
async output does not alter guest logic cadence.
