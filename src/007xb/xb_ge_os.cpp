#include "xb_ge_os.h"
#include <string.h>
#include <stddef.h>
#if !defined(GEOS_XBOX_AUDIO_PRIORITY_BOOST)
/* Original AUDI_THREAD_ID=4 runs at priority 20 (MAIN=10). Default Win32
 * CreateThread priority loses that distinction on the single-core Xbox. */
#define GEOS_XBOX_AUDIO_PRIORITY_BOOST 1
#endif

#if defined(GE_XBOX_NATIVE32)
extern "C" void GEOSNativeGraphicsFailure(void);
#endif
#if !defined(XBOX) && !defined(_XBOX)
#include <time.h>
#include <unistd.h>
#include <errno.h>
#endif

/* Native game and renderer calls include model/texture decompression and
 * SDK work on the same thread. Reserve their full call-chain headroom
 * explicitly; the original N64 stack arrays are not used by CreateThread.
 * Three engine workers plus the retrace thread reserve 832 KiB in total. */
enum {
    GEOS_WORKER_STACK_BYTES = 256u * 1024u,
    GEOS_RETRACE_STACK_BYTES = 64u * 1024u,
    GEOS_GRAPHICS_STACK_BYTES = 256u * 1024u
};

/* Xbox KTHREAD prefix, independently documented by XboxDev/nxdk's
 * xboxkrnl.h and Cxbx-Reloaded's EmuFS.cpp: StackBase=0x1C,
 * StackLimit=0x20. The supplied old RXDK XapiThreadStartup obtains the
 * current KTHREAD from FS:[0x28]. Read its bounds directly instead of
 * assuming desktop TIB behavior.
 * Keep this private so no newer SDK kernel declaration is required. */
struct GEOSKernelStackPrefix {
    unsigned char reserved[0x1C];
    unsigned stackBase,stackLimit;
};
typedef char GEOSStackWordSize[(sizeof(unsigned)==4)?1:-1];
typedef char GEOSStackBaseOffset[(offsetof(GEOSKernelStackPrefix,stackBase)==0x1C)?1:-1];
typedef char GEOSStackLimitOffset[(offsetof(GEOSKernelStackPrefix,stackLimit)==0x20)?1:-1];

static int GEOSValidStackBounds(unsigned base,unsigned limit,unsigned pointer) {
    if(!limit || base<=limit || ((base|limit)&3u))return 0;
    const unsigned bytes=base-limit;
    if(bytes<4096u || bytes>16u*1024u*1024u)return 0;
    return pointer>=limit && pointer<base;
}
extern "C" int GEOSGetCurrentStackSnapshot(unsigned* stackBase,
    unsigned* stackLimit,unsigned* stackPointer) {
    if(stackBase)*stackBase=0;
    if(stackLimit)*stackLimit=0;
    if(stackPointer)*stackPointer=0;
    if(!stackBase || !stackLimit || !stackPointer)return 0;
#if (defined(XBOX) || defined(_XBOX)) && (defined(_M_IX86) || defined(__i386__))
    unsigned currentThread=0,pointer=0;
#if defined(_MSC_VER)
    __asm {
        mov eax,fs:[28h]
        mov currentThread,eax
        mov eax,esp
        mov pointer,eax
    }
#else
    __asm__ __volatile__("movl %%fs:0x28,%0\n\tmovl %%esp,%1"
        : "=r"(currentThread),"=r"(pointer) : : "memory");
#endif
    if(!currentThread || (currentThread&3u))return 0;
    const volatile GEOSKernelStackPrefix* thread=
        (const volatile GEOSKernelStackPrefix*)currentThread;
    const unsigned base=thread->stackBase,limit=thread->stackLimit;
    if(!GEOSValidStackBounds(base,limit,pointer))return 0;
    *stackBase=base;*stackLimit=limit;*stackPointer=pointer;
    return 1;
#else
    return 0;
#endif
}

