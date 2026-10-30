# Release wait audit, 2026-10-30

The fixed Release passes the requested 120-second gate at Match display 18x
(4320 reference lines): 59.573 presentations/s, 60.003 VBlanks/s and zero audio
underruns. PGXP geometry, perspective texturing and precise culling remain on.
The owner’s applications were left running. Whole-machine CPU averaged 65.99%;
the runtime consumed 0.829 logical CPU, or 5.18% of this 16-thread machine.

The premise that the game burns 4–7 hardware threads is not supported by OS
thread-time accounting. The unfixed Release already used 0.807 logical CPU and
passed the throughput gate. The fixed run had substantially more background
load; its slightly higher total CPU does not establish an improvement or a
regression. No large spin-related CPU saving is claimed.

| Metric | Diagnostic | Release baseline | Fixed Release |
| --- | ---: | ---: | ---: |
| Report window, seconds | 120.074 | 121.016 | 121.027 |
| Presented images/s | 57.681 | 59.951 | 59.573 |
| VBlanks/s | 59.680 | 60.000 | 60.003 |
| Audio underrun counter delta | 26,726 | 0 | 0 |
| Total game CPU, one core = 100% | 97.47% | 80.74% | 82.91% |
| Total game CPU, machine share | 6.09% | 5.05% | 5.18% |
| Whole-machine CPU | 73.67% | 53.86% | 65.99% |
| Estimated other-process CPU | 67.58% | 48.81% | 60.81% |
| GPU packet ms/presentation | Not exposed by old binary | 3.448 | 3.467 |
| Whole-device GPU utilization | 51.04% | 49.24% | 46.83% |

| OS thread name (100% = one logical CPU) | Diagnostic | Release baseline | Fixed Release |
| --- | ---: | ---: | ---: |
| PSX emulation | 47.12% | 43.40% | 42.11% |
| PSX render | 27.09% | 19.57% | 20.69% |
| PSX native raster | 8.37% | 6.00% | 6.31% |
| PSX audio | 0.29% | 0.37% | 0.44% |
| PSX present | Unnamed | Unnamed | 0.35% |
| PSX input reclamation | Unnamed | Unnamed | 0.33% |
| PSX transform storage | Unnamed | Unnamed | 0.04% |
| psx-dbg-io | 0.013% | Absent | Absent |

The receipts include every observed thread, including zero-CPU and unnamed
threads, with its ID. The largest unnamed thread used 7.85%, 7.04% and 6.75%
of one core respectively; no attribution to driver or runtime is assumed.
The audio bridge increments its historically named `underrun_events` counter
once per output sample frame for which input is unavailable, so 26,726 is not
26,726 separate audible gaps. The diagnostic run contains a major stall;
background load and build settings differ, so its failure cannot be attributed
solely to debug tools.

## Measurement

Hardware: Ryzen 7 9800X3D (8 cores, 16 logical CPUs), RTX 4080 SUPER. Hidden SDL
window only, dummy audio, default mods plus the 60 FPS enhancement, LLE GPU batch
implementation and recompiled BIOS/guest execution. No HLE, PGO, trace files,
debug-server sampling or frame readback in the Release measurements. The
presentation-image diagnostic ring is off; PGXP is never disabled.

Production configuration uses Clang, `Release`, `-O2 -DNDEBUG`, ThinLTO
(`PSX_RUNTIME_IPO=ON`), `PSX_DEBUG_TOOLS=OFF`, Vulkan off, four compile jobs.
The Release baseline adds only environment measurement/startup-save support to
`3a9319fc`; the fixed executable includes the blocking-wait changes through
`e8803a3c`. Executable SHA-256 values and all observed thread IDs/names/times are
in [the receipts](RELEASE_WAIT_RECEIPTS.json).

The diagnostic comparator is the preserved `3a9319fc` executable from the prior
pooled-restage audit (SHA-256 starts `e53b5553`): Clang 22.1.8, Release `-O3`,
ThinLTO, debug tools on, host sampler explicitly on, trace files off. Its old
binary cannot perform a safe LLE environment restore or scripted menu input.
TCP drives its cold boot and makes a same-build grid save, then sets held Cross
before the measurement; there are no TCP requests during the measured window.
Its cadence and underrun counters are read from environment-enabled stdout
reports, and CPU is measured externally. Thus this is an operational
debug-versus-production comparison, not a same-compiler-flags isolated toggle.

Each Release executable cold-boots and creates its own private grid slot 9.
`PSX_PAD_SCRIPT` drives the existing smoke-test menu sequence and saves it at a
VBlank boundary. A separate process then uses `PSX_LOAD_SLOT=9`,
`PSX_HOLD_PADS=bfff`, `PSX_FRAME_REPORT_MS=1000`,
`PSX_GL_ASYNC_GPU_TIMER=1`, and `SDL_AUDIODRIVER=dummy`. Startup must print
`PSX_LOAD_SLOT=9 restored`; otherwise the harness rejects the run. After ten
seconds of warmup it collects 120 seconds of OS thread times and device load.
One-second report boundaries enclose a 121.016/121.027-second counter window.
The measured driving input is held Cross; this is not an autopilot lap gate.

