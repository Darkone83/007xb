#ifndef GE_ASSET32_H
#define GE_ASSET32_H

/* Original GoldenEye ROM records are 32-bit big-endian. Xbox uses the
 * same pointer width, so convert scalars in place and retain every offset.
 * No PC64 model or audio-bank image may be passed to these functions.
 * Returns 1 on success, 0 on invalid input/allocation failure. A failed
 * operation leaves the entire input image untouched. */
#ifdef __cplusplus
extern "C" {
#endif
int GEAsset32_ConvertModel(void *image, unsigned bytes,
                           unsigned switches, unsigned textures);
int GEAsset32_ConvertAudioBank(void *image, unsigned bytes);
int GEAsset32_ConvertFont(void *image, unsigned bytes);
int GEAsset32_ConvertMusicTable(void *image, unsigned bytes);
int GEAsset32_ConvertCseq(void *image, unsigned bytes);
int GEAsset32_ConvertLanguage(void *image, unsigned bytes);
int GEAsset32_ConvertBriefing(void *image, unsigned bytes);
int GEAsset32_ConvertAnimations(void *image, unsigned bytes,
                                const int *table_a, const int *table_b);

/* BG offset-zero reads only: either the initial 64-byte header probe or
 * the complete metadata prefix through ALIGN16(room[1].point_offset).
 * file_bytes is the original whole BG file length. Original segment-0x0f
 * offsets and record sizes are retained; aligned compressed-tail bytes
 * are untouched. Room Gfx/Vtx streams are converted after decompression
 * by the existing bg.c loaders, not by this function. */
int GEAsset32_ConvertBg(void *image, unsigned bytes, unsigned file_bytes);
/* Decompressed final-format STAN, before stanDetermineEOF relocates the
 * four-byte region offsets. Tile offsets/links and byte fields retain
 * their original positions; beta/float STAN is a different format. */
int GEAsset32_ConvertStan(void *image, unsigned bytes);
/* Decompressed native32 mission/multiplayer setup before runtime pointer
 * relocation. Includes pads, paths, AI records and serialized prop data. */
int GEAsset32_ConvertSetup(void *image, unsigned bytes);

/* Callback receives offsets within the wavetable .tbl, not the .ctl.
 * Call this on pristine ROM bytes. Duplicate spans are harmless; the
 * consumer must merge them before byte-swapping a cached table. */
typedef int (*GEAsset32Raw16Range)(void *user, unsigned offset, unsigned bytes);
int GEAsset32_ScanRaw16(const void *bank, unsigned bank_bytes,
                        GEAsset32Raw16Range callback, void *user);
/* Streamed cartridges stay pristine. Register the RAW16 spans while the
 * control bank is still big-endian, then postprocess every successful PI
 * read. Compressed ADPCM and bytes outside these spans are unchanged.
 * Registrations are merged; a failed registration changes no prior span. */
void GEAsset32_ResetAudioTables(void);
int GEAsset32_RegisterAudioTable(const void *bank, unsigned bank_bytes,
                                unsigned table_cart_address,
                                unsigned table_bytes);
int GEAsset32_SwapRaw16Dma(unsigned cart_address, void *destination,
                          unsigned bytes);
const char *GEAsset32_Error(void);
#ifdef __cplusplus
}
#endif
#endif