bool GEOSQueueInit(GEOSQueue* q,GEOSMesg* slots,int capacity){
    if(!q||!slots||capacity<1||capacity>1024)return false;
    memset(q,0,sizeof(*q));q->buffer=slots;q->capacity=capacity;
#if defined(XBOX) || defined(_XBOX)
    InitializeCriticalSection(&q->lock);
    q->notEmpty=CreateEvent(0,TRUE,FALSE,0);
    q->notFull=CreateEvent(0,TRUE,TRUE,0);
    if(!q->notEmpty||!q->notFull){
        if(q->notEmpty)CloseHandle(q->notEmpty);
        if(q->notFull)CloseHandle(q->notFull);
        DeleteCriticalSection(&q->lock);return false;
    }
#else
    if(pthread_mutex_init(&q->lock,0)!=0)return false;
    if(pthread_cond_init(&q->notEmpty,0)!=0){pthread_mutex_destroy(&q->lock);return false;}
    if(pthread_cond_init(&q->notFull,0)!=0){
        pthread_cond_destroy(&q->notEmpty);pthread_mutex_destroy(&q->lock);return false;
    }
#endif
    q->ready=1;return true;
}
void GEOSQueueBindMirror(GEOSQueue* q,volatile int* count,volatile int* first){
    if(!q||!q->ready)return;
#if defined(XBOX) || defined(_XBOX)
    EnterCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
#endif
    q->abiCount=count;q->abiFirst=first;
    if(count)*count=q->count;
    if(first)*first=q->first;
#if defined(XBOX) || defined(_XBOX)
    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_unlock(&q->lock);
#endif
}
static void GEOSMirrorLocked(GEOSQueue* q){
    if(q->abiCount)*q->abiCount=q->count;
    if(q->abiFirst)*q->abiFirst=q->first;
}
void GEOSQueueDestroy(GEOSQueue* q){
    if(!q||!q->ready)return;
    /* Caller must join all producer/consumer threads before destroying. */
    q->ready=0;
#if defined(XBOX) || defined(_XBOX)
    CloseHandle(q->notEmpty);CloseHandle(q->notFull);
    DeleteCriticalSection(&q->lock);
#else
    pthread_cond_destroy(&q->notEmpty);pthread_cond_destroy(&q->notFull);
    pthread_mutex_destroy(&q->lock);
#endif
    q->buffer=0;q->capacity=q->first=q->count=0;
}
int GEOSSend(GEOSQueue* q,GEOSMesg msg,int blocking){
    if(!q||!q->ready)return -1;
#if defined(XBOX) || defined(_XBOX)
    for(;;){
        EnterCriticalSection(&q->lock);
        if(q->count<q->capacity){
            q->buffer[(q->first+q->count)%q->capacity]=msg;++q->count;
            SetEvent(q->notEmpty);
            if(q->count==q->capacity)ResetEvent(q->notFull);
            GEOSMirrorLocked(q);
            LeaveCriticalSection(&q->lock);return 0;
        }
        LeaveCriticalSection(&q->lock);
        if(blocking!=GEOS_BLOCK)return -1;
        if(WaitForSingleObject(q->notFull,INFINITE)!=WAIT_OBJECT_0)return -1;
    }
#else
    pthread_mutex_lock(&q->lock);
    while(q->count==q->capacity){
        if(blocking!=GEOS_BLOCK){pthread_mutex_unlock(&q->lock);return -1;}
        pthread_cond_wait(&q->notFull,&q->lock);
    }
    q->buffer[(q->first+q->count)%q->capacity]=msg;++q->count;
    GEOSMirrorLocked(q);
    pthread_cond_signal(&q->notEmpty);pthread_mutex_unlock(&q->lock);return 0;
#endif
}
int GEOSReceive(GEOSQueue* q,GEOSMesg* msg,int blocking){
    if(!q||!q->ready)return -1;
#if defined(XBOX) || defined(_XBOX)
    for(;;){
        EnterCriticalSection(&q->lock);
        if(q->count){
            GEOSMesg val=q->buffer[q->first];
            q->first=(q->first+1)%q->capacity;--q->count;
            SetEvent(q->notFull);
            if(!q->count)ResetEvent(q->notEmpty);
            GEOSMirrorLocked(q);
            LeaveCriticalSection(&q->lock);
            if(msg)*msg=val;
            return 0;
        }
        LeaveCriticalSection(&q->lock);
        if(blocking!=GEOS_BLOCK)return -1;
        if(WaitForSingleObject(q->notEmpty,INFINITE)!=WAIT_OBJECT_0)return -1;
    }
#else
    pthread_mutex_lock(&q->lock);
    while(!q->count){
        if(blocking!=GEOS_BLOCK){pthread_mutex_unlock(&q->lock);return -1;}
        pthread_cond_wait(&q->notEmpty,&q->lock);
    }
    GEOSMesg val=q->buffer[q->first];
    q->first=(q->first+1)%q->capacity;--q->count;
    GEOSMirrorLocked(q);
    pthread_cond_signal(&q->notFull);pthread_mutex_unlock(&q->lock);
    if(msg)*msg=val;
            return 0;
#endif
}
int GEOSPending(GEOSQueue* q){
    if(!q||!q->ready)return -1;
#if defined(XBOX) || defined(_XBOX)
    EnterCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
#endif
    int n=q->count;
#if defined(XBOX) || defined(_XBOX)
    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_unlock(&q->lock);
#endif
    return n;
}
int GEOSPostCritical(GEOSQueue* q,GEOSMesg msg){
    if(!q||!q->ready)return -1;
#if defined(XBOX) || defined(_XBOX)
    EnterCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
#endif
    if(q->count==q->capacity){
        q->first=(q->first+1)%q->capacity;--q->count;++q->dropped;
    }
    q->buffer[(q->first+q->count)%q->capacity]=msg;++q->count;
#if defined(XBOX) || defined(_XBOX)
    SetEvent(q->notEmpty);
    if(q->count==q->capacity)ResetEvent(q->notFull);
    GEOSMirrorLocked(q);
    LeaveCriticalSection(&q->lock);
#else
    GEOSMirrorLocked(q);
    pthread_cond_signal(&q->notEmpty);pthread_mutex_unlock(&q->lock);
#endif
    return 0;
}

