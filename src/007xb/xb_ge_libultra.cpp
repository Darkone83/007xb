#include "xb_ge_libultra.h"
#include "xb_ge_runtime.h"
#include "xb_ge_audio.h"
#include <string.h>
#include <stdint.h>
#if defined(GE_XBOX_NATIVE32)
#include "ge_native_platform.h"
extern "C" int GEAsset32_SwapRaw16Dma(unsigned address, void *destination, unsigned bytes);
extern "C" void audioSetNextBuffer(const short *samples, unsigned bytes);
extern "C" unsigned audioGetAiLengthBytes(void);
extern "C" void sysFatalError(const char *format, ...);
#endif
#if !defined(XBOX) && !defined(_XBOX)
#include <time.h>
#endif

/* N64 game-owned structures are NOT enlarged with host synchronization.
   64 side slots cover all queues used during title boot and gameplay. */
struct QSlot {void* owner;GEOSQueue queue;};
static QSlot s_queues[64];
static GERuntime* s_runtime=0;
struct TSlot {
    void* owner;
    GEOSThread native;
    void (*entry)(void*);
    void* arg;
    int id,priority;
};
static TSlot s_threads[16];
static int s_audioReady=0;
static unsigned s_audioLastBytes=0;
#if defined(XBOX) || defined(_XBOX)
static CRITICAL_SECTION s_registryLock;
#else
static pthread_mutex_t s_registryLock;
#endif
static int s_registryReady=0;
static void RegistryLock(){
#if defined(XBOX) || defined(_XBOX)
    EnterCriticalSection(&s_registryLock);
#else
    pthread_mutex_lock(&s_registryLock);
#endif
}
static void RegistryUnlock(){
#if defined(XBOX) || defined(_XBOX)
    LeaveCriticalSection(&s_registryLock);
#else
    pthread_mutex_unlock(&s_registryLock);
#endif
}


