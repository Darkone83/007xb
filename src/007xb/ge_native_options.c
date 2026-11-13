/* Original N64 defaults for optional desktop-port settings.
 * These are configuration leaves, not replacement game implementations.
 * Xbox controller input is supplied through the real osCont* boundary. */
#include <ultra64.h>
#include "ge_native_video.h"
/* Native Xbox configuration does not expose desktop process overrides.
 * NULL selects each caller's existing default; an empty string would enable
 * the many diagnostic switches that test only whether a variable exists. */
char *geNativeGetenv(const char *name)
{
    (void)name;
    return NULL;
}

float portScreenShakeScale = 1.0f;
int portSkipIntro = 0;
int portNoHitFlash = 0;
int portCrosshairHide = 0;
int portAllUnlocked = 0;
int portCrosshairPersistent(void) { return 0; }
int portCrosshairStyle(void) { return 0; }
float portCrosshairScale(void) { return 1.0f; }
void portCrosshairTint(s32 *r,s32 *g,s32 *b) { if(r)*r=255;if(g)*g=255;if(b)*b=255; }
void portCrosshairApplyTintCombine(Gfx *command) { (void)command; }
s32 portHudScalePercent(void) { return 100; }
f32 portNativeAspect(void)
{
    /* Reuse the PC port's Hor+ projection and HUD/reticle corrections.
     * The video mode supplies display aspect, including anamorphic 480
     * widescreen. Pixel width/height alone is not the display aspect there.
     * As with PC native widescreen, this automatic mode takes precedence
     * over the original game's Ratio option; Screen size still applies. */
    f32 aspect = videoGetDisplayAspectRatio();
    if (!(aspect > 0.01f)) return 0.0f;
    if (aspect < 0.5f) aspect = 0.5f;
    if (aspect > 4.0f) aspect = 4.0f;
    return aspect;
}
f32 portScaleFovY(f32 fovy,s32 title) { (void)title;return fovy>160.0f?160.0f:fovy; }
f32 portFovYScaleFactor(void) { return 1.0f; }
f32 portDrawDistanceMultiplier(void) { return 1.0f; }
f32 portFogDistanceMultiplier(void) { return 1.0f; }
f32 portLodDistanceMultiplier(void) { return 1.0f; }
f32 portRoomPoolScale(void)
{
    /* The PC port budgets sqrt(display aspect / 4:3) for the extra visible
     * side rooms. boss.c allocates in quarter-scale units, rounded down;
     * returning 1.1547 at 16:9 would therefore add no room memory. Round
     * up to those units here (1.25 for 16:9), with the same 2x upper bound.
     * No square-root or wide-integer helper is needed for four steps. The
     * existing stage-bank headroom cap still governs the allocation. */
    f32 required = portNativeAspect() * 0.75f;
    f32 scale = 1.0f;
    while (scale < 2.0f && scale * scale < required) scale += 0.25f;
    return scale;
}
int portAIWideView(void) { return 0; }
float portGunSwayTheta(float delta) { (void)delta;return 0.0f; }
int portMouseAimPdGetTurn(f32 *x,f32 *y) { if(x)*x=0.0f;if(y)*y=0.0f;return 0; }
Gfx *optionsFileSelectLabel(Gfx *dl) { return dl; }
