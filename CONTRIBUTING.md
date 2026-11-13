# Contributing to 007xb

007xb is a work-in-progress original-Xbox compatibility project. Contributions should be focused, testable on real hardware where possible, and traceable to their actual authors and source licenses. Read [LICENSING.md](LICENSING.md) and the [rights tracker](docs/RIGHTS_CLEARANCE_TRACKER.md) first.

## Before submitting

- Do not upload a GoldenEye ROM, `007xb_assets.dat`, extracted game assets, proprietary SDK/XDK files, private logs or copyrighted graphics lacking permission.
- Only submit code, documentation or artwork for which you can establish suitable contribution rights. A copied file from a public repository is not automatically open-source.
- Keep the complete existing copyright/attribution headers. Do **not** modify or delete notices to disguise copied material.
- Record the **source URL, upstream commit, author, license, portions used, modifications, and intended redistribution** for each third-party addition.
- Original contributions should be offered under the root MIT license where the contributor owns or controls the necessary rights; by submitting a PR, clearly state whether you agree to license your original changes under MIT. If you cannot make that grant, disclose the limitation before submission. The root license never relicenses inherited code, tools, art, or data.

## Technical practices

- Keep `src/` for the Xbox engine, `tools/` for every tool/build command, `images/` for original visual material, and `docs/` for project notes.
- Preserve original console rendering semantics, working gunbarrel, models and audio, and maintain small reversible performance changes.
- Compare gameplay changes against the original decomp and PC port, while recognizing that the PC port is not always N64-identical.
- Don't alter RNG/ticks as part of unrelated renderer optimizations. Keep diagnostic logging bounded.
- For the asset packer, retain one user-supplied ROM → only `007xb_assets.dat` as output, with extraction caches private/local.

## Pull request description

Include: rationale; changed files; delete list; exact tested source revision; real-Xbox versus host test results; rights/provenance details of every new external dependency or file. The maintainer may defer or decline code with unresolved rights even if it compiles.

**Acceptance of a contribution does not authorize distribution of third-party game, SDK, or other materials the submitter does not own.**