static QSlot* FindQueue(void* o){
    if(!o||!s_registryReady)return 0;
    RegistryLock();
    QSlot* found=0;
    for(int i=0;i<64;++i)if(s_queues[i].owner==o){found=&s_queues[i];break;}
    RegistryUnlock();
    return found;
}
extern "C" void GEOSBindGameRuntime(GERuntime* r){
    /* Only safe before original mainproc begins. */
    if(!s_registryReady){
#if defined(XBOX) || defined(_XBOX)
        InitializeCriticalSection(&s_registryLock);
#else
        pthread_mutex_init(&s_registryLock,0);
#endif
        s_registryReady=1;
    }
    RegistryLock();
    s_runtime=r;
    memset(s_queues,0,sizeof(s_queues));
    memset(s_threads,0,sizeof(s_threads));
    s_audioReady=0;s_audioLastBytes=0;
    RegistryUnlock();
}
extern "C" void GEOSInstallRSPRunner(GEOSTaskProc cb,void* user){
    if(s_runtime)s_runtime->OS().SetSPRunner(cb,user);
}
extern "C" void GEOSInstallAudioRunner(GEOSTaskProc cb,void* user){
    if(s_runtime)s_runtime->OS().SetAudioRunner(cb,user);
}
extern "C" void GEOSInstallDPRunner(GEOSDPProc cb,void* user){
    if(s_runtime)s_runtime->OS().SetDPRunner(cb,user);
}
extern "C" void GEOSUnbindGameRuntime(void){
    /* Never unbind while game-owned threads are still alive. */
    if(s_registryReady)RegistryLock();
    for(int i=0;i<64;++i){
        if(s_queues[i].owner){GEOSQueueDestroy(&s_queues[i].queue);s_queues[i].owner=0;}
    }
    if(s_audioReady){GEAudio_Shutdown();s_audioReady=0;}
    s_runtime=0;
    if(s_registryReady){
        RegistryUnlock();
#if defined(XBOX) || defined(_XBOX)
        DeleteCriticalSection(&s_registryLock);
#else
        pthread_mutex_destroy(&s_registryLock);
#endif
        s_registryReady=0;
    }
}
extern "C" void osCreateMesgQueue(void* q,void** storage,int count){
    if(!s_runtime||!q||!storage||count<1||!s_registryReady)return;
    RegistryLock();
    QSlot* slot=0;
    for(int i=0;i<64;++i)if(s_queues[i].owner==q){slot=&s_queues[i];break;}
    if(!slot){for(int i=0;i<64;++i)if(!s_queues[i].owner){slot=&s_queues[i];break;}}
    if(!slot){RegistryUnlock();return;}
    if(slot->owner)GEOSQueueDestroy(&slot->queue);
    XBGEAbiQueue* abi=(XBGEAbiQueue*)q;
    memset(abi,0,sizeof(*abi));abi->msgCount=count;abi->msg=storage;
    if(!GEOSQueueInit(&slot->queue,storage,count)){RegistryUnlock();return;}
    slot->owner=q;
    GEOSQueueBindMirror(&slot->queue,&abi->validCount,&abi->first);
    RegistryUnlock();
}
extern "C" int osSendMesg(void* q,void* msg,int blocking){
    QSlot* s=FindQueue(q);if(!s)return -1;
    return GEOSSend(&s->queue,msg,blocking);
}
extern "C" int osRecvMesg(void* q,void** msg,int blocking){
    QSlot* s=FindQueue(q);if(!s)return -1;
    return GEOSReceive(&s->queue,msg,blocking);
}
extern "C" void osEnqueueMesg(void* q,void* msg){(void)osSendMesg(q,msg,GEOS_NOBLOCK);}
extern "C" void* osDequeueMesg(void* q){
    void* msg=0;return osRecvMesg(q,&msg,GEOS_NOBLOCK)==0?msg:0;
}
extern "C" void* osMesgQueueLast(void* q){
    QSlot* s=FindQueue(q);if(!s)return 0;
    /* Only diagnostic use; caller should not concurrently modify the queue. */
    int n=GEOSPending(&s->queue);
    return n>0?s->queue.buffer[(s->queue.first+n-1)%s->queue.capacity]:0;
}
extern "C" void osSetEventMesg(int id,void* q,void* msg){
    QSlot* s=FindQueue(q);
    if(s_runtime)s_runtime->OS().SetEvent((unsigned)id,s?&s->queue:0,msg);
}
extern "C" void osViSetEvent(void* q,void* msg,unsigned n){
    QSlot* s=FindQueue(q);
    if(s_runtime)s_runtime->OS().SetVIEvent(s?&s->queue:0,msg,n);
}
extern "C" unsigned osGetCount(void){return s_runtime?s_runtime->OS().GetCount():0;}
extern "C" unsigned long long osGetTime(void){
    /* GoldenEye libultra OSTime is measured in osClockRate (6.25MHz) cycles.
       Microseconds here made every osGetTime-based timer 6.25x too slow. */
    GEOSU64 us=s_runtime?s_runtime->OS().GetTimeUs():0ull;
    return (us/4ull)*25ull+((us%4ull)*25ull)/4ull;
}
static TSlot* FindThread(void* ptr){
    if(!ptr||!s_registryReady)return 0;
    RegistryLock();
    TSlot* out=0;
    for(int i=0;i<16;++i)if(s_threads[i].owner==ptr ||
        ((uintptr_t)ptr<0x10000u && s_threads[i].owner &&
         s_threads[i].id==(int)(uintptr_t)ptr)){out=&s_threads[i];break;}
    RegistryUnlock();return out;
}
extern "C" void osCreateThread(void* ptr,int id,void (*entry)(void*),void* arg,void* stack,int priority){
    (void)stack;
    if(!ptr||!entry||!s_runtime||!s_registryReady)return;
    RegistryLock();
    TSlot* t=0;
    for(int i=0;i<16;++i)if(s_threads[i].owner==ptr){t=&s_threads[i];break;}
    if(!t)for(int i=0;i<16;++i)if(!s_threads[i].owner){t=&s_threads[i];break;}
    if(!t){RegistryUnlock();return;}
    t->owner=ptr;t->entry=entry;t->arg=arg;t->id=id;t->priority=priority;
    XBGEAbiThreadHeader* os=(XBGEAbiThreadHeader*)ptr;
    os->id=id;os->priority=priority;os->state=1; /* OS_STATE_STOPPED */
    RegistryUnlock();
}
extern "C" void osStartThread(void* ptr){
    TSlot* t=FindThread(ptr);if(!t||t->native.started||!s_runtime)return;
    /* N64 idleproc is an infinite no-yield loop. The Xbox kernel already
       owns the idle thread, so leave this bookkeeping thread parked. */
    if(t->id==1){t->native.exited=1;((XBGEAbiThreadHeader*)t->owner)->state=1;return;}
    if(s_runtime->OS().Spawn(&t->native,t->entry,t->arg,t->id,t->priority))
        ((XBGEAbiThreadHeader*)t->owner)->state=2; /* OS_STATE_RUNNABLE */
}
extern "C" void osStopThread(void* ptr){
    TSlot* t=FindThread(ptr);if(!t)return;
    /* Not a forced termination: game entry must reach its quiescent point. */
    ((XBGEAbiThreadHeader*)t->owner)->state=1;
}
extern "C" void osDestroyThread(void* ptr){
    TSlot* t=FindThread(ptr);if(!t)return;
    if(t->native.started && !t->native.exited)return;
    if(t->native.started && s_runtime)s_runtime->OS().Join(&t->native,0);
    t->owner=0;
}
extern "C" void osYieldThread(void){
#if defined(XBOX) || defined(_XBOX)
    Sleep(1);
#else
    /* Host test: give other OS queues and scheduler threads a turn. */
    struct timespec t;t.tv_sec=0;t.tv_nsec=1000000L;nanosleep(&t,0);
#endif
}
extern "C" int osGetThreadPri(void* ptr){
    TSlot* t=FindThread(ptr);return t?t->priority:0;
}
extern "C" void osSetThreadPri(void* ptr,int priority){
    TSlot* t=FindThread(ptr);
    if(t){t->priority=priority;((XBGEAbiThreadHeader*)t->owner)->priority=priority;}
}
extern "C" int osSetTimer(void* tag,unsigned long long delay,unsigned long long repeat,void* q,void* msg){
    QSlot* s=FindQueue(q);
    return s_runtime&&s?s_runtime->OS().SetTimer(tag,delay,repeat,&s->queue,msg):-1;
}
extern "C" int osStopTimer(void* tag){
    return s_runtime?s_runtime->OS().StopTimer(tag):-1;
}
/* Unlike the N64, the Xbox allocates native .bss/stack/heap pointers.
   Game addresses use the original PC-port V1 (0x70...), K0 (0x80...), or
   K1 (0xA0...) ranges.  An ordinary Xbox pointer MUST NOT be
   interpreted as a DRAM offset.  Only explicitly-tagged N64 addresses are
   translated; ordinary pointers remain ordinary pointers. */