static GEOSU64 GEOSNowUs(){
#if defined(XBOX) || defined(_XBOX)
    LARGE_INTEGER freq,t;
    if(!QueryPerformanceFrequency(&freq)||!QueryPerformanceCounter(&t)||freq.QuadPart<=0)return (GEOSU64)GetTickCount()*1000u;
    GEOSU64 f=(GEOSU64)freq.QuadPart,v=(GEOSU64)t.QuadPart;
    return (v/f)*1000000ull+(v%f)*1000000ull/f;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    return (GEOSU64)ts.tv_sec*1000000ull+(GEOSU64)ts.tv_nsec/1000ull;
#endif
}
static void GEOSSleepMs(unsigned ms){
#if defined(XBOX) || defined(_XBOX)
    Sleep(ms);
#else
    struct timespec t;t.tv_sec=ms/1000u;t.tv_nsec=(long)(ms%1000u)*1000000L;
    while(nanosleep(&t,&t)!=0 && errno==EINTR){}
#endif
}
GEOSKernel::GEOSKernel():timersReady_(0),viQ_(0),viMsg_(0),viCount_(1),viDiv_(0),spTask_(0),audioTask_(0),dpTask_(0),spUser_(0),audioUser_(0),dpUser_(0),gfxWorkerReady_(0),gfxWorkerFailed_(0),gfxBusy_(0),gfxStop_(0),hz_(60),ready_(0),running_(0),retraces_(0),droppedRetraces_(0){
    memset(events_,0,sizeof(events_));
    memset(timers_,0,sizeof(timers_));
    memset(&gfxQueue_,0,sizeof(gfxQueue_));
    gfxQueueStorage_[0]=0;
#if defined(XBOX) || defined(_XBOX)
    gfxThread_=0;
    retraceThread_=0;
#else
    memset(&gfxThread_,0,sizeof(gfxThread_));
    memset(&retraceThread_,0,sizeof(retraceThread_));
#endif
}
bool GEOSKernel::Init(unsigned hz){
    if(ready_)Shutdown();
    if(hz!=50&&hz!=60)return false;
    gfxWorkerReady_=gfxWorkerFailed_=gfxBusy_=gfxStop_=0;
    memset(events_,0,sizeof(events_));
    viQ_=0;viMsg_=0;viCount_=1;viDiv_=0;
    spTask_=audioTask_=0;dpTask_=0;spUser_=audioUser_=dpUser_=0;
    memset(timers_,0,sizeof(timers_));
#if defined(XBOX) || defined(_XBOX)
    InitializeCriticalSection(&timerLock_);timersReady_=1;
#else
    if(pthread_mutex_init(&timerLock_,0)!=0)return false;
    timersReady_=1;
#endif
    hz_=hz;retraces_=droppedRetraces_=0;ready_=1;return true;
}
void GEOSKernel::SetVIEvent(GEOSQueue* q,GEOSMesg msg,unsigned count){
    viQ_=q;viMsg_=msg;viCount_=count?count:1;viDiv_=0;
}
void GEOSKernel::SetEvent(unsigned id,GEOSQueue* q,GEOSMesg msg){
    if(id>=16)return;
    events_[id].q=q;events_[id].msg=msg;
}
void GEOSKernel::SetSPRunner(GEOSTaskProc cb,void* user){
    spTask_=cb;spUser_=user;
#if GEOS_ASYNC_GFX
    /* Must precede start of the original game/scheduler threads. Failed
     * worker creation leaves the proven synchronous path usable. */
    if(cb && ready_ && !gfxWorkerReady_)StartGfxWorker();
#endif
}

