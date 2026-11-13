// 007xb Phase 2: Xbox host for the original GoldenEye engine.
#include <xtl.h>
#include <string.h>
#include "xb_archive.h"
#include "xb_logo.h"
#include "xb_video.h"
#include "xb_render.h"
#include "xb_ge_runtime.h"
#include "xb_ge_libultra.h"
#include "xb_ge_audio.h"
#include "ge_original_input.h"
#include "ge_native_platform.h"
#include "ge_native_video.h"
#include "ge_native_error.h"
/* The host pumps controller USB + DirectSound on the main thread. At 1 ms
 * it made ~1000 XInput/DirectSoundDoWork/GetCurrentPosition/Lock calls per
 * second, starving a single-core 733 MHz Xbox between original game ticks.
 * DirectSound's PCM cushion is ~46 ms, and submissions also service its
 * cursor: a 4 ms pump remains comfortably within that budget. */
#ifndef GE_XBOX_SERVICE_POLL_MS
#define GE_XBOX_SERVICE_POLL_MS 4u
#endif

extern "C" void mainproc(void *arg);
extern "C" int romdataInit(void);
static XBArchive s_archive;
static XBLogo s_logo;
static XBVideo s_video;
static XBRenderer s_draw;
static GERuntime s_runtime;
static HANDLE s_pads[4]={0,0,0,0};
static DWORD s_lastBack=0;
static int s_showDiagnostics=0;

