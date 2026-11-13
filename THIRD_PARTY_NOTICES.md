# Third-party notices and attribution

**Attribution record, not a grant of redistribution rights.** Retain original file-level notices. Do not edit them into a project-wide license or imply the individuals listed here authorized the 007xb distribution.

| Upstream / author | Contribution and evidence | Required follow-up |
| --- | --- | --- |
| Original GoldenEye 007 developers, Rare and other applicable game rights holders | Original game design/code/content reproduced or reconstructed in decomp lineage | Confirm applicable rights and obtain appropriate legal advice/permission before distributing covered material |
| GoldenEye decompilation contributors; Larry Ficken (kholdfuzion) and other upstream contributors | https://github.com/n64decomp/007 and original project acknowledgments | Preserve upstream acknowledgments, review lack of comprehensive license and individual contributions |
| James Dansereau | GoldenEye PC port — MIT **original port contributions**, © 2026; `src/007xb/engine/LICENSE` and adjacent `NOTICE` | Retain exact MIT text and NOTICE; verify code is within its stated scope |
| Emill and MaikelChan | Fast3D original licensed lineage — MIT, © 2020; `src/007xb/engine/port/fast3d/LICENSE.txt` | Preserve text; trace adapted portions and other upstream contributors if present |
| Silicon Graphics, Inc. | N64 SDK/libultra notices, including proprietary restriction statements (see audit) | Rights review / replacement / permission; retain notices, do not claim they are open-source |
| Nintendo | SDK header `src/007xb/engine/include/PR/gs2dex.h`; game and platform trademark questions separately | Review redistribution and use rights |
| MIPS Computer Systems, AT&T, UNIX System Laboratories | Notices in imported include files; full list in copyright audit | Preserve notices and verify applicable rights |
| Istvan Novak / SpectNet.SpectrumEmu | `src/007xb/engine/src/game/spectrum_hw.h` cites MIT-licensed reference comments (2017) | Verify copied portions and retain attributable license notice for any reproduced material |
| Darkone83 / original 007xb contributors | Xbox platform/runtime, NV2A work, original project branding where independently created | Verify each contributor's authorship and agree on a scoped license; never relicense inherited code |
| Team Resurgent / RXDK and relevant Xbox toolchain rights holders | Build dependencies referenced in Visual Studio project (`RXDK_LIBS`) | Obtain exact toolchain provenance and redistribution terms, particularly for any linked XBE components |
| Game footage/logo/title art contributors and underlying IP holders | `images/` and `src/007xb/titleimage.png`, plus original visual assets included in game | Confirm authorship, third-party image content, and use of marks before public release |

## Source notices that must stay intact

- `src/007xb/engine/LICENSE` and `src/007xb/engine/NOTICE`.
- `src/007xb/engine/port/fast3d/LICENSE.txt`.
- Original copyright lines throughout `src/007xb/engine/include/PR/`, `engine/src/libultra/`, `engine/src/libultrare/`, and other source files.
- `tools/007xb_converter_reference.zip`'s own `LICENSE` and `NOTICE` remain intact if the archive is distributed, **but those do not license the decomp-derived metadata contained in it**.

The PC-port `NOTICE` mentions rabbitizer, armips, puff/zlib, gzip and glad as dependencies **of the upstream project**. This audit has not established that each is present in or linked into 007xb. Do not list these as 007xb vendored binary dependencies without confirming which pieces actually ship.

## Specific media cautions

- The game title and `007`/GoldenEye-related marks may raise trademark/branding questions independently of source-code licensing.
- `images/demo.gif` shows game footage; original capture by the project does not automatically give exclusive rights to the underlying game visuals.
- `images/logo.png`, `images/Darkone83.png`, `src/007xb/titleimage.png` and `tools/branding/` need a provenance entry stating who created each, what material was used, and what distribution permission exists.

This file is a truthful credits map, **not a certification**. See [LICENSING.md](LICENSING.md), [docs/LEGAL_RELEASE_REVIEW.md](docs/LEGAL_RELEASE_REVIEW.md), and [docs/COPYRIGHT_HEADER_AUDIT.md](docs/COPYRIGHT_HEADER_AUDIT.md).
