# Licensing and redistribution — status and scope

**Status: MIT adopted for independently authored 007xb contributions whose copyright is controlled by Darkone83. Other rights reviews remain pending. This document is not authorization to redistribute the complete source tree or a compiled XBE.**

007xb combines independently written Xbox integration, MIT-licensed port components, reconstructed GoldenEye source and metadata of unresolved licensing status, N64-era SDK/libultra files bearing restrictive notices, and user-supplied or embedded artwork. The root [MIT LICENSE](LICENSE) covers only original work Darkone83 has authority to license (and other contributions expressly offered under MIT by their rights holders). **It does not convert the complete repository into MIT-licensed material.** The original-Xbox binary still requires its own distribution review.

## The rule

A copyright credit names a contributor. A license or other legal basis establishes what can be copied, modified, and distributed. **Crediting a rights holder alone is not permission.** Equally, the absence of a license header does **not** make material public domain. Publication on GitHub is not a blanket grant of open-source permissions.

## What is established versus unresolved

| Component | Scope / evidence | Working release status |
| --- | --- | --- |
| Independently authored Xbox-specific additions and documentation controlled by Darkone83 | Root `LICENSE` (MIT), copyright © 2026 Darkone83; this covers only the original portion Darkone83 has authority to license | **MIT grant applies to this work only**; confirm authorship and derivative portions |
| GoldenEye PC-port-specific original code | MIT, copyright © 2026 James Dansereau; original `engine/LICENSE` and scope-limiting `engine/NOTICE` | **Keep license and notice; verify each copied file's scope** |
| Fast3D original work in its licensed lineage | MIT, © 2020 Emill and MaikelChan; `engine/port/fast3d/LICENSE.txt` | **Keep license; verify lineage for adapted files** |
| GoldenEye decompilation-derived `engine/src/`, `engine/include/`, `engine/assets/` and other inherited files | Original upstream decomp has no identified blanket license, as explicitly stated in the PC-port `NOTICE` | **Not cleared for a general public license** |
| N64 SDK and libultra-derived files | 79 SGI copyright-notice files, 77 of them flagged with restrictive wording; 53 of the SGI-notice files are listed as Visual Studio C/C++ compilation units | **High-priority rights review; do not assume attribution suffices** |
| ROM-derived/static graphics, model, font and tables | Some game graphics/font arrays are embedded in source, in addition to the user's separately generated `.dat` | **Review/remove/replace or obtain appropriate clearance** |
| ROM-only asset packer | Bundled converter reference combines MIT-covered PC tools with decomp-derived image/model references | **Tooling and bundled metadata need separate review** |
| `default.xbe` | Compiled game code, graphics/data and potentially SDK/runtime components | **Not cleared by a source license; separate binary gate** |
| `images/demo.gif`, `logo.png`, `Darkone83.png`, `titleimage.png` | Confirm ownership, embedded GoldenEye imagery, branding, and relevant trademark issues | **Approval/provenance pending** |

## Root MIT scope and exclusions

The standard MIT terms appear in the root `LICENSE`. They apply to independently created 007xb software and documentation for which Darkone83 owns or controls the copyright, and to any other original contributions whose rights holders expressly grant MIT permission. In a file combining original changes with inherited code, **only the original, licensable contributions are covered**; inherited material remains subject to its own terms and rights.

The root MIT grant **does not** cover or grant permission for:

- GoldenEye 007 original game code, decompilation-derived content, original graphics, audio, fonts, models, recorded gameplay, or ROM-derived data.
- N64 SDK/libultra, Silicon Graphics, Nintendo, MIPS, or other proprietary third-party files.
- Upstream components beyond what each upstream author actually licensed (including the PC port and Fast3D, whose own notices remain authoritative).
- Third-party trademarks, logos or media merely because they are included in the repository.
- Redistribution of `default.xbe`; a root software license cannot supply rights to unrelated compiled-in components.

Before merging third-party material or original contributions from other authors, record provenance and confirm the appropriate license or permission. See `THIRD_PARTY_NOTICES.md` and `docs/RIGHTS_CLEARANCE_TRACKER.md`.

## License locations to preserve (source tree moved to `src/`)

- `src/007xb/engine/LICENSE` — MIT, © James Dansereau, **scope limited by the adjacent `NOTICE`**.
- `src/007xb/engine/NOTICE` — defines the PC port's original-work scope and excludes inherited decompilation from its MIT grant.
- `src/007xb/engine/port/fast3d/LICENSE.txt` — MIT, © Emill and MaikelChan.
- `tools/007xb_converter_reference.zip` — contains copies of upstream `LICENSE` and `NOTICE`; those terms apply only to the covered original work, not all 522 enclosed entries.

**Do not replace these upstream license files with the root MIT license** or alter their scope. The root MIT license now provides a grant for eligible original 007xb contributions only. It does not settle the unresolved rights of inherited source or binaries.

## Release paths

| Intended release | Required clearance before posting |
| --- | --- |
| Documentation only | Original documentation authorship; permission/appropriate legal basis for any included images or footage |
| Asset Studio only | Audit converter scripts **and** bundled decomp-derived model/image metadata, branding, and included notices; no ROM or generated `.dat` |
| Engine source | Resolve or replace decomp-derived code, SDK/libultra content, embedded game data, and source contributions; counsel's rights analysis |
| Precompiled XBE | All source gates **plus** binary linkage, runtime/SDK, title graphics, and game-code distribution analysis |

See [the detailed review](docs/LEGAL_RELEASE_REVIEW.md), [rights-clearance tracker](docs/RIGHTS_CLEARANCE_TRACKER.md), [copyright header audit](docs/COPYRIGHT_HEADER_AUDIT.md), and [third-party notices](THIRD_PARTY_NOTICES.md). The public-release checklist is at [docs/PUBLIC_RELEASE_CHECKLIST.md](docs/PUBLIC_RELEASE_CHECKLIST.md).

## Statements not made by this project

Adopting the root MIT license does **not** mean the entire source tree is MIT-licensed, that ROM ownership resolves all extraction/distribution issues, that a non-commercial fan project is automatically exempt from copyright or trademark law, or that the upstream project's publication establishes independent permission for this port.

The appropriate legal basis for some reverse-engineered portions may depend on facts and jurisdiction; **no conclusion on fair use or interoperability exemptions is made here**. Obtain advice from a qualified IP attorney before releasing unresolved source or binaries.

## Primary references

1. PC port source and scoped `NOTICE`: https://github.com/jkdansereau/goldeneye-pc-port — `NOTICE`, `LICENSE`, and `README.md`.
2. Original decompilation: https://github.com/n64decomp/007 — published without a repository-wide license identified by this review.
3. GitHub's no-license guidance: https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/licensing-a-repository
4. United States Copyright Act, 17 U.S.C. §106: https://uscode.house.gov/us/usc/119-124/t17/s106/
5. 17 U.S.C. §1201(f) contains a conditional interoperability provision; it is **not** general authorization to redistribute all decompiled code: https://uscode.house.gov/us/usc/119-118/t17/s1201/

**Reference snapshot:** November 12, 2026. **Not legal advice.**
