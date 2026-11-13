# Contributing to 007xb

Thanks for helping improve this original-Xbox port of GoldenEye 007. The project is under active development. Please keep changes **small, traceable and testable**, with special care for original hardware and compatibility with the N64 gameplay engine.

## Before making changes

- Read the [technical findings](docs/TECHNICAL_FINDINGS.md) and check for an existing issue or discussion before starting substantial work.
- Do not upload a GoldenEye ROM, `007xb_assets.dat`, extracted game assets, game saves, proprietary SDK files, or other files without redistribution permission.
- Preserve original source attribution and copyright notices. This repository contains material with **different licenses or unresolved rights**, so review [Licensing](LICENSING.md) and [third-party notices](THIRD_PARTY_NOTICES.md) before contributing code from another project.
- Keep original-Xbox game source in `src/`, tool scripts and build commands in `tools/`, documentation in `docs/`, visual previews in `images/`, and user-deployable XBE material in `xbe/` when distribution is authorized.

## Development guidelines

- Target the **original Xbox**, using the existing classic RXDK / Xbox Direct3D8 integration. Avoid dependencies on modern C++ runtimes, exceptions, RTTI or unsupported Direct3D features.
- Separate Xbox platform/backend changes from original game logic. Use the [original N64 decompilation](https://github.com/n64decomp/007) to check gameplay fidelity and the [PC port](https://github.com/jkdansereau/goldeneye-pc-port) for portable implementation references. PC-specific changes do not automatically reflect original N64 behavior.
- Avoid changing simulation ticks or RNG consumption when optimizing rendering, input or audio. Replay/determinism fixes should include comparative evidence and recorded first-divergence data.
- Be conservative with graphics-state caching, culling and shader changes: retain gunbarrel, weapon models, transparency, large/scissor-crossing polygons, and display-list semantics.
- Do not put required GPU/device behavior behind a diagnostics toggle. Logging should remain issue-focused and bounded.
- Keep the ROM packer's **one-ROM-input, one-`007xb_assets.dat`-output** contract. Intermediate files and logs belong in `tools/.cache/`; not `xbe/data/` or the checked-in repository.

## Pull request checklist

1. Explain the problem and what changed. List modified files, any files that must be removed, and the intended release scope.
2. Describe the baseline used and provide reproducible steps or minimal test data that **does not contain original game assets**.
3. Build/test in the actual supported environment where possible; distinguish **host tests** from **real-Xbox hardware verification**.
4. For rendering changes, check title/gunbarrel, normal gameplay, weapons, models, transparent effects and a large level such as Runway.
5. For audio/timing changes, check music continuity, controller input and replay timing.
6. For asset tooling, run the tests documented in [`tools/README.md`](tools/README.md); confirm `data/` contains only `007xb_assets.dat` after a normal build.
7. If borrowing code, record its origin, license and any modifications in the PR and update [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) when appropriate.

Bug reports are most useful with the hardware configuration (including RAM size), whether the test was on real Xbox or xemu, build revision, exact steps, expected/actual results, and a **short excerpt around the failure**. Please do not attach unbounded logs or ROM data.

## Licensing of contributions

**A project-wide license has not yet been selected.** Do not submit third-party material unless you can document the required rights and its compatibility with the eventual repository terms. Maintainers may need to discuss permissions before accepting changes. The project does not obtain rights to third-party game material simply by accepting a contribution.
