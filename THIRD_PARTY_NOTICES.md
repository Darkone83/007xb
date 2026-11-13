# Credits and third-party notices

This page credits upstream work **identified in the supplied 007xb source archive**. It is an attribution inventory, **not** a substitute for the original file headers, bundled license texts, or a complete redistribution-rights review. Paths below reflect the intended public layout where the uploaded `007xb/` source tree is placed under `src/`.

## Project and upstream work

| Project / contributor | Contribution or relationship | Source and notice |
| --- | --- | --- |
| **Darkone83** | 007xb project direction, original-Xbox integration, Xbox-specific runtime/graphics work and project branding | This repository; verify file-by-file authorship of any modified upstream code |
| **Rare / original GoldenEye 007 creators** | Original Nintendo 64 game, game design and original game content | Rights retained by their respective holders; original content is not granted an open license by this project |
| **GoldenEye decompilation contributors** | Reconstructed N64 game source and game-structure reference | [n64decomp/007](https://github.com/n64decomp/007) (mirror of [kholdfuzion/goldeneye_src](https://gitlab.com/kholdfuzion/goldeneye_src)); source attribution must be preserved |
| **James Dansereau** | GoldenEye 007 PC port, platform adaptations, converter and pipeline lineage | [jkdansereau/goldeneye-pc-port](https://github.com/jkdansereau/goldeneye-pc-port); MIT for the scoped PC-port work, © 2026; `src/007xb/engine/LICENSE` and `NOTICE` |
| **Emill and MaikelChan** | Fast3D software graphics-port lineage | MIT © 2020; `src/007xb/engine/port/fast3d/LICENSE.txt` |
| **Silicon Graphics, Inc., MIPS Computer Systems, AT&T and related owners** | N64-era system headers, math/libultra code and associated notices retained in imported sources | For example `src/007xb/engine/include/PR/`, `include/assert.h`, `include/math.h`, `include/sgidefs.h`, `src/libultra/`; **rights/redistribution review required** |
| **Nintendo** | Some imported N64 SDK headers have Nintendo copyright statements; original platform and game-related trademarks | Example: `src/007xb/engine/include/PR/gs2dex.h`; verify redistribution status |
| **Istvan Novak / SpectNet.SpectrumEmu** | Comment descriptions cited in the imported Spectrum emulator header | `src/007xb/engine/src/game/spectrum_hw.h` credits © 2017 Istvan Novak and [SpectNet source](https://github.com/Dotneteer/spectnetide) as MIT-licensed reference text |
| **Original Xbox homebrew toolchain / RXDK authors** | Required build/runtime interfaces for the Xbox target | Toolchain and SDK terms are separate from this repository; identify exact redistributable components before a binary release |

**Additional decompilation documentation acknowledgments:** the upstream decompilation's README credits contributors to the reverse-engineering effort and GoldenEye/Perfect Dark technical documentation by Zoinkity. See the original upstream README for its full current acknowledgments.

## Archive and packer attribution

The `tools/` directory contains 007xb Asset Studio and `tools/007xb_converter_reference.zip`. The bundle contains converter source/reference metadata and an upstream PC-port `LICENSE`/`NOTICE`. The upstream notice also names other libraries and build tools **used by the parent PC-port/decomp project**, such as rabbitizer, armips and puff/gzip tools. **Do not claim they are all present in or linked into 007xb** merely because they appear in a borrowed notice; include any extra license texts only for components actually redistributed.

The asset studio requires a **user-provided clean US/NTSC-U GoldenEye ROM** and produces a single `007xb_assets.dat`. The source repository and downloadable tools should not distribute ROMs, extracted models/textures/audio or generated `.dat` files. ROM-derived metadata embedded in tooling needs a separate provenance check prior to release.

## Visual media and branding

- `images/logo.png` — project logo supplied for the README; confirm project use/distribution permission.
- `images/Darkone83.png` — Darkone83 branding supplied for the README; confirm project use/distribution permission.
- `images/demo.gif` — captured 007xb gameplay preview; validate that the footage is approved for sharing and acknowledge underlying GoldenEye game content.
- `src/007xb/titleimage.png` and `tools/branding/` — title/logo and embedded project branding assets; confirm provenance, especially when distributing the compiled XBE.

## Keeping notices intact

Preserve all original license texts and copyright headers inside source files. Add notices when introducing outside code or assets and record the upstream URL, version/commit (when known), author, license, changes, and affected paths in the pull request. If uncertain, **do not remove a notice or assume MIT applies**.

See [Licensing](LICENSING.md), the [copyright header audit](docs/COPYRIGHT_HEADER_AUDIT.md), and the [release checklist](docs/PUBLIC_RELEASE_CHECKLIST.md).
