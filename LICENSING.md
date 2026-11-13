# Licensing and redistribution — status and scope

**Status: rights review pending. This file is documentation, not a license or a public-release authorization.**

007xb combines independently written Xbox integration, MIT-licensed port components, reconstructed GoldenEye source and metadata of unresolved licensing status, N64-era SDK/libultra files bearing restrictive notices, and user-supplied or embedded artwork. **No single license currently covers this entire repository.** The original-Xbox binary also requires its own distribution review.

## The rule

A copyright credit names a contributor. A license or other legal basis establishes what can be copied, modified, and distributed. **Crediting a rights holder alone is not permission.** Equally, the absence of a license header does **not** make material public domain. Publication on GitHub is not a blanket grant of open-source permissions.

## What is established versus unresolved

| Component | Scope / evidence | Working release status |
| --- | --- | --- |
| Original Xbox-specific additions and original documentation | May be licensed by their actual rights holders after authorship and any derivative portions are confirmed | **License choice pending; not a license grant** |
| GoldenEye PC-port-specific original code | MIT, copyright © 2026 James Dansereau; original `engine/LICENSE` and scope-limiting `engine/NOTICE` | **Keep license and notice; verify each copied file's scope** |
| Fast3D original work in its licensed lineage | MIT, © 2020 Emill and MaikelChan; `engine/port/fast3d/LICENSE.txt` | **Keep license; verify lineage for adapted files** |
| GoldenEye decompilation-derived `engine/src/`, `engine/include/`, `engine/assets/` and other inherited files | Original upstream decomp has no identified blanket license, as explicitly stated in the PC-port `NOTICE` | **Not cleared for a general public license** |
| N64 SDK and libultra-derived files | 79 SGI copyright-notice files, 77 of them flagged with restrictive wording; 53 of the SGI-notice files are listed as Visual Studio C/C++ compilation units | **High-priority rights review; do not assume attribution suffices** |
| ROM-derived/static graphics, model, font and tables | Some game graphics/font arrays are embedded in source, in addition to the user's separately generated `.dat` | **Review/remove/replace or obtain appropriate clearance** |
| ROM-only asset packer | Bundled converter reference combines MIT-covered PC tools with decomp-derived image/model references | **Tooling and bundled metadata need separate review** |
| `default.xbe` | Compiled game code, graphics/data and potentially SDK/runtime components | **Not cleared by a source license; separate binary gate** |
| `images/demo.gif`, `logo.png`, `Darkone83.png`, `titleimage.png` | Confirm ownership, embedded GoldenEye imagery, branding, and relevant trademark issues | **Approval/provenance pending** |

## License locations to preserve (source tree moved to `src/`)

- `src/007xb/engine/LICENSE` — MIT, © James Dansereau, **scope limited by the adjacent `NOTICE`**.
- `src/007xb/engine/NOTICE` — defines the PC port's original-work scope and excludes inherited decompilation from its MIT grant.
- `src/007xb/engine/port/fast3d/LICENSE.txt` — MIT, © Emill and MaikelChan.
- `tools/007xb_converter_reference.zip` — contains copies of upstream `LICENSE` and `NOTICE`; those terms apply only to the covered original work, not all 522 enclosed entries.

**Do not replace these upstream license files with a new root MIT license** or alter their scope. Original 007xb-authored code could later receive its own clearly scoped license after ownership and contributor permissions are settled. Until then, this document is a notice of unresolved rights rather than a grant of permission.

## Release paths

| Intended release | Required clearance before posting |
| --- | --- |
| Documentation only | Original documentation authorship; permission/appropriate legal basis for any included images or footage |
| Asset Studio only | Audit converter scripts **and** bundled decomp-derived model/image metadata, branding, and included notices; no ROM or generated `.dat` |
| Engine source | Resolve or replace decomp-derived code, SDK/libultra content, embedded game data, and source contributions; counsel's rights analysis |
| Precompiled XBE | All source gates **plus** binary linkage, runtime/SDK, title graphics, and game-code distribution analysis |

See [the detailed review](docs/LEGAL_RELEASE_REVIEW.md), [rights-clearance tracker](docs/RIGHTS_CLEARANCE_TRACKER.md), [copyright header audit](docs/COPYRIGHT_HEADER_AUDIT.md), and [third-party notices](THIRD_PARTY_NOTICES.md). The public-release checklist is at [docs/PUBLIC_RELEASE_CHECKLIST.md](docs/PUBLIC_RELEASE_CHECKLIST.md).

## Statements not made by this project

The current review does **not** say the entire source tree is MIT-licensed, that ROM ownership resolves all extraction/distribution issues, that a non-commercial fan project is automatically exempt from copyright or trademark law, or that the upstream project's publication establishes independent permission for this port.

The appropriate legal basis for some reverse-engineered portions may depend on facts and jurisdiction; **no conclusion on fair use or interoperability exemptions is made here**. Obtain advice from a qualified IP attorney before releasing unresolved source or binaries.

## Primary references

1. PC port source and scoped `NOTICE`: https://github.com/jkdansereau/goldeneye-pc-port — `NOTICE`, `LICENSE`, and `README.md`.
2. Original decompilation: https://github.com/n64decomp/007 — published without a repository-wide license identified by this review.
3. GitHub's no-license guidance: https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/licensing-a-repository
4. United States Copyright Act, 17 U.S.C. §106: https://uscode.house.gov/us/usc/119-124/t17/s106/
5. 17 U.S.C. §1201(f) contains a conditional interoperability provision; it is **not** general authorization to redistribute all decompiled code: https://uscode.house.gov/us/usc/119-118/t17/s1201/

**Reference snapshot:** November 12, 2026. **Not legal advice.**
