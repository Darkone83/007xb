#ifndef GE_NATIVE_GFX_H
#define GE_NATIVE_GFX_H
#include <stdint.h>
struct GfxWindowManagerAPI;
struct GfxRenderingAPI;
#ifdef __cplusplus
extern "C" {
#endif
int geNativeGfxInit(struct GfxWindowManagerAPI *,struct GfxRenderingAPI *,unsigned,unsigned);
int geNativeGfxFrame(void *,unsigned);
unsigned geNativeGfxMaxDLDepth(void);
void geNativeGfxResolution(int,int);
void geNativeGfxDisplayAspect(float);
void geNativeGfxDestroy(void);
void geNativeGfxTextureReset(void);
void geNativeDrawLogo(float radians,float scale,float alpha);
#ifdef __cplusplus
}
#endif
#endif
