#ifndef XB_GE_LIBULTRA_H
#define XB_GE_LIBULTRA_H
/* 007xb Phase 2: Xbox implementation of the libultra calls that the real
 * GoldenEye src/sched.c and src/boss.c use.  The ABIQueue matches the first
 * six fields of PR/os.h's OSMesgQueue on 32-bit Xbox. Locks/events are kept
 * OUTSIDE the game struct, as in the upstream PC port.
 *
 * The real N64 header keeps its public types. This file is a private ABI
 * bridge and intentionally does not re-define OSThread/OSMesgQueue names.
 * Compile the game with its real headers; link this implementation instead
 * of port/src/libultra.c. Every symbol uses C linkage.
 */
#include "xb_ge_os.h"
struct GERuntime;
struct XBGEAbiQueue {
    void* mtqueue;
    void* fullqueue;
    int validCount;
    int first;
    int msgCount;
    void** msg;
};
/* C++98 compile-time checks when building a true 32-bit Xbox target. */
typedef char XBGE_QUEUE_ABI_SIZE[(sizeof(void*)!=4 || sizeof(XBGEAbiQueue)==24)?1:-1];
/* OSThread header up to context; matching PR/os.h field order. */
struct XBGEAbiThreadHeader {
    void* next;
    int priority;
    void* queue;
    void* tlnext;
    unsigned short state;
    unsigned short flags;
    int id;
    int fp;
};
typedef char XBGE_THREAD_ABI_PREFIX_SIZE[(sizeof(void*)!=4 || sizeof(XBGEAbiThreadHeader)==28)?1:-1];
#ifdef __cplusplus
extern "C" {
#endif
void GEOSBindGameRuntime(GERuntime* runtime);
void GEOSUnbindGameRuntime(void);
/* Called only when the original GoldenEye gfx_pc interpreter is linked. */
void GEOSInstallRSPRunner(GEOSTaskProc renderer,void* user);
void GEOSInstallAudioRunner(GEOSTaskProc mixer,void* user);
void GEOSInstallDPRunner(GEOSDPProc renderer,void* user);
void osCreateMesgQueue(void* queue,void** storage,int count);
int osSendMesg(void* queue,void* msg,int blocking);
int osRecvMesg(void* queue,void** msg,int blocking);
void osEnqueueMesg(void* queue,void* msg);
void* osDequeueMesg(void* queue);
void* osMesgQueueLast(void* queue);
void osSetEventMesg(int eventId,void* queue,void* msg);
void osViSetEvent(void* queue,void* msg,unsigned count);
unsigned osGetCount(void);
unsigned long long osGetTime(void);
void osCreateThread(void* thread,int id,void (*entry)(void*),void* arg,void* stack,int priority);
void osStartThread(void* thread);
void osStopThread(void* thread);
void osDestroyThread(void* thread);
void osYieldThread(void);
int osGetThreadPri(void* thread);
void osSetThreadPri(void* thread,int priority);
int osSetTimer(void* timer,unsigned long long delay,unsigned long long repeat,void* q,void* msg);
int osStopTimer(void* timer);
/* Classic N64 OS hardware leaf calls: no false completion on failed DMA. */
int osPiStartDma(void* io,int priority,int direction,unsigned cartAddress,
                 void* target,unsigned bytes,void* queue);
int osPiRawStartDma(int direction,unsigned cartAddress,void* target,unsigned bytes);
unsigned osPiGetStatus(void);
int osAiSetFrequency(unsigned hz);
int osAiSetNextBuffer(void* pcm,unsigned bytes);
unsigned osAiGetLength(void);
void osSpTaskLoad(void* task);
int osSpTaskStartGo(void* task);
void osSpTaskYield(void);
int osSpTaskYielded(void* task);
int osSpTaskDone(void);
int osDpSetNextBuffer(void* buf,unsigned long long bytes);
void osDpGetCounters(unsigned* counters);
/* VI framebuffer equivalence is required by the original __scTaskReady().
   The pointer is a token, not a drawn frame. Rendering still requires gfx_pc. */
void* osViGetCurrentFramebuffer(void);
void* osViGetNextFramebuffer(void);
void osViSwapBuffer(void* frame);
void osViBlack(unsigned char black);
void osViSetXScale(float value);
void osViSetYScale(float value);
void osViRepeatLine(unsigned char line);
void osInitialize(void);
#ifdef __cplusplus
}
#endif
#endif
