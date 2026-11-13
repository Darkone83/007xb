# Building and testing 007xb

These are **development notes**, not a replacement for the existing scripts and project files under `tools/` and `src/`.

## Source tree (intended public layout)

- `src/007xb.slnx` — Visual Studio solution from the uploaded project.
- `src/007xb/007xb.vcxproj` — classic original-Xbox RXDK project; includes post-build XBE and XISO steps.
- `src/007xb/engine/` — inherited GoldenEye/PC-port code and the N64 compatibility layer.
- `src/007xb/xb_*.cpp`, `src/007xb/ge_*.cpp` — Xbox host, NV2A backend, audio, input and runtime integration.
- `tools/` — ROM packer, converter reference, GUI/CLI, launchers and any tool-side build utilities.
- `xbe/` — **release artifacts only**, if the maintainer chooses and is permitted to distribute them.

The original uploaded archive used `007xb.slnx` at the root and `007xb/` for source. **When moving it under `src/`, retain the relative solution-to-project relationship**, or adjust project paths accordingly. This document does not claim that a newly rearranged tree has already been built.

## Environment

The supplied project config references the `RXDK_LIBS` environment variable and Xbox-specific build programs, including `patchSubsystem.exe`, `imagebld.exe`, `bundler.exe` and `xdvdfs.exe`. It uses Win32/x86 configurations. Build under a suitable Windows development environment with a compatible classic RXDK setup. **No SDK binaries or platform libraries are supplied by these documentation files.** Check the rights and terms governing any SDK/toolchain before redistributing build outputs.

The Visual Studio project post-build steps generate an `.xbe`, copy it as `default.xbe` into a build folder, and may generate an XISO. Those are **developer build artifacts**; the final deployable `xbe/` layout is described in the root README.

## ROM asset preparation (end users and developers)

The verified Asset Studio v0.6 source workflow is described in [`tools/README.md`](../tools/README.md). It requires **only a clean US/NTSC-U GoldenEye N64 ROM**, which the user supplies. On Windows, run `tools/Start 007xb.cmd`, select the ROM, and build. The packer outputs only `data/007xb_assets.dat`. Copy this to `xbe/data/007xb_assets.dat` before deployment.

The upstream source bundle used for conversion and intermediate artifacts remain within `tools/` and its cache. An optional standalone Windows EXE builder is at `tools/Build Windows EXE.cmd`; the original validation notes state Windows freezing was **not verified** in v0.6. Never commit converter cache or generated ROM-derived data.

## Test matrix

| Area | Minimum verification |
| --- | --- |
| Build / startup | Compiles for classic RXDK, launch succeeds on real Xbox, no fatal error, startup splash/hand-off behaves |
| Intro / rendering | Gunbarrel, logo, weapon/character models, transparency, alpha/fog edge cases |
| Performance | Compare the same stage/position and resolution; large stages (Runway), fast pans, onscreen and offscreen triangle cases |
| Audio | Title music, gameplay music, sound effects under rendering load, underruns (only if measured) |
| Inputs / gameplay | Controller mappings, pause/menu, level entry/exit, saves |
| Attract demos | Record first RNG-check failure separately from normal-gameplay regressions; demo RNG remains an open issue |
| Tooling | GUI/CLI tests, one-file `.dat` output, SHA/hash validation, no ROM-derived files in tracked tree |

Existing log output is capped by the current Xbox logging implementation; supply concise failure windows rather than long continuous diagnostics.

## Hardware status

The current work was validated by the maintainer on **real original Xbox hardware**. The available trace showed **128 MiB total physical RAM**; this document does not claim a verified 64 MiB compatibility level. Host test harnesses are useful for functional regressions but do not replace real NV2A/DirectSound testing.
