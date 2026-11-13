# 007xb technical findings and development record

**Status:** Work in progress / Phase 2 real-hardware playtesting (November 2026). This is a concise engineering handoff grounded in the supplied 007xb source snapshot, earlier source patches and maintainer test reports. It is **not a claim of campaign-wide validation**. For installation, use the [root README](../README.md); for contributor rules, see [CONTRIBUTING.md](../CONTRIBUTING.md).

## Architecture

007xb compiles the reconstructed GoldenEye 007 game logic from the [N64 decompilation](https://github.com/n64decomp/007), using portable code and metadata from the [PC port](https://github.com/jkdansereau/goldeneye-pc-port), against an original-Xbox host. It is a **native game-port effort**, not a conventional N64 emulator. Xbox-specific bridges implement original scheduler/libultra-facing behavior, controller input, filesystem, graphics and audio.

| Subsystem | Relevant code in the uploaded tree | Notes |
| --- | --- | --- |
| Launcher and asset loader | `src/007xb/main.cpp`, `xb_archive.*`, `xb_ge_runtime.*` | Opens `D:\data\007xb_assets.dat`; runs the original engine through platform glue |
| Native scheduler | `xb_ge_os.*`, `ge_original_sched_platform.cpp`, `engine/src/boss.c` | Original thread/task semantics mapped to Xbox threads and a graphics worker; replay scheduling differs from the original N64 loop |
| Graphics / Fast3D | `engine/port/fast3d/gfx_pc.cpp`, `xb_ge_fast3d.*`, `ge_nv2a_combiner.*`, `xb_render.*` | Interpret original game display lists; submit through Xbox Direct3D8/NV2A shaders |
| Audio | `xb_ge_audio.*`, `engine/src/audi.c`, `engine/src/libultra/audio/` | Original synthesizer/mixer path with Xbox DirectSound output |
| Input | `ge_original_input.*`, `ge_native_input.h`, `engine/src/game/` | Original controls bridged to Xbox input |
| RNG / demos | `engine/port/src/random.c`, `engine/src/game/ramromreplay.c`, `engine/src/game/chraction.c`, `gun.c`, `gunfire.c`, `explosion.c` | Original RNG transformation retained; deterministic demo playback remains unresolved |
| Data build | `tools/007xb Asset Studio.pyw`, `tools/xb_studio_pipeline.py`, `tools/007xb_assets.py` | User ROM becomes one packed archive; intermediates stay in tool cache |

## What has been demonstrated on real hardware

- The maintainer restored a known-working graphics build after an over-broad diagnostic cleanup. **Gunbarrel, 3D models and normal gameplay rendered again** on real Xbox hardware.
- The Fast3D/NV2A path now uses cached renderer state and shader/material reuse, with a conservative **early-out for fully offscreen triangles**. The maintainer reported modest gains on large stages such as **Runway**. No universal frame-rate improvement percentage is established.
- Certain original N64 combiners require more varying alpha inputs than the Xbox vertex format provides. The host originally stopped with `FATAL Combiner needs more varying alpha channels than Xbox FVF provides`. A **compatibility fallback** allowed continued execution on the maintainer's hardware. It approximates the excess alpha varying per triangle, so visual fidelity of some materials still needs targeted testing.
- The renderer supports the necessary native Xbox execution/graphics path for ongoing playtesting. Support across every stage, video mode, RAM configuration and Xbox revision has **not** been fully established by the supplied evidence.

## Important regression lesson: debug text versus GPU startup state

An attempted quiet-release cleanup disabled `GE_XBOX_STARTUP_CHECKPOINTS` and inadvertently compiled out startup behavior as well as diagnostic text. Gunbarrel and models disappeared. Restoring the pre-cleanup source brought rendering back, after which **only the three startup `DrawText` calls** were removed without touching device initialization, handoff, scene submission or state restore.

**Rule:** debug/logging flags must not gate essential runtime state transitions. Preserve complete, known-working GPU initialization and frame lifecycle before optimizing or removing diagnostics.

## Fast3D and performance findings

- Renderer CPU time matters on the single-core Xbox. The frontend and backend perform material evaluation, vertex conversion, shader lookup, primitive clipping checks and Direct3D submission. Improvements should avoid changing original display-list order and visible GPU output.
- Fast3D shader reuse, NV2A material caching, hardware scissor and texture-state optimizations were developed across incremental hardware test passes; regressions were reverted rather than hidden.
- The conservative offscreen optimization moves **an existing provably-offscreen rejection** ahead of per-triangle shader/constant setup. It does **not** add a new occlusion algorithm and must not discard scissor-crossing or viewport-enclosing triangles.
- The host test suite reported byte-identical visible vertex and GPU command output against its previous working baseline in covered tests. That does not prove every game-state or every Xbox shader is visually identical; real hardware remains the authority.
- The material alpha fallback is intentionally approximate. Check smoke, transparent surfaces, fog, muzzle effects and model/weapon overlays when changing combiners.

**Next performance candidates (not yet implemented as part of this documentation):** reducing redundant vertex transformations and render-state chatter, and measuring draw-call/texture-upload costs on representative stages. Make each optimization independently reversible.

## Audio and game scheduling

Original GoldenEye synthesizes audio through its own engine; Xbox output uses DirectSound. Brief music stutter was observed under graphics load. In the uploaded source, `xb_ge_audio.cpp` uses **22,050 Hz, stereo, 16-bit** output. `xb_ge_os.cpp` contains an option to elevate the native audio synthesis thread above normal priority, restoring some of the intended original thread-priority distinction.

**Unverified:** whether the priority change completely eliminates music stutter under heavy 720p gameplay. If stutter persists, next assess actual buffer fill/underrun counters and worker scheduling, with **bounded** instrumentation only; do not alter the game clock to mask audio starvation.

## Replay RNG — parked, not solved

The RNG implementation was checked against the original N64/decomp and PC-port algorithm; the captured Xbox 64-bit RNG states reproduced from their seeds and draw counts. Original RAM-ROM replay packet RNG bytes/checksums also matched the extracted recordings. The remaining mismatches therefore likely involve **which code paths consume RNG, how many times, and in what order** rather than an obviously incorrect PRNG transform.

Source and failure-log investigations identified the following conditional consumers: `gun.c` (weapon sway), `gunfire.c` (muzzle/casings), `chraction.c` and `chrai.c` (AI decisions), `explosion.c` (smoke/effects), `player.c` (player shuffling), and occasionally models/options. These are **suspect execution paths**, not individually confirmed root causes.

The Xbox has an additional recorded-demo packet deadline/wait path that differs from N64/PC scheduling. A frame gate was trialed, but the maintainer reports that demo RNG remains unresolved. **Do not claim deterministic replay, apply arbitrary RNG advances, or silently change simulation time.** Continue the investigation only when reprioritized, using the original decomp as behavior reference and the PC port as a portability reference.

## Logging and diagnostics

Earlier builds generated very large Xbox logs. The current source caps the trace at **256 KiB** and attempts to focus on actionable failures. Avoid continuous per-frame logging, and keep detailed RNG caller histories off unless diagnosing one reproducible desync. Preserve short fatal context and first-failure checkpoints. Do not let trace output alter GPU handoff or overwhelm the hard drive during playtests.

## Asset pipeline

The v0.6 asset tool expects a user-supplied clean US/NTSC-U revision 0 ROM, normalizes its byte order, validates SHA-1, and produces **one** runtime file: `data/007xb_assets.dat`. Archive build intermediates remain in `tools/.cache/`. The v0.6 validation report records a **17,649,664-byte** output with six archive entries, byte-for-byte matching a previously working archive. Windows frozen-EXE deployment is not yet verified by that report.

The runtime looks for `D:\data\007xb_assets.dat` adjacent to its XBE; no loose extracted data directory is required for the active Phase 2 entry point. See [`tools/README.md`](../tools/README.md) and [`tools/docs/ROM_ONLY_PACKER_V06_VALIDATION.md`](../tools/docs/ROM_ONLY_PACKER_V06_VALIDATION.md).

## Current known issues and testing gaps

| Area | Status |
| --- | --- |
| Boot, gunbarrel and gameplay rendering | Working in maintainer's tested hardware scenario |
| NV2A alpha overflow | Compatibility fallback works on hardware; visually approximate in uncommon materials |
| Large-stage performance | Improved subjectively on real hardware (not an independently measured FPS guarantee) |
| Music | Slight stutter observed; audio priority mitigation exists, full resolution unconfirmed |
| Recorded-demo RNG | **Known unresolved**; not a claim that every demo can finish |
| 64 MiB Xbox compatibility | Not established by the available hardware reports; recorded tests show a 128 MiB console |
| Windows stand-alone packer executable | Build script supplied; Windows distribution smoke test still required |
| Public source/XBE licensing | Not fully cleared; consult [licensing](../LICENSING.md) and [release checklist](PUBLIC_RELEASE_CHECKLIST.md) |

## Evidence boundaries

This document differentiates **maintainer-observed real-hardware behavior**, **source-code analysis**, and **host-side regression tests**. It is meant as a living findings record and should be updated when new hardware measurements or confirmed bug fixes supersede these observations. When documenting a performance change, include the stage, location, resolution, build revision and hardware details; don't assign gains to a specific optimization without a controlled A/B test.