bool GEOSKernel::StartGfxWorker(){
#if GEOS_ASYNC_GFX
    if(gfxWorkerReady_)return true;
    if(!GEOSQueueInit(&gfxQueue_,gfxQueueStorage_,1))return false;
    gfxWorkerFailed_=gfxBusy_=gfxStop_=0;
#if defined(XBOX) || defined(_XBOX)
    gfxThread_=CreateThread(0,GEOS_GRAPHICS_STACK_BYTES,&XboxGfxWorkerEntry,this,0,0);
    if(!gfxThread_){GEOSQueueDestroy(&gfxQueue_);return false;}
#else
    pthread_attr_t attr;pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr,GEOS_GRAPHICS_STACK_BYTES);
    int rc=pthread_create(&gfxThread_,&attr,&GfxWorkerEntry,this);
    pthread_attr_destroy(&attr);
    if(rc!=0){GEOSQueueDestroy(&gfxQueue_);return false;}
#endif
    gfxWorkerReady_=1;
    return true;
#else
    return false;
#endif
}
void* GEOSKernel::GfxWorkerEntry(void* state){
    ((GEOSKernel*)state)->GfxWorkerLoop();return 0;
}
#if defined(XBOX) || defined(_XBOX)
DWORD WINAPI GEOSKernel::XboxGfxWorkerEntry(LPVOID state){
    GfxWorkerEntry(state);return 0;
}
#endif
void GEOSKernel::GfxWorkerLoop(){
    for(;;){
        GEOSMesg task=0;
        if(GEOSReceive(&gfxQueue_,&task,GEOS_BLOCK)!=0)break;
        if(gfxStop_)break;
        if(!task)continue;
        const bool succeeded=spTask_ && spTask_(task,spUser_);
        /* Release ownership only after the D3D8 task has returned. The
         * original scheduler cannot submit another GFX task until its
         * completion interrupts are processed. */
#if defined(XBOX) || defined(_XBOX)
        InterlockedExchange((LPLONG)&gfxBusy_,0);
#else
        __sync_lock_release(&gfxBusy_);
#endif
        if(!succeeded){
            gfxWorkerFailed_=1;
#if defined(GE_XBOX_NATIVE32)
            /* Asynchronous failure cannot be propagated through the return
             * value of osSpTaskStartGo. Preserve the renderer's exact error. */
            GEOSNativeGraphicsFailure();
#endif
            break; /* Never fabricate SP/DP completion for a failed draw. */
        }
        /* SP before DP, matching the synchronous code and original sched.c. */
        if(events_[GEOS_EVENT_SP].q)
            GEOSPostCritical(events_[GEOS_EVENT_SP].q,events_[GEOS_EVENT_SP].msg);
        if(events_[GEOS_EVENT_DP].q)
            GEOSPostCritical(events_[GEOS_EVENT_DP].q,events_[GEOS_EVENT_DP].msg);
    }
}