Presented images means completed present-thread swap callbacks, not unique
pixel hashes or a physical scanout measurement. The hidden-window requirement
precludes proving physical 8K scanout. The internal render resolution is 18x
throughout both measured windows. VBlanks use the guest frame counter. Audio
underruns are cumulative callback counters differenced across the window; dummy
audio exercises transport/callback scheduling, not a physical sound device.

CPU percentages use user+kernel thread times from Windows via psutil, with
`GetThreadDescription` names. 100% is one logical CPU; divide the process sum by
16 for Task Manager-style machine share. No thread is suspended to collect
these measurements. Unnamed threads retain their OS IDs in the receipts; their
roles are not guessed. Device GPU utilization comes from `nvidia-smi` and
includes the owner’s other GPU applications.

## GPU

The Release baseline averages 3.448 ms of packet GPU timestamp span per
presentation, including 0.958 ms in presentation. The fixed Release averages
3.467 ms, including 0.969 ms in presentation. No GPU telemetry samples drop.
Device utilization averages 49.24% baseline and 46.83% fixed (peaks 67% and 73%).
The fixed window contains only 4.011 ms of aggregate present-slot fence wait,
and 72.602 ms of aggregate restaging across 44 restages. Neither explains a
frame-rate bottleneck. Timestamp spans can include command-feed gaps and GPU
waits; they are not isolated shader execution time. The average is below the
requested 8 ms investigation threshold.

## Wait inventory and changes

| Thread or path | Finding and final behavior |
| --- | --- |
| Emulation pacer | Per-thread high-resolution waitable timer already supplied the bulk sleep; final spin reduced from 500 to 200 microseconds. Catch-up preserves guest cadence. |
| Render/frame owner | Existing retained publication event plus high-resolution deadline timer blocks on work; no replacement necessary. |
| Present worker | Existing condition variable blocks on queue arrival and timed eligibility. Added OS thread name. |
| Native raster | Existing retained event blocks the worker queue. Checkpoint destruction now waits on a condition variable instead of repeatedly yielding. |
| Transform storage | Replaced 1 ms reclamation polling with a retained event signalled on consumption, return and shutdown. Added OS thread name. |
| Input reclamation | Already event driven; added OS thread name. |
| Seam worker | Replaced `SDL_Delay(0)` baseline polling with condition-variable publication/deadline waits. Shutdown wakes it. |
| Renderer shared publications | Seam baseline/ready, dynres stats and frame-generation cost spin/seqlock readers now use blocking mutexes. |
| Overlay/HUD callbacks | Callback completion spins replaced with condition variables; HUD publication uses a blocking mutex. |
| Diagnostic image mailbox | Replaced the image-ring spinlock with a blocking mutex. Ring remains disabled in these performance runs. |
| Test render barrier | `rt_drain` completion now uses a condition variable. This barrier is test-only and did not cause production race CPU cost. |
| Audio | SDL invokes the audio callback; there is no runtime audio-feed spin thread to replace. OS-accounted callback CPU remains below 0.5% of one core in Release. |
| Host sampler | Debug builds now require `PSX_HOST_SAMPLER=1`; absent in Release. Its periodic sampling already waits on a timer, but suspension perturbs the guest. |
| Debug phase sampler / fatal debug loop | Debug-listener-only 1 ms timed polling; not started in the measured Release. Periodic sampling is intentional diagnostic work. |
| Legacy draw-service pacing adapter | Supports timer-sliced optional service polling, but no production runtime caller registers that service. Not active in these runs. |
| Rewind / mailboxes | Existing retained events, mutexes and completion wakes; no persistent polling loop. |

This removes runtime-owned busy waits on another thread’s progress. Bounded
atomic queue/CAS retries and executing actual queue work are not idle loops.
Driver-internal waits cannot be changed in this repository.

## Startup restore correction

The old environment load request was rejected in LLE mode because an
interactive restore could longjmp across a host fiber stack. Release compiled
out the refusal message, making a cold boot look like a slot-based run. Those
initial probes were discarded.

The new startup-only restore uses the existing complete state loader before
the scheduler has ever run. It resumes the saved PC/register state through the
normal dispatch entry and retains the restore-edge IRQ check. A scheduler that
has already run rejects this API. Interactive cross-fiber LLE restore remains
rejected. No guest state or savestate bytes are patched.

`PSX_PAD_SCRIPT` is an opt-in plain-text sequence of
`<absolute VBlank> <hex active-low pad word> <save-slot or -1>` rows. It is read
once and applied after live input sampling, permitting same-build grid saves
without a debug server. It has no effect when unset.

## Verification

Release build succeeds. Focused CTests pass: `frame_pacing_test`,
`native_raster_test`, `host_overlay_test`, `frame_mailbox_test`,
`presentation_wake_test`, and `savestate_title_policy_test`.
`test_restore_edge_irq_check.py` passes. The hidden OpenGL packet fixture
(`gl_frame_packet_test`) passes with PGXP enabled, including production surface
coverage. Same-build grid creation and the 120-second Release race pass.
All 22 registered R4 root tests pass; the PGXP-off boot test is excluded.

Raw local logs, per-second OS thread samples, private saves and immutable
executables remain under the R4 validation worktree’s `analysis/release-waits/`.
Game data, savestates and executable payloads are not committed.