#if defined(XBOX) || defined(_XBOX)
/* Xbox kernel export 179; already used by the linked XDK libraries.
   CreateThread uses kernel stacks in system virtual memory, so physical
   RAM size is not a valid upper bound for a native DMA destination. */
extern "C" __declspec(dllimport) ULONG __stdcall MmQueryAddressProtect(void* address);
static int NativeDMATargetWritable(uintptr_t address,unsigned bytes){
    const GEU32 nativeAddress=(GEU32)address;
    if(address!=(uintptr_t)nativeAddress || nativeAddress<0x10000u || !bytes ||
       bytes-1u>0xFFFFFFFFu-nativeAddress)return 0;
    const uintptr_t lastPage=(address+bytes-1u)&~(uintptr_t)0xFFFu;
    uintptr_t page=address&~(uintptr_t)0xFFFu;
    for(;;){
        const ULONG protection=MmQueryAddressProtect((void*)page);
        /* Xbox PAGE_NOACCESS=0x01, PAGE_GUARD=0x100,
           PAGE_READWRITE=0x04 and PAGE_EXECUTE_READWRITE=0x40. */
        if((protection&0x101u) || !(protection&0x44u))return 0;
        if(page==lastPage)return 1;
        page+=0x1000u;
    }
}
#endif
/* Synchronous, animation-only ROM cache read for model.c.  The generic
 * romCopy operation immediately consumes the one DMA-completion message;
 * this path returns the same bytes without constructing that message pair.
 * Other DMA callers, cache misses, and all unrelated cartridge reads retain
 * the original translation, queue, and error handling. */