void GEOSKernel::SetAudioRunner(GEOSTaskProc cb,void* user){audioTask_=cb;audioUser_=user;}
void GEOSKernel::SetDPRunner(GEOSDPProc cb,void* user){dpTask_=cb;dpUser_=user;}
static GEOSU64 CyclesToUs(GEOSU64 cycles){
    return (cycles/6250000ull)*1000000ull+((cycles%6250000ull)*1000000ull)/6250000ull;
}
int GEOSKernel::SetTimer(void* tag,GEOSU64 delayCycles,GEOSU64 repeatCycles,
                        GEOSQueue* q,GEOSMesg msg){
    if(!ready_||!tag||!q||!q->ready||!timersReady_)return -1;
#if defined(XBOX) || defined(_XBOX)
    EnterCriticalSection(&timerLock_);
#else
    pthread_mutex_lock(&timerLock_);
#endif
    int idx=-1;
    for(int i=0;i<16;++i){
        if(timers_[i].tag==tag){idx=i;break;}
        if(idx<0&&!timers_[i].active)idx=i;
    }
    if(idx>=0){
        TimerSlot& t=timers_[idx];t.tag=tag;t.queue=q;t.msg=msg;
        t.dueUs=GEOSNowUs()+CyclesToUs(delayCycles);
        t.repeatUs=CyclesToUs(repeatCycles);
        t.active=1;
    }
#if defined(XBOX) || defined(_XBOX)
    LeaveCriticalSection(&timerLock_);
#else
    pthread_mutex_unlock(&timerLock_);
#endif
    return idx>=0?0:-1;
}
int GEOSKernel::StopTimer(void* tag){
    if(!ready_||!timersReady_||!tag)return -1;
#if defined(XBOX) || defined(_XBOX)
    EnterCriticalSection(&timerLock_);
#else
    pthread_mutex_lock(&timerLock_);
#endif
    int rc=-1;
    for(int i=0;i<16;++i){
        if(timers_[i].tag==tag&&timers_[i].active){timers_[i].active=0;rc=0;break;}
    }
#if defined(XBOX) || defined(_XBOX)
    LeaveCriticalSection(&timerLock_);
#else
    pthread_mutex_unlock(&timerLock_);
#endif
    return rc;
}
void GEOSKernel::ServiceTimers(GEOSU64 now){
    if(!timersReady_)return;
#if defined(XBOX) || defined(_XBOX)
    EnterCriticalSection(&timerLock_);
#else
    pthread_mutex_lock(&timerLock_);
#endif
    for(int i=0;i<16;++i){
        TimerSlot& t=timers_[i];
        if(!t.active||now<t.dueUs)continue;
        if(t.queue&&t.queue->ready)GEOSSend(t.queue,t.msg,GEOS_NOBLOCK);
        if(t.repeatUs){t.dueUs=now+t.repeatUs;}else t.active=0;
    }
#if defined(XBOX) || defined(_XBOX)
    LeaveCriticalSection(&timerLock_);
#else
    pthread_mutex_unlock(&timerLock_);
#endif
}
int GEOSKernel::RunSPTask(void* task,bool graphics){
    GEOSTaskProc runner=graphics?spTask_:audioTask_;
    void* user=graphics?spUser_:audioUser_;
    if(!ready_||!task||!runner || gfxWorkerFailed_)return -1;
#if GEOS_ASYNC_GFX
    if(graphics && gfxWorkerReady_){
        /* Native32 uses a single physical CPU, but this frees the N64
         * scheduler thread to service VI/audio while D3D8 executes. Never
         * allow concurrent Fast3D tasks or speculative completion events. */
#if defined(XBOX) || defined(_XBOX)
        if(InterlockedCompareExchange((LPLONG)&gfxBusy_,1,0)!=0)return -1;
#else
        if(!__sync_bool_compare_and_swap(&gfxBusy_,0,1))return -1;
#endif
        if(GEOSSend(&gfxQueue_,task,GEOS_NOBLOCK)==0)return 0;
#if defined(XBOX) || defined(_XBOX)
        InterlockedExchange((LPLONG)&gfxBusy_,0);
#else
        __sync_lock_release(&gfxBusy_);
#endif
        return -1;
    }
#endif
    if(!runner(task,user))return -1;
    /* Original __scMain must receive SP before DP on a graphics task.
       An audio task produces SP only. Neither event is speculative. */
    if(events_[GEOS_EVENT_SP].q)
        GEOSPostCritical(events_[GEOS_EVENT_SP].q,events_[GEOS_EVENT_SP].msg);
    if(graphics && events_[GEOS_EVENT_DP].q)
        GEOSPostCritical(events_[GEOS_EVENT_DP].q,events_[GEOS_EVENT_DP].msg);
    return 0;
}
int GEOSKernel::RunDPTask(void* buffer,GEOSU64 bytes){
    if(!ready_||!dpTask_||!buffer||!bytes)return -1;
    if(!dpTask_(buffer,bytes,dpUser_))return -1;
    PostDPComplete();
    return 0;
}
void GEOSKernel::PostDPComplete(){
    if(events_[GEOS_EVENT_DP].q)GEOSPostCritical(events_[GEOS_EVENT_DP].q,events_[GEOS_EVENT_DP].msg);
}
GEOSU64 GEOSKernel::GetTimeUs()const{return GEOSNowUs();}
GEOSU32 GEOSKernel::GetCount()const{
    GEOSU64 us=GEOSNowUs();
    return (GEOSU32)((us*465525ull)/10000ull); /* 46.5525 MHz, wraps as N64 */
}
void GEOSKernel::TickLoop(){
    GEOSU64 t0=GEOSNowUs(),frames=0;
    while(running_){
        GEOSU64 due=t0+(frames+1ull)*1000000ull/(GEOSU64)hz_;
        GEOSU64 now=GEOSNowUs();
        if(now<due){
            GEOSSleepMs(due-now>4000ull?4u:1u);continue;
        }
        ++frames;
        if(now>due+100000ull){t0=now;frames=0;} /* no burst after a stall */
        ++retraces_;
        ServiceTimers(now);
        if(!viQ_||!viQ_->ready)continue;
        if(++viDiv_<viCount_)continue;
        viDiv_=0;
        /* Like the PC libultra port: leave >=2 message slots for the RSP
           and RDP completion events. Stale VI retraces may be discarded. */
        int pending=GEOSPending(viQ_);
        if(pending<0 || pending>=viQ_->capacity-2 || GEOSSend(viQ_,viMsg_,GEOS_NOBLOCK)!=0){
            ++droppedRetraces_;
        }
    }
}
void* GEOSKernel::ThreadEntry(void* state){((GEOSKernel*)state)->TickLoop();return 0;}
#if defined(XBOX) || defined(_XBOX)
DWORD WINAPI GEOSKernel::XboxThreadEntry(LPVOID state){ThreadEntry(state);return 0;}
DWORD WINAPI GEOSKernel::XboxGameEntry(LPVOID state){
    GEOSThread* thread=(GEOSThread*)state;
    thread->entry(thread->arg);thread->exited=1;return 0;
}
#else
void* GEOSKernel::HostGameEntry(void* state){
    GEOSThread* thread=(GEOSThread*)state;
    thread->entry(thread->arg);thread->exited=1;return 0;
}
#endif
bool GEOSKernel::StartRetrace(){
    if(!ready_)return false;
    if(running_)return true;
    running_=1;
#if defined(XBOX) || defined(_XBOX)
    retraceThread_=CreateThread(0,GEOS_RETRACE_STACK_BYTES,&XboxThreadEntry,this,0,0);
    if(!retraceThread_){running_=0;return false;}
#else
    pthread_attr_t attr;pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr,GEOS_RETRACE_STACK_BYTES);
    int rc=pthread_create(&retraceThread_,&attr,&ThreadEntry,this);
    pthread_attr_destroy(&attr);
    if(rc!=0){running_=0;return false;}
