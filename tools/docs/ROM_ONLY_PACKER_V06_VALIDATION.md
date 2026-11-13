# 007xb Asset Studio v0.6 — verification

Validated from the user's existing archived v0.5 source and project files.

## Target behavior

- Source/launch/build/branding/test utilities all live under `tools/`.
- Only runtime file generated in `data/`: `007xb_assets.dat`.
- No `data/extracted/` or sidecar directories are produced by the new build.
- Intermediates + logs/manifests stored inside `tools/.cache/`.
- User-supplied ROM is required, validated and **never included in release ZIPs**.
- Internal archive remains 007XBDAT v1 with six entries, 4 KiB payload alignment,
  80-byte directory records, directory CRC and per-entry CRC.

## Results

- Python integration tests: 7 passed (mock ROM/emitters, cache invalidation,
  folder safety, input discovery, bundled reference, frozen tools/dist placement).
- Full original ROM conversion: **passed** with 512 model, 86 stage resources,
  34/34 setup files; only one deployable `.dat` produced.
- Full archive size: **17,649,664 bytes**.
- SHA-256: `4cdfcdb5b4807977397a56368dd6023b23ac0cecba858ac1c4f028f207c5cfac`.
- **Byte-for-byte identical to the user's archived working
  `007xb/Media/data/007xb_assets.dat`**.
- Valid entries: `rom/ge007.ntsc-final.z64`,
  `pcmodels/pcmodels.bin`, `pcmodels/manifest.csv`,
  `pccg/pccg.bin`, `pccg/manifest.csv`, `branding/darkone83.lines`.
- Windows PyInstaller EXE and real-hardware install: **not yet tested** for v0.6.

Archive contents derived from the user's own ROM are intentionally **excluded**
from the source/update packages.