#if defined(GE_XBOX_NATIVE32)
extern "C" int GE_XboxTryCopyCachedAnimFrame(unsigned cart,void* target,unsigned bytes){
    if(!s_runtime || !target || !bytes)return 0;
    if(!s_runtime->Cart().ReadCachedAnimation(cart,target,bytes))return 0;
    /* Preserve the original DMA post-read RAW16 behavior, even though the
       verified animation region never overlaps registered audio samples. */
    if(!GEAsset32_SwapRaw16Dma(cart,target,bytes))
        sysFatalError("Cached animation ROM fixup failed (addr=%08X size=%u)",cart,bytes);
    return 1;
}
#endif
static int DMARead(int dir,unsigned cart,void* target,unsigned bytes){
    if(!s_runtime||dir!=0||!target)return -1; /* OS_READ=0 */
    if(!bytes)return 0;
    GEDram& ram=s_runtime->Dram();
    const uintptr_t p=(uintptr_t)target;
    void* mapped=target;
    const GEU32 va=(GEU32)p;
    GEU32 physical=0;
    if(ram.ToPhysical(target,&physical)){
        mapped=ram.Resolve(physical,bytes);
        if(!mapped)return -1;
    }else if(p<=0xFFFFFFFFull &&
       ((va>=0x70000000u && va<0x70800000u) ||
        (va>=0x80000000u && va<0x80800000u) ||
        (va>=0xA0000000u && va<0xA0800000u)
#if defined(GE_XBOX_NATIVE32)
        || (va>=0xF0000000u && va<0xF0800000u)
#endif
       )){
        mapped=ram.Resolve(va&0x007FFFFFu,bytes);
        if(!mapped)return -1;
    }else{
#if defined(XBOX) || defined(_XBOX)
        if(!NativeDMATargetWritable(p,bytes))return -1;
#else
        /* On the 64-bit host only true host pointers (above 4 GiB) pass. */
        if(p<0x10000u || p<=0xFFFFFFFFull)return -1;
#endif
    }
    if(!s_runtime->Cart().Read(cart,mapped,bytes))return -1;
#if defined(GE_XBOX_NATIVE32)
    if(!GEAsset32_SwapRaw16Dma(cart,mapped,bytes))return -1;
#endif
    return 0;
}
extern "C" int osPiStartDma(void* io,int priority,int direction,unsigned cart,void* target,unsigned bytes,void* q){
    (void)priority;
    const int rc=DMARead(direction,cart,target,bytes);
#if defined(GE_XBOX_NATIVE32)
    if(rc!=0)sysFatalError("Cartridge DMA failed (address=%08X target=%08X bytes=%u)",cart,(unsigned)(uintptr_t)target,bytes);
#endif
    /* A failing DMA may not report success or allow the game to continue
       with uninitialized model/texture memory. */
    if(rc==0&&q)osSendMesg(q,io?io:(void*)1,GEOS_BLOCK);
    return rc==0?1:-1;
}
extern "C" int osPiRawStartDma(int direction,unsigned cart,void* target,unsigned bytes){
    const int rc=DMARead(direction,cart,target,bytes);
#if defined(GE_XBOX_NATIVE32)
    if(rc!=0)sysFatalError("Raw cartridge DMA failed (address=%08X target=%08X bytes=%u)",cart,(unsigned)(uintptr_t)target,bytes);
#endif
    return rc==0?1:-1;
}
extern "C" unsigned osPiGetStatus(void){return 0u;}
extern "C" int osAiSetFrequency(unsigned hz){
    return hz==22050u?22050:-1;
}
extern "C" int osAiSetNextBuffer(void* pcm,unsigned bytes){
    if(!s_runtime||!pcm||!bytes||(bytes&3u)||bytes>16384u)return -1;
#if defined(GE_XBOX_NATIVE32)
    audioSetNextBuffer((const short *)pcm,bytes);
    s_audioLastBytes=bytes;
    return GENativePlatform_HasFatalError()?-1:1;
#else
    if(!s_audioReady){if(!GEAudio_Init())return -1;s_audioReady=1;}
    if(!GEAudio_SubmitPCM((const short*)pcm,bytes/4u))return -1;
    s_audioLastBytes=bytes;return 1;
#endif
}
extern "C" unsigned osAiGetLength(void){
#if defined(GE_XBOX_NATIVE32)
    return audioGetAiLengthBytes();
#else
    if(!s_audioReady)return 0;
    unsigned queued=GEAudio_QueuedFrames()*4u;
    return queued<s_audioLastBytes?queued:s_audioLastBytes;
#endif
}

