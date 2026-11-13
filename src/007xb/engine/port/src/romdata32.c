/* Native Xbox endian boundary. These resources keep the N64's four-byte
 * pointer layout; only serialized scalar byte order changes. The PC64
 * sidecar converters must not participate in this path. */
#include <stddef.h>
#include <PR/libaudio.h>
#include "bondtypes.h"
#include "romdata.h"
#include "ge_asset32.h"
#include "system.h"

#if !defined(GE_XBOX_NATIVE32)
#error romdata32.c belongs only to the native 32-bit Xbox target
#endif
typedef char geAssertNativePointer[sizeof(void *) == 4 ? 1 : -1];
typedef char geAssertBankOffsets[offsetof(ALBank,instArray) == 12 ? 1 : -1];
typedef char geAssertInstrumentOffsets[offsetof(ALInstrument,soundArray) == 16 ? 1 : -1];
typedef char geAssertSoundLayout[sizeof(ALSound) == 16 ? 1 : -1];
typedef char geAssertWaveLayout[sizeof(ALWaveTable) == 20 ? 1 : -1];
typedef char geAssertFontCharLayout[sizeof(struct fontchar) == 24 ? 1 : -1];
typedef char geAssertFontOffsets[offsetof(struct font,chars) == 676 ? 1 : -1];

static void requireConversion(int ok,const char *name)
{
    if(!ok)sysFatalError("Xbox %s conversion: %s",name,GEAsset32_Error());
}
void romdataFixupAnimationData(u8 *blob,u32 size,const s32 *a,const s32 *b)
{
    requireConversion(GEAsset32_ConvertAnimations(blob,size,a,b),"animation");
}
void romdataFixupMusicSeqTable(u8 *blob,u32 size)
{
    requireConversion(GEAsset32_ConvertMusicTable(blob,size),"music table");
}
void romdataFixupCseq(u8 *blob)
{
    requireConversion(GEAsset32_ConvertCseq(blob,68),"compact sequence");
}
void romdataFixupLangBank(u8 *blob,u32 size)
{
    requireConversion(GEAsset32_ConvertLanguage(blob,size),"language bank");
}
void romdataFixupBriefing(u8 *blob)
{
    requireConversion(GEAsset32_ConvertBriefing(blob,48),"briefing");
}
u32 romdataFontPcSize(const u8 *source,u32 size)
{
    /* source is a streamed cartridge token, never a dereferenceable pointer. */
    (void)source;
    return (size+15u)&~15u;
}
void romdataFixupFont(u8 *blob,u32 size)
{
    requireConversion(GEAsset32_ConvertFont(blob,size),"font");
}
u32 romdataAudioBankPcSize(const u8 *source,u32 size)
{
    (void)source;
    return (size+15u)&~15u;
}
void romdataFixupAudioBank(u8 *blob,u32 size,u32 allocated)
{
    if(allocated<size)sysFatalError("Xbox audio bank allocation is too small");
    requireConversion(GEAsset32_ConvertAudioBank(blob,size),"audio bank");
}
