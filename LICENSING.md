# Licensing and redistribution

> **Draft for public-release review — not a project-wide license grant.**
>
> This repository includes material from projects with different provenance and licensing terms. **Do not label the entire tree MIT-licensed.** A license for the original 007xb-specific contributions has **not yet been selected or authorized by the maintainer**. Do not interpret the presence of an upstream license file as permission to redistribute unrelated game code, headers, assets, XBE binaries, or this repository as a whole.

## Scope by component

| Material | Relevant terms / current status |
| --- | --- |
| Original 007xb Xbox host, NV2A/Direct3D8 backend, tools, branding and documentation | Maintainer to select and publish a license for material they have rights to license; **not specified by this draft**. Some files derive from upstream code and require a file-by-file provenance review. |
| GoldenEye 007 PC-port-specific work | James Dansereau's MIT license applies to the **original PC-port work identified by its upstream `NOTICE`**; see `src/007xb/engine/LICENSE` and `src/007xb/engine/NOTICE`. It is not a blanket license for the inherited game source. |
| Original GoldenEye 007 decompilation and inherited source/metadata | [n64decomp/007](https://github.com/n64decomp/007) is an upstream reconstruction; no universal upstream redistribution license has been established for all inherited game source. **Rights review required** before distribution. |
| Fast3D-derived work | Portions are accompanied by an MIT license crediting Emill and MaikelChan; keep `src/007xb/engine/port/fast3d/LICENSE.txt` and review the provenance of adapted sections. |
| Silicon Graphics / MIPS / AT&T / Nintendo N64 headers and libultra-derived files | Some files contain restrictive copyright or nondisclosure notices. **Retaining these notices does not itself grant redistribution rights.** Review before publishing these files. |
| Original GoldenEye content | Game ROM, music, graphics, text, models and other original game materials remain with their respective rights holders. Users must provide their own ROM. Do not upload a ROM, extracted assets, or generated `007xb_assets.dat`. |
| Original Xbox SDK / RXDK tools and binary dependencies | Subject to their own terms. Verify source/build dependency and precompiled XBE redistribution rights independently. No SDK binaries should be added to the repository without review. |
| Project logos and gameplay GIF | Confirm the permission and ownership of each image and the embedded game footage before publication; see [third-party notices](THIRD_PARTY_NOTICES.md). |

## Existing upstream license material

The supplied source archive includes these license/notice files. Preserve them with their original contents when reorganizing `007xb/` under `src/`:

- `src/007xb/engine/LICENSE` — the PC port's MIT license, copyright © 2026 James Dansereau.
- `src/007xb/engine/NOTICE` — explicitly limits the PC-port license's scope and lists other dependencies.
- `src/007xb/engine/port/fast3d/LICENSE.txt` — MIT license, copyright © 2020 Emill and MaikelChan.
- `tools/007xb_converter_reference.zip` contains its own upstream `LICENSE` and `NOTICE`; keep and review those as well.

Do not remove existing header-level notices. A separate [third-party inventory](THIRD_PARTY_NOTICES.md) and [copyright header audit](docs/COPYRIGHT_HEADER_AUDIT.md) document credit and provenance; it does not supersede the actual licenses.

## Before making a public release

1. Confirm which source files are original 007xb work versus inherited or modified upstream work; choose a license **only** for works the maintainer has the right to license.
2. Review `src/007xb/engine/include/` and `src/007xb/engine/src/libultra/` for third-party/proprietary notices, and decide what can lawfully be published.
3. Review the bundled converter metadata and source, and the compiled `default.xbe` (including embedded third-party code) for permitted redistribution.
4. Confirm the rights to publish `images/logo.png`, `images/Darkone83.png` and `images/demo.gif`.
5. Exclude ROMs, `.dat` archives generated from ROMs, extracted assets, SDK binaries where not redistributable, private logs and machine-specific build settings.
6. Preserve attribution and license texts in all distributions and document any further third-party components discovered during the audit.

See [public-release checklist](docs/PUBLIC_RELEASE_CHECKLIST.md). For unresolved rights questions, seek qualified legal advice before distribution. This document is a provenance checklist, not legal advice.
