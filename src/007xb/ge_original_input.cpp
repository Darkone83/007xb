#include "ge_original_input.h"
#include "xb_ge_libultra.h"
#include <string.h>
#if defined(XBOX) || defined(_XBOX)
#include <xtl.h>
static CRITICAL_SECTION s_inputLock;
static int s_inputLockReady=0;
static void LockInput(){if(s_inputLockReady)EnterCriticalSection(&s_inputLock);}
static void UnlockInput(){if(s_inputLockReady)LeaveCriticalSection(&s_inputLock);}
#else
#include <pthread.h>
static pthread_mutex_t s_inputLock=PTHREAD_MUTEX_INITIALIZER;
static void LockInput(){pthread_mutex_lock(&s_inputLock);}
static void UnlockInput(){pthread_mutex_unlock(&s_inputLock);}
#endif
/* The N64 SI driver operates on four controller channels. Keep the real
   game-visible pad and status arrays; the Xbox OS takes care of XInput. */
static GEOSContStatus s_status[4];
static GEOSContPad s_pads[4];
static GEGameXboxState s_xboxPads[4];
static unsigned char s_connected=0;
extern "C" void GEGameInput_Reset(void){
#if defined(XBOX) || defined(_XBOX)
    /* Called once before the game scheduler and input threads start. */
    if(!s_inputLockReady){InitializeCriticalSection(&s_inputLock);s_inputLockReady=1;}
#endif
    LockInput();
    s_connected=0;
    memset(s_status,0,sizeof(s_status));
    memset(s_pads,0,sizeof(s_pads));
    memset(s_xboxPads,0,sizeof(s_xboxPads));
    for(unsigned i=0;i<4u;++i){
        s_status[i].error_code=8; /* CONT_NO_RESPONSE_ERROR */
        s_pads[i].error_code=8;
    }
    UnlockInput();
}
static signed char ClampStick(int v){
    if(v>80)v=80;
    if(v< -80)v=-80;
    return (signed char)v;
}
static void UpdatePadUnlocked(unsigned port,int connected,
                               unsigned short buttons,int sx,int sy){
    if(connected){
        s_connected=(unsigned char)(s_connected|(1u<<port));
        s_status[port].type=5u; /* CONT_TYPE_NORMAL */
        s_status[port].status=0u;
        s_status[port].error_code=0u;
        s_pads[port].button=buttons;
        s_pads[port].stick_x=ClampStick(sx);
        s_pads[port].stick_y=ClampStick(sy);
        s_pads[port].error_code=0;
    }else{
        s_connected=(unsigned char)(s_connected&~(1u<<port));
        s_status[port].type=0;
        s_status[port].status=0;
        s_status[port].error_code=8; /* CONT_NO_RESPONSE_ERROR */
        s_pads[port].button=0;
        s_pads[port].stick_x=0;
        s_pads[port].stick_y=0;
        s_pads[port].error_code=8;
        memset(&s_xboxPads[port],0,sizeof(s_xboxPads[port]));
    }
}
extern "C" void GEGameInput_Update(unsigned port,int connected,
                                    unsigned short buttons,int sx,int sy){
    if(port>=4)return;
    LockInput();
    UpdatePadUnlocked(port,connected,buttons,sx,sy);
    /* A legacy-only provider has no live native sideband. */
    memset(&s_xboxPads[port],0,sizeof(s_xboxPads[port]));
    UnlockInput();
}
extern "C" void GEGameInput_UpdateXbox(unsigned port,int connected,
        unsigned short buttons,int sx,int sy,const GEGameXboxState* xbox){
    if(port>=4)return;
    LockInput();
    UpdatePadUnlocked(port,connected,buttons,sx,sy);
    GEGameXboxState& out=s_xboxPads[port];
    if(connected && xbox) {
        const unsigned held=xbox->buttons&0xFFFFu;
        out.pressed|=held&~out.buttons;
        out.buttons=held;
        out.move_x=ClampStick(xbox->move_x);out.move_y=ClampStick(xbox->move_y);
        out.look_x=ClampStick(xbox->look_x);out.look_y=ClampStick(xbox->look_y);
        out.connected=1;
    } else {
        memset(&out,0,sizeof(out));
    }
    UnlockInput();
}
extern "C" int GEGameInput_ReadXbox(unsigned port,GEGameXboxState* out){
    if(!out)return 0;
    if(port>=4){memset(out,0,sizeof(*out));return 0;}
    LockInput();
    *out=s_xboxPads[port];
    s_xboxPads[port].pressed=0;
    const int connected=out->connected;
    UnlockInput();
    return connected;
}
extern "C" void GEGameInput_ClearXboxEdges(unsigned port){
    if(port>=4)return;
    LockInput();
    s_xboxPads[port].pressed=0;
    UnlockInput();
}
extern "C" int osContInit(void*,unsigned char* mask,GEOSContStatus* status){
    LockInput();
    if(mask)*mask=s_connected;
    if(status)memcpy(status,s_status,sizeof(s_status));
    UnlockInput();
    return 0;
}
extern "C" int osContReset(void*,GEOSContStatus* status){
    LockInput();
    if(status)memcpy(status,s_status,sizeof(s_status));
    UnlockInput();
    return 0;
}
extern "C" int osContStartReadData(void* q){
    /* Original N64 SI completion is delivered before joy.c reads pads. */
    return q?osSendMesg(q,0,0):-1;
}
extern "C" int osContStartQuery(void* q){
    return q?osSendMesg(q,0,0):-1;
}
extern "C" void osContGetReadData(GEOSContPad* pads){
    LockInput();
    if(pads)memcpy(pads,s_pads,sizeof(s_pads));
    UnlockInput();
}
extern "C" void osContGetQuery(GEOSContStatus* status){
    LockInput();
    if(status)memcpy(status,s_status,sizeof(s_status));
    UnlockInput();
}