static void StartInput(void) {
    GEGameInput_Reset();
    XDEVICE_PREALLOC_TYPE prealloc;
    ZeroMemory(&prealloc,sizeof(prealloc));
    prealloc.DeviceType=XDEVICE_TYPE_GAMEPAD;prealloc.dwPreallocCount=4;
    XInitDevices(1,&prealloc);
}
static void PollInput(void) {
    DWORD inserted=0,removed=0;
    XGetDeviceChanges(XDEVICE_TYPE_GAMEPAD,&inserted,&removed);
    const DWORD connected=XGetDevices(XDEVICE_TYPE_GAMEPAD);
    for(unsigned i=0;i<4;++i) {
        if((removed&(1u<<i)) && s_pads[i]) { XInputClose(s_pads[i]);s_pads[i]=0; }
        if(!s_pads[i] && (connected&(1u<<i)))
            s_pads[i]=XInputOpen(XDEVICE_TYPE_GAMEPAD,i,XDEVICE_NO_SLOT,0);
        XINPUT_STATE state;ZeroMemory(&state,sizeof(state));
        if(!s_pads[i] || XInputGetState(s_pads[i],&state)!=0) {
            GEGameInput_UpdateXbox(i,0,0,0,0,0);if(i==0)s_lastBack=0;continue;
        }
        const XINPUT_GAMEPAD &p=state.Gamepad;
        unsigned short buttons=0;
        if(p.bAnalogButtons[XINPUT_GAMEPAD_A]>80)buttons|=GE_CONT_A;
        if(p.bAnalogButtons[XINPUT_GAMEPAD_B]>80)buttons|=GE_CONT_B;
        if(p.bAnalogButtons[XINPUT_GAMEPAD_X]>80)buttons|=GE_CONT_L;
        if(p.wButtons&XINPUT_GAMEPAD_START)buttons|=GE_CONT_START;
        if(p.wButtons&XINPUT_GAMEPAD_DPAD_UP)buttons|=GE_CONT_UP;
        if(p.wButtons&XINPUT_GAMEPAD_DPAD_DOWN)buttons|=GE_CONT_DOWN;
        if(p.wButtons&XINPUT_GAMEPAD_DPAD_LEFT)buttons|=GE_CONT_LEFT;
        if(p.wButtons&XINPUT_GAMEPAD_DPAD_RIGHT)buttons|=GE_CONT_RIGHT;
        if(p.bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER]>80)buttons|=GE_CONT_Z;
        if(p.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER]>80)buttons|=GE_CONT_R;
        if(p.sThumbRY>16000)buttons|=GE_CONT_CUP;
        if(p.sThumbRY< -16000)buttons|=GE_CONT_CDOWN;
        if(p.sThumbRX< -16000)buttons|=GE_CONT_CLEFT;
        if(p.sThumbRX>16000)buttons|=GE_CONT_CRIGHT;
        GEGameXboxState xbox;ZeroMemory(&xbox,sizeof(xbox));
        if(p.bAnalogButtons[XINPUT_GAMEPAD_A]>80)xbox.buttons|=GE_XBOX_A;
        if(p.bAnalogButtons[XINPUT_GAMEPAD_B]>80)xbox.buttons|=GE_XBOX_B;
        if(p.bAnalogButtons[XINPUT_GAMEPAD_X]>80)xbox.buttons|=GE_XBOX_X;
        if(p.bAnalogButtons[XINPUT_GAMEPAD_Y]>80)xbox.buttons|=GE_XBOX_Y;
        if(p.bAnalogButtons[XINPUT_GAMEPAD_BLACK]>80)xbox.buttons|=GE_XBOX_BLACK;
        if(p.bAnalogButtons[XINPUT_GAMEPAD_WHITE]>80)xbox.buttons|=GE_XBOX_WHITE;
        if(p.bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER]>80)xbox.buttons|=GE_XBOX_LT;
        if(p.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER]>80)xbox.buttons|=GE_XBOX_RT;
        if(p.wButtons&XINPUT_GAMEPAD_START)xbox.buttons|=GE_XBOX_START;
        if(p.wButtons&XINPUT_GAMEPAD_BACK)xbox.buttons|=GE_XBOX_BACK;
        if(p.wButtons&XINPUT_GAMEPAD_LEFT_THUMB)xbox.buttons|=GE_XBOX_LTHUMB;
        if(p.wButtons&XINPUT_GAMEPAD_RIGHT_THUMB)xbox.buttons|=GE_XBOX_RTHUMB;
        if(p.wButtons&XINPUT_GAMEPAD_DPAD_UP)xbox.buttons|=GE_XBOX_UP;
        if(p.wButtons&XINPUT_GAMEPAD_DPAD_DOWN)xbox.buttons|=GE_XBOX_DOWN;
        if(p.wButtons&XINPUT_GAMEPAD_DPAD_LEFT)xbox.buttons|=GE_XBOX_LEFT;
        if(p.wButtons&XINPUT_GAMEPAD_DPAD_RIGHT)xbox.buttons|=GE_XBOX_RIGHT;
        xbox.move_x=(int)p.sThumbLX/410;xbox.move_y=(int)p.sThumbLY/410;
        xbox.look_x=(int)p.sThumbRX/410;xbox.look_y=(int)p.sThumbRY/410;
        xbox.connected=1;
        GEGameInput_UpdateXbox(i,1,buttons,xbox.move_x,xbox.move_y,&xbox);
        if(i==0) {
            const DWORD back=p.wButtons&XINPUT_GAMEPAD_BACK;
            if(back && !s_lastBack) {
                s_showDiagnostics=!s_showDiagnostics;
                GENativeVideo_SetDiagnostics(s_showDiagnostics);
            }
            s_lastBack=back;
        }
    }
}
static bool RunGraphics(void *task,void *) { return geNativeTaskGraphics(task)!=0; }
static bool RunAudio(void *task,void *) { return geNativeTaskAudio(task)!=0; }
static bool RunDP(void *commands,GEOSU64 bytes,void *) {
    if(bytes>0xFFFFFFFFull)return false;
    return geNativeRunGraphics(geNativeAddressToPointer((uintptr_t)commands),(unsigned)bytes)!=0;
}
static void ShowError(const char *message) {
    s_video.Begin();
    s_draw.DrawText(s_video.Device(),24,38,2.4f,"007XB - NATIVE ENGINE STOPPED",0xFFFFB065u);
    geNativeDrawErrorLines(&s_draw,s_video.Device(),message,92.0f);
    s_draw.DrawText(s_video.Device(),24,350,1.5f,
        "CAPTURE THIS ERROR AND THE XDK DEBUG OUTPUT",0xFF9FADC5u);
    s_video.End();
}
static const char *StartEngine(void) {
    if(!s_video.HasDepth())return "NATIVE ENGINE REQUIRES A D16 OR D24S8 DEPTH BUFFER";
    if(!s_archive.Open("D:\\data\\007xb_assets.dat"))return s_archive.Error();
    if(!(s_logo.LoadLoose("D:\\data\\darkone83.lines") ||
         s_logo.LoadArchive(s_archive,"branding/darkone83.lines")))return s_logo.Error();
    if(!s_runtime.Init(s_archive,"D:\\data\\007xb_assets.dat"))return s_runtime.Error();
    if(!GENativePlatform_Bind(&s_runtime,"D:\\data"))return GENativePlatform_Error();
    if(romdataInit()!=0)return GENativePlatform_Error();
    if(!GEAudio_Init())return "DIRECTSOUND INITIALIZATION FAILED";
    if(!GENativeVideo_Bind(&s_video,&s_draw,&s_logo))return "CANNOT BIND NATIVE VIDEO";
    GENativePlatform_SetFatalHook(GENativeVideo_OnFatal);
    if(!GENativeVideo_Start())
        return GENativePlatform_Error();
    GEOSInstallRSPRunner(RunGraphics,0);
    GENativePlatform_Trace("SCHED GFX_WORKER async=%d active=%d host_service_poll_ms=%u (VI=60Hz, 1 GFX task)\n",
        (int)GEOS_ASYNC_GFX,s_runtime.OS().GfxWorkerActive()?1:0,
        (unsigned)GE_XBOX_SERVICE_POLL_MS);
    GEOSInstallAudioRunner(RunAudio,0);
    GEOSInstallDPRunner(RunDP,0);
    PollInput();
    /* Original init.c creates the scheduler, game and audio threads and
       initializes the original memory pools. There is exactly one entry. */
    GENativeVideo_BootStep("STARTING ORIGINAL ENGINE");
    if(GENativePlatform_HasFatalError() || GENativeVideo_Failed())
        return GENativePlatform_Error();
    if(!s_runtime.RunOriginalMain(mainproc))return s_runtime.Error();
    return 0;
}
void __cdecl main(void) {
    if(!s_video.Init())for(;;)Sleep(1000);
    if(!s_draw.Init())for(;;)Sleep(1000);
    StartInput();
    const char *error=StartEngine();
    /* Startup can return an error without entering sysFatalError. Record it
     * once when storage is armed; the display loop must never repeat it. */
    if(error && !GENativePlatform_HasFatalError() && GENativePlatform_TraceReady())
        GENativePlatform_TraceIssue("STARTUP FAILED %s\n",error);
    for(;;) {
        PollInput();
        GEAudio_Tick();
        if(!error && (GENativePlatform_HasFatalError() || GENativeVideo_Failed()))
            error=GENativePlatform_Error();
        if(error && !GENativeVideo_Rendering())ShowError(error);
        /* Rendering runs only from original scheduler tasks. */
        Sleep(error?20:GE_XBOX_SERVICE_POLL_MS);
    }
}
