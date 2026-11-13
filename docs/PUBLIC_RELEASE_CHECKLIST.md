# Public release checklist

A practical checklist for the maintainer before publishing a public 007xb repository or release. **This does not itself approve distribution.**

## Repository contents

- [ ] Move engine and solution/project files to `src/` while retaining/fixing their relative paths.
- [ ] Keep **all** scripts, packer code and tool build commands inside `tools/`.
- [ ] Use `images/demo.gif`, `images/logo.png` and `images/Darkone83.png`; preview layout matches `README.md`.
- [ ] Place the intended distributable `default.xbe` (if approved) in `xbe/` and initialize an empty `xbe/data/`; add a `.gitkeep` if the empty folder must be tracked by Git.
- [ ] Remove/ignore local build output, ROMs, generated `.dat` files, temporary extraction/cache directories, `.iso`, private logs and save data.
- [ ] Exclude machine-specific `src/007xb/007xb.vcxproj.user` and inspect project settings for personal paths, credentials and machine-specific dependencies.
- [ ] From a clean clone, run the ROM-only packer and confirm **only** `data/007xb_assets.dat` is produced for runtime deployment.
- [ ] Test clean Xbox deployment: `default.xbe` and `data/007xb_assets.dat` under the **same title directory**.

## Rights and credit — must be resolved

- [ ] Select and explicitly authorize a license **for original 007xb-authored code and documents only**. Update `LICENSING.md` to reflect the choice and its exact scope; don't override upstream terms.
- [ ] Preserve upstream PC-port MIT text/NOTICE and Fast3D MIT text in relocated files.
- [ ] Review publication rights for the decompilation-derived source and N64 SDK/libultra headers containing Silicon Graphics, Nintendo, MIPS or AT&T notices.
- [ ] Review `tools/007xb_converter_reference.zip` content and each included source/metadata file's provenance.
- [ ] Confirm redistribution rights for `default.xbe` and any SDK/runtime binary code it incorporates. If not approved, **omit the XBE** and provide source/build instructions as appropriate.
- [ ] Confirm rights/permission for the logo files and gameplay GIF; ensure any required attributions are included.
- [ ] Final review of `THIRD_PARTY_NOTICES.md`, `docs/COPYRIGHT_HEADER_AUDIT.md`, copyright headers and any externally derived assets/code not yet listed.

## Technical release gate

- [ ] Build with the intended classic RXDK environment; ensure the relocated solution builds.
- [ ] Confirm startup, title/gunbarrel, models, weapon render, common alpha/fog paths, and gameplay on real Xbox hardware.
- [ ] Check Runway or similarly heavy stages, plus title and gameplay audio.
- [ ] Decide how to present the **known unresolved demo RNG synchronization** issue to testers.
- [ ] Confirm quiet logging stays bounded and errors remain diagnosable.
- [ ] Publish release notes with tested console model/RAM, known issues, and a version/commit identifier.

**Do not add the user's ROM or any generated ROM-derived data file to the repository or release.**