#endif
    return true;
}
void GEOSKernel::Shutdown(){
    /* The owner never calls Shutdown while the original game is alive.
     * Wake and join the graphics worker before destroying its queue. */
    if(gfxWorkerReady_){
        gfxStop_=1;
        GEOSSend(&gfxQueue_,0,GEOS_BLOCK);
#if defined(XBOX) || defined(_XBOX)
        WaitForSingleObject(gfxThread_,INFINITE);
        CloseHandle(gfxThread_);gfxThread_=0;
#else
        pthread_join(gfxThread_,0);
#endif
        GEOSQueueDestroy(&gfxQueue_);
        gfxWorkerReady_=0;
    }
    if(running_){
        running_=0;
#if defined(XBOX) || defined(_XBOX)
        if(retraceThread_){WaitForSingleObject(retraceThread_,5000);CloseHandle(retraceThread_);retraceThread_=0;}
#else
        pthread_join(retraceThread_,0);
#endif
    }
    viQ_=0;ready_=0;spTask_=audioTask_=0;dpTask_=0;
    spUser_=audioUser_=dpUser_=0;
    if(timersReady_){
#if defined(XBOX) || defined(_XBOX)
        DeleteCriticalSection(&timerLock_);
#else
        pthread_mutex_destroy(&timerLock_);
#endif
        timersReady_=0;
    }
}
bool GEOSKernel::Spawn(GEOSThread* thread,void (*entry)(void*),void* arg,int id,int priority){
    if(!ready_||!thread||!entry)return false;
    memset(thread,0,sizeof(*thread));
    thread->entry=entry;thread->arg=arg;thread->id=id;thread->priority=priority;
#if defined(XBOX) || defined(_XBOX)
    thread->handle=CreateThread(0,GEOS_WORKER_STACK_BYTES,&XboxGameEntry,thread,0,0);
    if(!thread->handle)return false;
#if GEOS_XBOX_AUDIO_PRIORITY_BOOST
    /* Native 007 AUDI thread (4) is higher priority than MAIN (3). Grant
     * just this blocking 30 Hz PCM producer one modest Xbox priority level
     * over the CPU-heavy graphics worker. Do not change GFX/scheduler/VI
     * threads or engine timing. Failure retains the former normal priority. */
    if(id==4 && priority>=20)
        (void)SetThreadPriority(thread->handle,THREAD_PRIORITY_ABOVE_NORMAL);
#endif
#else
    pthread_attr_t attr;pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr,GEOS_WORKER_STACK_BYTES);
    int rc=pthread_create(&thread->handle,&attr,&HostGameEntry,thread);
    pthread_attr_destroy(&attr);
    if(rc!=0)return false;
#endif
    thread->started=1;return true;
}
bool GEOSKernel::Join(GEOSThread* thread,unsigned ms){
    if(!thread||!thread->started)return false;
#if defined(XBOX) || defined(_XBOX)
    DWORD rc=WaitForSingleObject(thread->handle,ms);
    if(rc!=WAIT_OBJECT_0)return false;
    CloseHandle(thread->handle);thread->handle=0;
#else
    (void)ms;
    if(pthread_join(thread->handle,0)!=0)return false;
#endif
    thread->started=0;return true;
}