#if defined(GE_XBOX_NATIVE32)
static void NativeTaskFailure(const char* kind){
    /* The interpreter/video backend records its concrete failure before
       returning false. sysFatalError replaces the shared error text, so
       promote that detail verbatim instead of erasing it with the fallback.
       It formats into a separate buffer before updating the error store. */
    const char* detail=GENativePlatform_Error();
    if(detail && detail[0])sysFatalError("%s",detail);
    else sysFatalError("%s task failed before completion",kind);
}
#endif

/* Stage-boundary scheduling counters make real Xbox drops measurable
 * without issuing any trace writes from the renderer or VI timer. */
extern "C" void GEOSNativeSchedulerSnapshot(unsigned* retraces,unsigned* dropped){
    if(retraces)*retraces=s_runtime?s_runtime->OS().Retraces():0u;
    if(dropped)*dropped=s_runtime?s_runtime->OS().DroppedRetraces():0u;
}

/* Worker graphics failures occur after osSpTaskStartGo returned. Preserve
 * the native error and never post a fake completion. */
extern "C" void GEOSNativeGraphicsFailure(void){
#if defined(GE_XBOX_NATIVE32)
    NativeTaskFailure("Graphics");
#endif
}

/* Exact libultra graphics/audio task hand-off used by src/sched.c.  The
   upstream OSTask union begins with the u32 t.type: 1=graphics, 2=audio. */
extern "C" void osSpTaskLoad(void* task){(void)task;}
extern "C" int osSpTaskStartGo(void* task){
    if(!s_runtime||!task)return -1;
    unsigned kind=*(const unsigned*)task;
    if(kind!=1u && kind!=2u)return -1;
    /* Never send a bogus RSP_DONE (or DP_DONE) if the software RSP
       interpreter or audio task callback has not actually executed. */
    const int result=s_runtime->OS().RunSPTask(task,kind==1u);
#if defined(GE_XBOX_NATIVE32)
    if(result!=0)NativeTaskFailure(kind==1u?"Graphics":"Audio");
#endif
    return result;
}
extern "C" void osSpTaskYield(void){}
extern "C" int osSpTaskYielded(void* task){(void)task;return 0;}
extern "C" int osSpTaskDone(void){return 1;}
extern "C" int osDpSetNextBuffer(void* buf,unsigned long long bytes){
    const int result=s_runtime?s_runtime->OS().RunDPTask(buf,bytes):-1;
#if defined(GE_XBOX_NATIVE32)
    if(result!=0)NativeTaskFailure("RDP");
#endif
    return result;
}
extern "C" void osDpGetCounters(unsigned* c){if(c)memset(c,0,4u*sizeof(unsigned));}
/* Original sched.c compares current and next CFB tokens before submitting
   graphics work.  A single stable token is correct for a software RDP that
   presents through the existing XBVideo back buffer.  It is not a video
   renderer and does not falsely report any task completion. */
static unsigned s_framebufferToken=0x0070B083u;
extern "C" void* osViGetCurrentFramebuffer(void){return &s_framebufferToken;}
extern "C" void* osViGetNextFramebuffer(void){return &s_framebufferToken;}
extern "C" void osViSwapBuffer(void*){}
extern "C" void osViBlack(unsigned char){}
extern "C" void osViSetXScale(float){}
extern "C" void osViSetYScale(float){}
extern "C" void osViRepeatLine(unsigned char){}
extern "C" void osInitialize(void){}
