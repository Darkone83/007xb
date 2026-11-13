#ifndef XB_GE_OS_H
#define XB_GE_OS_H
/* Set to zero for a synchronous A/B build. The on-device trace records mode.
 * This changes task execution only; the original 60Hz VI and game clocks stay untouched. */
#ifndef GEOS_ASYNC_GFX
#define GEOS_ASYNC_GFX 1
#endif
/* The actual N64 OS services required by GoldenEye's src/sched.c:
 * message queues, VI retraces, SP/DP completion, OS timer clock, and
 * lightweight Xbox threads.  The full game's OSThread/OSMesgQueue ABI is
 * adapted in the forthcoming PR/os.h bridge; this layer has NO fake intro
 * progression and does NOT invent task-complete events on a failed render.
 *
 * Fixed capacities, explicit shutdown, no STL/exceptions; C++98 compatible.
 */
#if defined(XBOX) || defined(_XBOX)
#include <xtl.h>
#else
#include <pthread.h>
#endif

typedef unsigned int GEOSU32;
typedef unsigned long long GEOSU64;
typedef void* GEOSMesg;
enum { GEOS_NOBLOCK=0, GEOS_BLOCK=1 };
enum { GEOS_EVENT_SP=4, GEOS_EVENT_DP=9, GEOS_EVENT_PRENMI=14 };

/* N64 queue ring retains caller-owned message storage, like osCreateMesgQueue.
 * A manual-reset not-empty/not-full event (Xbox) or mutex/condvar (host)
 * makes blocking send/receive wakeup-safe; queues are initialized once. */
struct GEOSQueue {
    GEOSMesg* buffer;
    int capacity,first,count;
    unsigned dropped;
    unsigned ready;
    /* N64 OSMesgQueue fields remain accurate while queue lock is held. */
    volatile int* abiCount;
    volatile int* abiFirst;
#if defined(XBOX) || defined(_XBOX)
    CRITICAL_SECTION lock;
    HANDLE notEmpty,notFull;
#else
    pthread_mutex_t lock;
    pthread_cond_t notEmpty,notFull;
#endif
};

struct GEOSThread {
    void (*entry)(void*);
    void* arg;
    int id, priority;
    volatile int started, exited;
#if defined(XBOX) || defined(_XBOX)
    HANDLE handle;
#else
    pthread_t handle;
#endif
};

typedef bool (*GEOSTaskProc)(void* task,void* user);
/* Read-only snapshot of this native Xbox thread's fixed kernel stack.
 * Returns 1 only for plausible bounds containing the sampled ESP. Host
 * builds return 0; failure clears all supplied outputs. No disk I/O or
 * allocation occurs here. StackPointer is sampled inside this call. */
extern "C" int GEOSGetCurrentStackSnapshot(unsigned* stackBase,
    unsigned* stackLimit,unsigned* stackPointer);
/* The RDP-only task takes the raw command buffer and byte length. It cannot
 * be marked complete unless a real RDP producer accepts that buffer. */
