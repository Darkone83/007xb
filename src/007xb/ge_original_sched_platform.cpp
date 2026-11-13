#include "ge_original_sched_platform.h"
#include <string.h>
/* This unit supplies platform LEAF declarations absent until the complete
 * GoldenEye game sources are linked.  Original scheduler code in
 * ge_original_sched.cpp is compiled intact apart from RXDK 32-bit ABI fixes.
 * The optional hooks deliberately do not fabricate game audio or graphics.
 */

extern "C" {
u32 osTvType = 1u; /* OS_TV_NTSC, United States ROM */
OSViMode* viMode = 0;
u32 g_viOriginalHstart=0, g_viOriginalVstart0=0, g_viOriginalVstart1=0;
static unsigned short s_cfb0=0, s_cfb1=0;
unsigned short* cfb_16[2]={&s_cfb0,&s_cfb1};
unsigned char sp_shed[0x2000];
static OSThread s_schedThread;
}
static GEOriginalSchedulerHooks s_geHooks;
static OSViMode* s_activeViMode=0;
static volatile unsigned s_mask=0x003fff01u; /* OS_IM_ALL */
static int s_maskReady=0;
static int s_clientLockReady=0;
#if defined(XBOX) || defined(_XBOX)
static CRITICAL_SECTION s_maskCS;
static CRITICAL_SECTION s_clientCS;
#else
static pthread_mutex_t s_maskCS;
static pthread_mutex_t s_clientCS;
#endif
static void MaskInit() {
    if(s_maskReady)return;
#if defined(XBOX) || defined(_XBOX)
    InitializeCriticalSection(&s_maskCS);
    InitializeCriticalSection(&s_clientCS);
#else
    pthread_mutex_init(&s_maskCS,0);
    pthread_mutex_init(&s_clientCS,0);
#endif
    s_maskReady=1;
    s_clientLockReady=1;
}
extern "C" OSIntMask osSetIntMask(OSIntMask newmask) {
    /* Interrupt priorities are MIPS-specific.  Preserve the current mask
     * value atomically; original scheduler client mutations use the separate
     * cross-thread lock below, also taken on retrace traversal. */
    MaskInit();
#if defined(XBOX) || defined(_XBOX)
    EnterCriticalSection(&s_maskCS);
#else
    pthread_mutex_lock(&s_maskCS);
#endif
    const OSIntMask before=s_mask;
    s_mask=newmask;
#if defined(XBOX) || defined(_XBOX)
    LeaveCriticalSection(&s_maskCS);
#else
    pthread_mutex_unlock(&s_maskCS);
#endif
    return before;
}
extern "C" void GEOriginalScheduler_ClientLock(void) {
    if(!s_clientLockReady)MaskInit();
#if defined(XBOX) || defined(_XBOX)
    EnterCriticalSection(&s_clientCS);
#else
    pthread_mutex_lock(&s_clientCS);
#endif
}
extern "C" void GEOriginalScheduler_ClientUnlock(void) {
#if defined(XBOX) || defined(_XBOX)
    LeaveCriticalSection(&s_clientCS);
#else
    pthread_mutex_unlock(&s_clientCS);
#endif
}
extern "C" void* setSPToEnd(void* stack,unsigned int bytes) {
    return stack && bytes>=8u ? ((unsigned char*)stack+bytes-8u) : 0;
}
extern "C" void osCreateViManager(int) {
    /* The 60 Hz VI timer is managed by GERuntime::OS().StartRetrace(). */
}
extern "C" void osViSetMode(OSViMode* mode) {
    s_activeViMode=mode;
    viMode=mode;
}
extern "C" void osDpSetStatus(u32) { /* software fast3d owns DP state */ }
extern "C" void osWritebackDCacheAll(void) { /* coherent Xbox x86 */ }
extern "C" void GEOriginalScheduler_SetHooks(const GEOriginalSchedulerHooks* hooks) {
    if(hooks)s_geHooks=*hooks;
    else memset(&s_geHooks,0,sizeof(s_geHooks));
}
extern "C" void speedgraphMarkerUpdate(void) {
    if(s_geHooks.speedgraphUpdate)s_geHooks.speedgraphUpdate();
}
extern "C" void speedgraphMarkerCommit(void) {}
extern "C" void speedgraphMarkerHandler(u32 event) {
    if(s_geHooks.speedgraphMarker)s_geHooks.speedgraphMarker(event);
}
extern "C" void joyPoll(void) {
    if(s_geHooks.joyPoll)s_geHooks.joyPoll();
}
extern "C" void joyRumblePakStop(void) { /* no upstream N64 rumble yet */ }
extern "C" void musicFadeTick(void) {
    if(s_geHooks.musicFadeTick)s_geHooks.musicFadeTick();
}
extern "C" void viVsyncRelated(void) {
    if(s_geHooks.vsyncUpdate)s_geHooks.vsyncUpdate();
}
extern "C" void crashRenderFrame(void*) { /* original crash UI not linked */ }

extern "C" void GEOriginalScheduler_PlatformInit(void) {
    MaskInit();
    /* All 42 real VI mode records are compiled from original vitbl.c;
     * do not replace them with made-up width/hstart constants. */
    s_activeViMode=0;
    viMode=0;
    s_mask=0x003fff01u;
}
extern "C" int GEOriginalScheduler_StartOnXbox(void) {
    GEOriginalScheduler_PlatformInit();
    /* Real original game scheduler from src/sched.c; no synthetic menus. */
    osCreateScheduler(&os_scheduler,&s_schedThread,OS_VI_NTSC_LAN1,1u);
    return os_scheduler.interruptQ.msgCount==8 &&
           os_scheduler.cmdQ.msgCount==8 &&
           s_schedThread.header.state==2;
}
