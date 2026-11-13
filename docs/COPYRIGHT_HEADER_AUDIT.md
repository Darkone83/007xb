# Copyright header audit (source snapshot)

**Automated inventory of explicit attribution notices in the user-supplied `007xb(1).zip` source snapshot, November 2026.** This is **not a legal clearance** or a full license audit. Paths below assume the uploaded `007xb/` source tree is moved to `src/007xb/`. Some files have multiple notices and appear in more than one group. Files without explicit notices may still contain third-party or copyrighted code; do not infer that they are free to redistribute.

| Explicit notice family | Files identified |
| --- | ---: |
| Silicon Graphics | 79 |
| MIPS Computer Systems | 2 |
| AT&T | 4 |
| UNIX System Laboratories | 1 |
| Nintendo | 1 |
| SpectNet / Istvan Novak | 1 |

This inventory is intended to prevent copyright/header notices from being lost during repository cleanup. **Do not remove or replace these notices.** In particular, some N64-era SDK headers contain restrictive language. Their redistribution status must be reviewed before making the repository public; attribution alone does not grant permission.

## Silicon Graphics (79 files)

- `src/007xb/engine/include/`: `assert.h`, `bstring.h`, `limits.h`, `math.h`, `sgidefs.h`, `stddef.h`, `stdlib.h`, `svr4_math.h`
- `src/007xb/engine/include/PR/`: `R4300.h`, `abi.h`, `gbi.h`, `gu.h`, `libaudio.h`, `mbi.h`, `os.h`, `os_internal.h`, `ram_ORIGINAL_rom.h`, `rcp.h`, `region.h`, `rm_ORIGINAL_on.h`, `ultraerror.h`, `ultratypes.h`
- `src/007xb/engine/src/libultra/audio/`: `auxbus.c`, `bnkf.c`, `copy.c`, `cseq.c`, `cspgetstate.c`, `csplayer.c`, `cspplay.c`, `cspsetseq.c`, `cspsetvol.c`, `cspstop.c`, `event.c`, `filter.c`, `heapalloc.c`, `heapinit.c`, `initfx.h`, `load.c`, `mainbus.c`, `resample.c`, `save.c`, `seq.c`, `seqplayer.c`, `seqpsetbank.c`, `sl.c`, `synaddplayer.c`, `synallocfx.c`, `synallocvoice.c`, `syndelete.c`, `synfreevoice.c`, `synsetfxmix.c`, `synsetpan.c`, `synsetpitch.c`, `synsetpriority.c`, `synsetvol.c`, `synstartvoice.c`, `synstartvoiceparam.c`, `synstopvoice.c`, `synthInternals.h`, `synthesizer.c`
- `src/007xb/engine/src/libultra/gu/`: `align.c`, `cosf.c`, `coss.c`, `guint.h`, `lookat.c`, `lookatref.c`, `mtxutil.c`, `normalize.c`, `ortho.c`, `perspective.c`, `rotate.c`, `scale.c`, `sinf.c`, `sins.c`, `translate.c`
- `src/007xb/engine/src/libultra/io/`: `viint.h`
- `src/007xb/engine/src/libultrare/audio/`: `drvrNew.c`, `env.c`, `reverb.c`

## Nintendo (1 files)

- `src/007xb/engine/include/PR/gs2dex.h`

## MIPS Computer Systems (2 files)

- `src/007xb/engine/include/math.h`
- `src/007xb/engine/include/svr4_math.h`

## AT&T (4 files)

- `src/007xb/engine/include/assert.h`
- `src/007xb/engine/include/limits.h`
- `src/007xb/engine/include/stddef.h`
- `src/007xb/engine/include/stdlib.h`

## UNIX System Laboratories (1 files)

- `src/007xb/engine/include/stddef.h`

## SpectNet / Istvan Novak (1 files)

- `src/007xb/engine/src/game/spectrum_hw.h`

## Other upstream notices to retain

- `src/007xb/engine/LICENSE` and `NOTICE`: James Dansereau / GoldenEye PC port, limited-scope MIT work, inherited decomp rights exceptions.
- `src/007xb/engine/port/fast3d/LICENSE.txt`: © 2020 Emill, MaikelChan, MIT.
- `tools/007xb_converter_reference.zip`: contains upstream `LICENSE` and `NOTICE` plus source/reference metadata; independently review all bundled material.
- Original N64 GoldenEye code and reconstructed assets/metadata from [n64decomp/007](https://github.com/n64decomp/007) may not contain explicit local copyright lines. Follow the decompilation repository's contributor attributions and rights review.

This inventory is a **snapshot of observed notices only**. Expand it when components are added, code is rebased, or a component's provenance is confirmed. See [Third-party notices](../THIRD_PARTY_NOTICES.md) and [Licensing](../LICENSING.md).