typedef bool (*GEOSDPProc)(void* buffer,GEOSU64 bytes,void* user);
class GEOSKernel {
public:
    GEOSKernel();
    bool Init(unsigned hz);
    bool StartRetrace();
    void Shutdown();
    bool IsReady() const { return ready_!=0; }
    bool IsRunning() const { return running_!=0; }
    void SetVIEvent(GEOSQueue* q,GEOSMesg msg,unsigned count);
    void SetEvent(unsigned eventId,GEOSQueue* q,GEOSMesg msg);
    void SetSPRunner(GEOSTaskProc proc,void* user); /* GFX / fast3d */
    void SetAudioRunner(GEOSTaskProc proc,void* user); /* libaudio mixer */
    void SetDPRunner(GEOSDPProc proc,void* user); /* DP-only tasks */
    /* OSTime units use the original osClockRate = 6,250,000 cycles/sec. */
    int SetTimer(void* tag, GEOSU64 delayCycles,GEOSU64 repeatCycles,
                 GEOSQueue* queue,GEOSMesg msg);
    int StopTimer(void* tag);
    /* Invoke the real GBI/AI producer before reporting hardware completion.
       Return -1 (no engine) instead of posting false completion messages. */
    int RunSPTask(void* task,bool graphics);
    int RunDPTask(void* buffer,GEOSU64 bytes);
    void PostDPComplete();
    GEOSU32 GetCount() const;  /* original game's ~46.5525MHz RCP count */
    GEOSU64 GetTimeUs() const;
    GEOSU32 Retraces() const {return retraces_;}
    GEOSU32 DroppedRetraces() const {return droppedRetraces_;}
    bool GfxWorkerActive() const { return gfxWorkerReady_!=0; }
    bool GfxWorkerFailed() const { return gfxWorkerFailed_!=0; }
    bool Spawn(GEOSThread* thread,void (*entry)(void*),void* arg,int id,int priority);
    bool Join(GEOSThread* thread,unsigned milliseconds);
private:
    GEOSKernel(const GEOSKernel&);
    GEOSKernel& operator=(const GEOSKernel&);
    struct EventSlot {GEOSQueue* q;GEOSMesg msg;};
    EventSlot events_[16];
    struct TimerSlot {void* tag; GEOSQueue* queue; GEOSMesg msg;
                      GEOSU64 dueUs,repeatUs;int active;};
    TimerSlot timers_[16];
#if defined(XBOX) || defined(_XBOX)
    CRITICAL_SECTION timerLock_;
#else
    pthread_mutex_t timerLock_;
#endif
    int timersReady_;
    void ServiceTimers(GEOSU64 now);
    GEOSQueue* viQ_;
    GEOSMesg viMsg_;
    unsigned viCount_,viDiv_;
    GEOSTaskProc spTask_,audioTask_;
    GEOSDPProc dpTask_;
    void *spUser_,*audioUser_,*dpUser_;
    /* Exactly one in-flight original SP/DP graphics task. The queue owns
     * only a borrowed OSTask pointer; scheduler completion preserves its
     * lifetime, and never runs before the GPU task has actually returned. */
    GEOSQueue gfxQueue_;
    GEOSMesg gfxQueueStorage_[1];
    volatile int gfxWorkerReady_,gfxWorkerFailed_,gfxBusy_,gfxStop_;
#if defined(XBOX) || defined(_XBOX)
    HANDLE gfxThread_;
#else
    pthread_t gfxThread_;
#endif
    bool StartGfxWorker();
    void GfxWorkerLoop();
    static void* GfxWorkerEntry(void* state);
#if defined(XBOX) || defined(_XBOX)
    static DWORD WINAPI XboxGfxWorkerEntry(LPVOID state);
#endif
    unsigned hz_;
    volatile int ready_,running_;
    volatile GEOSU32 retraces_,droppedRetraces_;
#if defined(XBOX) || defined(_XBOX)
    HANDLE retraceThread_;
#else
    pthread_t retraceThread_;
#endif
    void TickLoop();
    static void* ThreadEntry(void* state);
#if defined(XBOX) || defined(_XBOX)
    static DWORD WINAPI XboxThreadEntry(LPVOID state);
    static DWORD WINAPI XboxGameEntry(LPVOID state);
#else
    static void* HostGameEntry(void* state);
#endif
};

bool GEOSQueueInit(GEOSQueue* q,GEOSMesg* slots,int capacity);
void GEOSQueueDestroy(GEOSQueue* q);
void GEOSQueueBindMirror(GEOSQueue* q,volatile int* count,volatile int* first);
int GEOSSend(GEOSQueue* q,GEOSMesg msg,int blocking);
int GEOSReceive(GEOSQueue* q,GEOSMesg* msg,int blocking);
int GEOSPending(GEOSQueue* q);
/* Force only the SP/DP task-complete interrupts through an overloaded queue.
 * Drops its oldest (stale VI) message, never blocks the scheduler on itself. */
int GEOSPostCritical(GEOSQueue* q,GEOSMesg msg);

#endif
