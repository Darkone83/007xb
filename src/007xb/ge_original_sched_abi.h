#ifndef XB_GE_ORIGINAL_SCHED_ABI_H
#define XB_GE_ORIGINAL_SCHED_ABI_H
/* Runtime declarations matching the NTSC GoldenEye decomp's 32-bit
 * game-visible scheduler structures.  See original src/sched.h, PR/os.h.
 * Compiled original scheduler source is in src/ge_original_sched.cpp.
 */
#include "xb_ge_scheduler.h"
#include <assert.h>
#include "ge_original_vi_regs.h"
#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed char s8;
typedef short s16;
typedef int s32;
typedef float f32;
typedef unsigned long long u64;
typedef u32 OSIntMask;
typedef void* OSMesg;
typedef XBGEAbiQueue OSMesgQueue;
typedef GEOriginalScMsg OSScMsg;
typedef GEOriginalScTask OSScTask;
typedef GEOriginalScClient OSScClient;
typedef GEOriginalScheduler OSSched;
typedef GEOriginalOSTask OSTask;
typedef struct { XBGEAbiThreadHeader header; unsigned char context[0x200]; } OSThread;
struct OSViCommonRegs {
    u32 ctrl,width,burst,vSync,hSync,leap,hStart,xScale,vCurrent;
};
struct OSViFieldRegs {u32 origin,yScale,vStart,vBurst,vIntr;};
struct OSViMode {u8 type; OSViCommonRegs comRegs; OSViFieldRegs fldRegs[2];};
#define OS_EVENT_SP 4
#define OS_EVENT_DP 9
#define OS_EVENT_PRENMI 14
#define OS_IM_NONE 0x00000001u
#define OS_IM_VI 0x00080401u
#define OS_MESG_BLOCK 1
#define OS_MESG_NOBLOCK 0
#define OS_TV_MPAL 2
#define TV_TYPE_MPAL 2
#define OS_PRIORITY_VIMGR 254
#define TRUE 1
#define FALSE 0
#define NUM_VIDEO_FRAME_BUFFERS 2
#define SCHED_THREAD_ID 2
#define SCHED_THREAD_PRIORITY 30
#define M_GFXTASK 1
#define M_AUDTASK 2
#define OS_SC_MAX_MESGS 8
#define OS_SC_RETRACE_MSG 1
#define OS_SC_DONE_MSG 2
#define OS_SC_RDP_DONE_MSG 3
#define OS_SC_RSP_MSG 4
#define OS_SC_PRE_NMI_MSG 5
#define OS_SC_NEEDS_RDP 0x0001
#define OS_SC_NEEDS_RSP 0x0002
#define OS_SC_DRAM_DLIST 0x0004
#define OS_SC_PARALLEL_TASK 0x0010
#define OS_SC_LAST_TASK 0x0020
#define OS_SC_SWAPBUFFER 0x0040
#define OS_SC_RCP_MASK 0x0003
#define OS_SC_TYPE_MASK 0x0007

#define OS_CLOCK_RATE 62500000ULL

extern "C" {
extern u32 osTvType;
extern OSViMode osViModeTable[42];
extern OSViMode* viMode;
extern u32 g_viOriginalHstart,g_viOriginalVstart0,g_viOriginalVstart1;
extern unsigned short* cfb_16[2];
extern unsigned char sp_shed[0x2000];
void* setSPToEnd(void* stack,unsigned int size);
void GEOriginalScheduler_ClientLock(void);
void GEOriginalScheduler_ClientUnlock(void);
void osCreateViManager(int priority);
void osViSetMode(OSViMode* mode);
void osViSetXScale(float value);
void osViSetYScale(float value);
void osViRepeatLine(unsigned char line);
void osViBlack(unsigned char value);
OSIntMask osSetIntMask(OSIntMask mask);
void osDpSetStatus(u32 status);
void osWritebackDCacheAll(void);
void speedgraphMarkerUpdate(void);
void speedgraphMarkerCommit(void);
void speedgraphMarkerHandler(u32 marker);
void joyPoll(void);
void joyRumblePakStop(void);
void musicFadeTick(void);
void viVsyncRelated(void);
void crashRenderFrame(void* addr);
void osDpGetCounters(u32* counters);
s32 __scTaskComplete(OSSched*,OSScTask*);
void __scHandleRetrace(OSSched*);
void __scHandleRSP(OSSched*);
void __scHandleRDP(OSSched*);
void __scMain(void*);
void __scAppendList(OSSched*,OSScTask*);
s32 __scSchedule(OSSched*,OSScTask**,OSScTask**,s32);
extern GEOriginalScheduler os_scheduler;
extern GEOriginalScClient gfxClient[3];
}
#endif
