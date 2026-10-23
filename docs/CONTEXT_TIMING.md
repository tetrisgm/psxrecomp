# Context timing groundwork

`feat/context-timing` starts at upstream `2b511773`. It has no dependency on
the threaded-context topic and does not enable real-frame worker execution.

## Immutable capture

`psx_timing_snapshot_capture(cpu)` copies timing inputs on the main guest
thread between instructions. Its const view contains the published clock,
device deadline, deferred charges (including the value of a local accumulator,
never its pointer), scale/fraction/gates, load pipeline, multiply/divide and
GTE deadlines, cache-control latch and I-cache tags. An unresolved cache-enable
value stays unresolved. Capture does not initialize the cache, flush charges,
invoke guest read hooks or service devices. Callers release the snapshot when
all readers finish; the API has no restore/publication operation.

This is a process-local timing input, not a savestate or a complete execution
snapshot. Device state, pending interrupt delivery, memory-access timing
configuration and host call-stack continuation still need separate ownership.
Neither this capture nor the helper below makes a lean draw clone timed.

## State-only I-cache operations

`psx_icache_state_fetch(tags, address)` changes only the supplied tag array
and returns the existing model's unscaled CPU refill cost. Zero is a hit;
nonzero tells the caller to clear its selected load give-back before charging.
`psx_icache_state_isc_store` updates the supplied tags for existing BIU modes.
The caller owns enable/replay gates. These helpers do not inspect runtime
globals, advance clocks, service devices or apply guest-cycle scaling.

The main runtime uses the helpers behind its existing gates. Compile with
`-DPSX_ICACHE_STATE_HELPERS=0` to retain the previous implementation for
comparison or rollback. This refactor preserves that implementation's
documented cache-model limitations; it is not a new hardware-accuracy claim.

## Verification

On Windows MinGW64 and macOS Clang, the helper and retained implementation
match over 30,000 deterministic mixed fetch/IsC transitions: 94,231 CPU
cycles, identical tags and identical load give-back behavior. Both variants
also pass the existing fast-path and fetch-elision counterexample tests.
The snapshot test verifies that capture leaves live state unchanged and owns
its values after all source state changes. Tests retain assertions in Release.

Registered CTests: `psx_icache_state_0_test`, `psx_icache_state_1_test`,
`psx_timing_snapshot_test`; existing `psx_icache_fastpath_test` and
`psx_icache_elision_guard_test` remain applicable. No game build, regeneration,
save migration or performance acceptance is part of this topic.

The next step is context-owned load/stall accounting using the same cost
routines, followed by explicit device/IRQ rendezvous at the existing
observation points. A whole-root estimated delay cannot replace those events.
