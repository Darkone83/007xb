/* Phase 0 sampling only. This file deliberately avoids modifying the
 * native OS clock, input state, DirectSound scheduling or graphics queue. */
#include "ge_phase0_profile.h"
#if GE_XBOX_PHASE0_PROFILE
#include "ge_native_platform.h"
#include "xb_ge_audio.h"
#include <string.h>
#if defined(XBOX) || defined(_XBOX)
#include <xtl.h>
#else
#include <time.h>
typedef long LONG;
#endif

/* 1/16 detailed game, service and GPU timing samples. One frame-start
 * timestamp every processed game frame gives an unbiased interval histogram. */
#define P0_SAMPLE_MASK 15u
#define P0_HIST_BUCKETS 256u
#define P0_MAX_ELAPSED 10000000u
#define P0_SCOPES 3u

struct P0Aggregate {
    unsigned count, maximum;
    unsigned long long total;
};
struct P0Capture {
    unsigned active;
    int stage;
    unsigned frames, intervals, samples;
    unsigned over33, over50, over66;
    unsigned lastFrameValid;
    unsigned long long lastFrameUs, phaseUs;
    unsigned sampled;
    unsigned phase;
    unsigned histogram[P0_HIST_BUCKETS];
    P0Aggregate phases[4]; /* 0 reserved; logic, render, prep + submit in index 0 */
    P0Aggregate scopes[P0_SCOPES];
    unsigned long long scopeStarts[P0_SCOPES];
    unsigned memaSearchCalls,memaSearchPasses;
    unsigned visibleSamples,visibleMax;
    unsigned long long visibleSum;
    unsigned viDropStart;
    LONG gfxCountStart,gfx100usStart;
    LONG padStart[4];
    unsigned hostCountStart,hostLongStart,hostVeryLongStart;
    unsigned hostMaxStart,hostTimeSamplesStart;
    unsigned long long hostTimeSumStart;
    unsigned hostTimeMaxStart;
    unsigned hostInputSamplesStart,hostAudioSamplesStart;
    unsigned long long hostInputSumStart,hostAudioSumStart;
    unsigned audioUnderStart,audioOverStart;
    LONG gfxDetailStartTime[GE_P0B_COUNT];
    LONG gfxDetailStartCalls[GE_P0B_COUNT];
    LONG gfxOver33Start,gfxOver50Start;
#if GE_XBOX_PHASE0E_PROFILE
    LONG gfxExclusiveStartUs[GE_P0E_COUNT];
    LONG gfxExclusiveStartOps[GE_P0E_COUNT];
    LONG gfxExclusiveOverflowStart;
#endif
};
static P0Capture s_p0;
#if GE_XBOX_CINEMATIC_TRACE
/* Main-thread only, fixed-size and allocation-free. A 1/16 Phase0 sample
 * cannot reliably observe a ~59ms hitch, so cinematic phases are measured
 * every game frame. The renderer has a separate serial and is never treated
 * as the same event without an explicit correlation. */
enum { CINE_TOP_MAIN=16, CINE_TOP_CPU=4 };
struct CineFrame {
    unsigned frame,mode,room,visible,gameGapUs,gameTotalUs;
    unsigned phase[4]; /* logic, lvlRender, prep, submit */
    unsigned scope[GE_CINE_SCOPE_COUNT];
    unsigned wait[GE_CINE_WAIT_COUNT]; /* retraces skipped since prior frame */
    unsigned loads,loadBytes;
};
static CineFrame s_cineCurrent,s_cineTop[CINE_TOP_MAIN],s_cineCpuTop[CINE_TOP_CPU];
static unsigned s_cineTopCount=0,s_cineCpuTopCount=0,s_cineFrames=0;
static unsigned s_cineSkipped[GE_CINE_WAIT_COUNT];
static unsigned s_cineSkippedPrior[GE_CINE_WAIT_COUNT];
static unsigned s_cineLoads=0,s_cineLoadBytes=0,s_cineMaxVisible=0;
static unsigned s_cineGameOver28=0,s_cineGameOver50=0,s_cineMaxGameUs=0;
static unsigned long long s_cineFrameStart=0,s_cinePhaseStart=0;
static volatile LONG s_cineActive=0;
static void CineKeep(const CineFrame& f) {
    /* Rank by largest game-loop interval OR recorded CPU work so queue
     * stalls survive even when lvlRender itself was inexpensive. */
    unsigned score=f.gameGapUs>f.gameTotalUs?f.gameGapUs:f.gameTotalUs;
    unsigned slot=s_cineTopCount;
    if(slot<CINE_TOP_MAIN)++s_cineTopCount;
    else {
        slot=0;
        for(unsigned i=1;i<CINE_TOP_MAIN;++i) {
            unsigned x=s_cineTop[i].gameGapUs>s_cineTop[i].gameTotalUs?
                s_cineTop[i].gameGapUs:s_cineTop[i].gameTotalUs;
            unsigned y=s_cineTop[slot].gameGapUs>s_cineTop[slot].gameTotalUs?
                s_cineTop[slot].gameGapUs:s_cineTop[slot].gameTotalUs;
            if(x<y)slot=i;
        }
        unsigned lowest=s_cineTop[slot].gameGapUs>s_cineTop[slot].gameTotalUs?
            s_cineTop[slot].gameGapUs:s_cineTop[slot].gameTotalUs;
        if(score<=lowest)return;
    }
    s_cineTop[slot]=f;
}
/* A large main-thread CPU spike can be hidden by longer frame-start gaps.
 * Keep an independent top-four for those genuinely CPU-expensive frames. */
static void CineKeepCpu(const CineFrame& f) {
    unsigned slot=s_cineCpuTopCount;
    if(slot<CINE_TOP_CPU)++s_cineCpuTopCount;
    else {
        slot=0;
        for(unsigned i=1;i<CINE_TOP_CPU;++i)
            if(s_cineCpuTop[i].gameTotalUs<s_cineCpuTop[slot].gameTotalUs)slot=i;
        if(f.gameTotalUs<=s_cineCpuTop[slot].gameTotalUs)return;
    }
    s_cineCpuTop[slot]=f;
}
#endif

/* The graphics worker is a separate thread. Keep its count and sum atomic,
 * in 100-us units so a 32-bit accumulator lasts many hours. Max is advisory. */
static volatile LONG s_stageActive=0;
static volatile LONG s_gfxCount=0;
static volatile LONG s_gfx100us=0;
static volatile LONG s_gfxMax100us=0;
static unsigned s_gfxTaskOrdinal=0; /* worker-owned; synchronous fallback only otherwise */
/* Only the gfx worker (or synchronous substitute) owns these task-local
 * counters. At task completion we publish one batch of atomic aggregates.
 * This avoids interlocked operations and disk I/O inside hot draw loops. */
static unsigned s_p0bSampleActive=0;
static unsigned s_p0bTaskUs[GE_P0B_COUNT],s_p0bTaskCalls[GE_P0B_COUNT];
static volatile LONG s_p0bTotalUs[GE_P0B_COUNT];
static volatile LONG s_p0bTotalCalls[GE_P0B_COUNT];
static volatile LONG s_p0bOver33=0,s_p0bOver50=0;
#if GE_XBOX_PHASE0E_PROFILE
/* Single graphics-worker ownership, never allocations or logs in hot loops.
 * RDTSC avoids two QPC/divisions for every Fast3D opcode. Calibrate the
 * ratio from the sampled graphics task's existing P0 wall-clock span. */
enum { P0E_STACK_MAX=16 };
struct P0EScopeFrame {
    unsigned category;
    unsigned long long beginTick,childTicks;
};
static P0EScopeFrame s_p0eStack[P0E_STACK_MAX];
static unsigned s_p0eDepth=0,s_p0eActive=0,s_p0eOverflows=0;
static unsigned long long s_p0eBeginTick=0;
static unsigned long long s_p0eTaskTicks[GE_P0E_COUNT];
static unsigned s_p0eTaskOps[GE_P0E_COUNT];
static volatile LONG s_p0eTotalUs[GE_P0E_COUNT];
static volatile LONG s_p0eTotalOps[GE_P0E_COUNT];
static volatile LONG s_p0eTotalOverflows=0;
#endif
static volatile LONG s_pad[4]={0,0,0,0};
static unsigned s_hostCalls=0,s_hostGapOver8=0,s_hostGapOver20=0;
static volatile LONG s_retraceDrops=0;
static unsigned s_hostMaxGapMs=0,s_hostLastMs=0,s_hostLastValid=0;
static unsigned s_hostSamples=0,s_hostMaxCostUs=0;
static unsigned long long s_hostCostSumUs=0,s_hostSampleStartUs=0;
static unsigned long long s_hostInputMarkUs=0,s_hostInputSumUs=0,s_hostAudioSumUs=0;
static unsigned s_hostInputSamples=0,s_hostAudioSamples=0;

static unsigned long long P0NowUs(void) {
#if defined(GE_P0_TEST_CLOCK)
    extern unsigned long long geP0TestClockUs(void);
    return geP0TestClockUs();
#elif defined(XBOX) || defined(_XBOX)
    static unsigned long long hz=0;
    LARGE_INTEGER freq,now;
    if(!hz) {
        if(!QueryPerformanceFrequency(&freq) || freq.QuadPart<=0)return 0;
        hz=(unsigned long long)freq.QuadPart;
    }
    if(!QueryPerformanceCounter(&now))return 0;
    return ((unsigned long long)now.QuadPart/hz)*1000000ull+
        (((unsigned long long)now.QuadPart%hz)*1000000ull)/hz;
#else
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts)!=0)return 0;
    return (unsigned long long)ts.tv_sec*1000000ull+
        (unsigned long long)ts.tv_nsec/1000ull;
#endif
}

#if GE_XBOX_PHASE0E_PROFILE
static unsigned long long P0ETicks(void) {
#if defined(_MSC_VER) && defined(_M_IX86)
    unsigned lo=0,hi=0;
    __asm {
        rdtsc
        mov lo,eax
        mov hi,edx
    }
    return ((unsigned long long)hi<<32) | (unsigned long long)lo;
#elif defined(__i386__) || defined(__x86_64__)
    unsigned lo,hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((unsigned long long)hi<<32) | (unsigned long long)lo;
#else
    /* Non-x86 host test only; Xbox RXDK uses the native TSC fast path. */
    return P0NowUs();
#endif
}
#endif
static LONG P0Load(volatile LONG* p) {
#if defined(XBOX) || defined(_XBOX)
    return InterlockedCompareExchange((LPLONG)p,0,0);
#else
    return __sync_fetch_and_add(p,0);
#endif
}
static void P0Store(volatile LONG* p,LONG value) {
#if defined(XBOX) || defined(_XBOX)
    InterlockedExchange((LPLONG)p,value);
#else
    __sync_lock_test_and_set(p,value);
#endif
}
static void P0Add(volatile LONG* p,LONG value) {
#if defined(XBOX) || defined(_XBOX)
    LONG old=P0Load(p);
    for(;;){
        LONG observed=InterlockedCompareExchange((LPLONG)p,old+value,old);
        if(observed==old)break;
        old=observed;
    }
#else
    __sync_fetch_and_add(p,value);
#endif
}
static void P0Max(volatile LONG* p,LONG value) {
    LONG old=P0Load(p);
    while(value>old) {
#if defined(XBOX) || defined(_XBOX)
        LONG seen=InterlockedCompareExchange((LPLONG)p,value,old);
#else
        LONG seen=__sync_val_compare_and_swap(p,old,value);
#endif
        if(seen==old)break;
        old=seen;
    }
}
static unsigned P0Delta(unsigned long long start,unsigned long long end) {
    if(!start || end<start)return 0;
    unsigned long long delta=end-start;
    return delta>P0_MAX_ELAPSED?0:(unsigned)delta;
}
static void P0Record(P0Aggregate* dest,unsigned value) {
    if(!value)return;
    ++dest->count;
    dest->total+=value;
    if(value>dest->maximum)dest->maximum=value;
}
static unsigned P0Avg(const P0Aggregate* a) {
    return a->count?(unsigned)(a->total/a->count):0;
}
static unsigned P0Percentile(unsigned pct) {
    if(!s_p0.intervals)return 0;
    unsigned target=(s_p0.intervals*pct+99u)/100u,acc=0;
    for(unsigned i=0;i<P0_HIST_BUCKETS;++i) {
        acc+=s_p0.histogram[i];
        if(acc>=target)return i;
    }
    return P0_HIST_BUCKETS-1;
}
static void P0InitStage(int stage) {
    memset(&s_p0,0,sizeof(s_p0));
    s_p0.stage=stage;s_p0.active=1;
#if GE_XBOX_CINEMATIC_TRACE
    memset(&s_cineCurrent,0,sizeof(s_cineCurrent));
    memset(s_cineTop,0,sizeof(s_cineTop));
    memset(s_cineCpuTop,0,sizeof(s_cineCpuTop));
    memset(s_cineSkipped,0,sizeof(s_cineSkipped));
    memset(s_cineSkippedPrior,0,sizeof(s_cineSkippedPrior));
    s_cineTopCount=s_cineCpuTopCount=s_cineFrames=s_cineLoads=s_cineLoadBytes=0;
    s_cineMaxVisible=s_cineGameOver28=s_cineGameOver50=s_cineMaxGameUs=0;
    s_cineFrameStart=s_cinePhaseStart=0;
    P0Store(&s_cineActive,0);
#endif
    s_p0.gfxCountStart=P0Load(&s_gfxCount);
    s_p0.gfx100usStart=P0Load(&s_gfx100us);
    for(unsigned i=0;i<GE_P0B_COUNT;++i){
        s_p0.gfxDetailStartTime[i]=P0Load(&s_p0bTotalUs[i]);
        s_p0.gfxDetailStartCalls[i]=P0Load(&s_p0bTotalCalls[i]);
    }
    s_p0.gfxOver33Start=P0Load(&s_p0bOver33);
#if GE_XBOX_PHASE0E_PROFILE
    for(unsigned i=0;i<GE_P0E_COUNT;++i){
        s_p0.gfxExclusiveStartUs[i]=P0Load(&s_p0eTotalUs[i]);
        s_p0.gfxExclusiveStartOps[i]=P0Load(&s_p0eTotalOps[i]);
    }
    s_p0.gfxExclusiveOverflowStart=P0Load(&s_p0eTotalOverflows);
#endif
    s_p0.gfxOver50Start=P0Load(&s_p0bOver50);
    for(unsigned i=0;i<4;++i)s_p0.padStart[i]=P0Load(&s_pad[i]);
    s_p0.hostCountStart=s_hostCalls;
    s_p0.hostLongStart=s_hostGapOver8;
    s_p0.hostVeryLongStart=s_hostGapOver20;
    s_p0.hostMaxStart=s_hostMaxGapMs;
    s_p0.hostTimeSamplesStart=s_hostSamples;
    s_p0.hostTimeSumStart=s_hostCostSumUs;
    s_p0.hostTimeMaxStart=s_hostMaxCostUs;
    s_p0.hostInputSamplesStart=s_hostInputSamples;
    s_p0.hostAudioSamplesStart=s_hostAudioSamples;
    s_p0.hostInputSumStart=s_hostInputSumUs;
    s_p0.hostAudioSumStart=s_hostAudioSumUs;
    s_p0.viDropStart=(unsigned)P0Load(&s_retraceDrops);
    P0Store(&s_gfxMax100us,0);
    s_hostMaxGapMs=0;
    s_p0.audioUnderStart=GEAudio_Underruns();
    s_p0.audioOverStart=GEAudio_Overruns();
    P0Store(&s_stageActive,1);
}

extern "C" void geP0FrameBegin(int stage) {
    if(!s_p0.active || s_p0.stage!=stage)P0InitStage(stage);
    unsigned long long t=P0NowUs();
#if GE_XBOX_CINEMATIC_TRACE
    memset(&s_cineCurrent,0,sizeof(s_cineCurrent));
    s_cineCurrent.frame=s_p0.frames+1u;
    for(unsigned i=0;i<GE_CINE_WAIT_COUNT;++i) {
        s_cineCurrent.wait[i]=s_cineSkipped[i]-s_cineSkippedPrior[i];
        s_cineSkippedPrior[i]=s_cineSkipped[i];
    }
    s_cineFrameStart=s_cinePhaseStart=t;
    /* Keep the previous camera flag until the new camera is sampled in
     * bgRoomVisibilityRelated. skyRender runs before that camera query. */
    if(s_p0.lastFrameValid)
        s_cineCurrent.gameGapUs=P0Delta(s_p0.lastFrameUs,t);
#endif
    if(s_p0.lastFrameValid && t>=s_p0.lastFrameUs) {
        unsigned us=P0Delta(s_p0.lastFrameUs,t);
        if(us) {
            unsigned bucket=us/1000u;
            if(bucket>=P0_HIST_BUCKETS)bucket=P0_HIST_BUCKETS-1u;
            ++s_p0.histogram[bucket];++s_p0.intervals;
            if(us>33000u)++s_p0.over33;
            if(us>50000u)++s_p0.over50;
            if(us>66000u)++s_p0.over66;
        }
    }
    s_p0.lastFrameUs=t;s_p0.lastFrameValid=t!=0;
    s_p0.sampled=((s_p0.frames&P0_SAMPLE_MASK)==0u);
    ++s_p0.frames;
    s_p0.phase=0;
    s_p0.phaseUs=s_p0.sampled?t:0;
    if(s_p0.sampled)++s_p0.samples;
}
extern "C" void geP0PhaseMark(int phase) {
#if GE_XBOX_CINEMATIC_TRACE
    if(phase<1 || phase>3)return;
    const unsigned long long now=P0NowUs();
    if(s_cineFrameStart) {
        s_cineCurrent.phase[phase]=P0Delta(s_cinePhaseStart,now);
        s_cinePhaseStart=now;
    }
    if(!s_p0.sampled || !s_p0.phaseUs)return;
#else
    if(!s_p0.sampled || !s_p0.phaseUs || phase<1 || phase>3)return;
    const unsigned long long now=P0NowUs();
#endif
    const unsigned delta=P0Delta(s_p0.phaseUs,now);
    /* Phase 1 = logic, Phase 2 = lvlRender, Phase 3 = prep. */
    P0Record(&s_p0.phases[phase],delta);
    s_p0.phase=phase;s_p0.phaseUs=now;
}
extern "C" void geP0FrameEnd(void) {
#if GE_XBOX_CINEMATIC_TRACE
    const unsigned long long now=P0NowUs();
    if(s_cineFrameStart) {
        s_cineCurrent.phase[0]=P0Delta(s_cinePhaseStart,now);
        s_cineCurrent.gameTotalUs=P0Delta(s_cineFrameStart,now);
        if(P0Load(&s_cineActive)) {
            ++s_cineFrames;
            if(s_cineCurrent.gameGapUs>28000u)++s_cineGameOver28;
            if(s_cineCurrent.gameGapUs>50000u)++s_cineGameOver50;
            if(s_cineCurrent.gameTotalUs>s_cineMaxGameUs)
                s_cineMaxGameUs=s_cineCurrent.gameTotalUs;
            s_cineLoads+=s_cineCurrent.loads;
            s_cineLoadBytes+=s_cineCurrent.loadBytes;
            if(s_cineCurrent.visible>s_cineMaxVisible)
                s_cineMaxVisible=s_cineCurrent.visible;
            CineKeep(s_cineCurrent);
            CineKeepCpu(s_cineCurrent);
        }
    }
    s_cineFrameStart=s_cinePhaseStart=0;
#else
    const unsigned long long now=P0NowUs();
#endif
    if(s_p0.sampled && s_p0.phase==3)
        P0Record(&s_p0.phases[0],P0Delta(s_p0.phaseUs,now));
    s_p0.sampled=0;s_p0.phaseUs=0;
}
#if GE_XBOX_CINEMATIC_TRACE
extern "C" void geP0CineCamera(int cameraMode,int room,int enabled) {
    if(!s_cineFrameStart)return;
    s_cineCurrent.mode=(unsigned)cameraMode;
    s_cineCurrent.room=(unsigned)room;
    P0Store(&s_cineActive,enabled?1:0);
}
extern "C" void geP0CineVisible(unsigned count) {
    if(s_cineFrameStart && P0Load(&s_cineActive))s_cineCurrent.visible=count;
}
extern "C" unsigned long long geP0CineScopeBegin(int category) {
    if(!s_cineFrameStart || !P0Load(&s_cineActive) || category<0 ||
       category>=GE_CINE_SCOPE_COUNT)return 0;
    return P0NowUs();
}
extern "C" void geP0CineScopeEnd(int category,unsigned long long start) {
    if(!start || !s_cineFrameStart || !P0Load(&s_cineActive) || category<0 ||
       category>=GE_CINE_SCOPE_COUNT)return;
    s_cineCurrent.scope[category]+=P0Delta(start,P0NowUs());
}
extern "C" void geP0CineRoomEvent(unsigned loaded,unsigned bytes) {
    if(s_cineFrameStart && P0Load(&s_cineActive) && loaded) {
        ++s_cineCurrent.loads;
        s_cineCurrent.loadBytes+=bytes;
    }
}
extern "C" void geP0CineSkip(int reason) {
    if(reason>=0 && reason<GE_CINE_WAIT_COUNT && P0Load(&s_cineActive))
        ++s_cineSkipped[(unsigned)reason];
}
extern "C" int geP0CineIsActive(void) { return P0Load(&s_cineActive)!=0; }
#endif
extern "C" void geP0ScopeStart(int which) {
    if(which>=0 && which<(int)P0_SCOPES && s_p0.sampled)
        s_p0.scopeStarts[which]=P0NowUs();
}
extern "C" void geP0ScopeEnd(int which) {
    if(which<0 || which>=(int)P0_SCOPES || !s_p0.sampled)return;
    unsigned long long start=s_p0.scopeStarts[which];
    s_p0.scopeStarts[which]=0;
    if(start)P0Record(&s_p0.scopes[which],P0Delta(start,P0NowUs()));
}
extern "C" void geP0MemaSearchEntry(void) {
    if(s_p0.sampled)++s_p0.memaSearchCalls;
}
extern "C" void geP0MemaSearchPass(void) {
    if(s_p0.sampled)++s_p0.memaSearchPasses;
}
extern "C" void geP0RetraceDrops(unsigned count) {
    P0Store(&s_retraceDrops,(LONG)count);
}
extern "C" void geP0VisibleProps(unsigned count) {
    if(s_p0.sampled){
        ++s_p0.visibleSamples;
        s_p0.visibleSum+=count;
        if(count>s_p0.visibleMax)s_p0.visibleMax=count;
    }
}
extern "C" void geP0PadEvent(int what) {
    if(what>=0&&what<4)P0Add(&s_pad[what],1);
}
extern "C" void geP0HostBegin(void) {
#if defined(XBOX) || defined(_XBOX)
    unsigned now=GetTickCount();
#else
    unsigned now=(unsigned)(P0NowUs()/1000ull);
#endif
    if(s_hostLastValid){
        unsigned gap=now-s_hostLastMs;
        if(gap>s_hostMaxGapMs)s_hostMaxGapMs=gap;
        if(gap>8u)++s_hostGapOver8;
        if(gap>20u)++s_hostGapOver20;
    }
    s_hostLastMs=now;s_hostLastValid=1;
    s_hostSampleStartUs=((s_hostCalls&P0_SAMPLE_MASK)==0)?P0NowUs():0;
    s_hostInputMarkUs=0;
    ++s_hostCalls;
}
extern "C" void geP0HostInputDone(void) {
    if(s_hostSampleStartUs){
        s_hostInputMarkUs=P0NowUs();
        unsigned elapsed=P0Delta(s_hostSampleStartUs,s_hostInputMarkUs);
        if(elapsed){++s_hostInputSamples;s_hostInputSumUs+=elapsed;}
    }
}
extern "C" void geP0HostEnd(void) {
    if(s_hostSampleStartUs){
        unsigned long long end=P0NowUs();
        unsigned elapsed=P0Delta(s_hostSampleStartUs,end);
        if(s_hostInputMarkUs){
            unsigned audioElapsed=P0Delta(s_hostInputMarkUs,end);
            if(audioElapsed){++s_hostAudioSamples;s_hostAudioSumUs+=audioElapsed;}
        }
        s_hostInputMarkUs=s_hostSampleStartUs=0;
        if(elapsed){
            ++s_hostSamples;s_hostCostSumUs+=elapsed;
            if(elapsed>s_hostMaxCostUs)s_hostMaxCostUs=elapsed;
        }
    }
}
/* PH0B: start/end of a nested Fast3D scope. A single timer query is
 * required only on a selected graphics task. Others are effectively no-ops. */
extern "C" unsigned long long geP0BStart(void) {
    return s_p0bSampleActive ? P0NowUs() : 0;
}
extern "C" void geP0BRecord(int category,unsigned microseconds) {
    if(!s_p0bSampleActive || category<0 || category>=GE_P0B_COUNT ||
       !microseconds || microseconds>P0_MAX_ELAPSED)return;
    const unsigned u=(unsigned)category;
    if(s_p0bTaskUs[u]<=0xFFFFFFFFu-microseconds)
        s_p0bTaskUs[u]+=microseconds;
    ++s_p0bTaskCalls[u];
}
extern "C" void geP0BEnd(int category,unsigned long long start) {
    if(start && s_p0bSampleActive)
        geP0BRecord(category,P0Delta(start,P0NowUs()));
}

#if GE_XBOX_PHASE0E_PROFILE
/* Child-inclusive elapsed ticks are charged to the parent as children,
 * while the child's own EXCLUSIVE ticks go to its category. DISPATCH at
 * depth zero is the only allowed root, so work outside PH0B DL is excluded. */
extern "C" int geP0EEnter(int category) {
    if(!s_p0eActive || category<0 || category>=GE_P0E_COUNT ||
       (!s_p0eDepth && category!=GE_P0E_DISPATCH))return 0;
    if(s_p0eDepth>=P0E_STACK_MAX){++s_p0eOverflows;return 0;}
    P0EScopeFrame& f=s_p0eStack[s_p0eDepth++];
    f.category=(unsigned)category;
    f.childTicks=0;
    f.beginTick=P0ETicks();
    return 1;
}
extern "C" void geP0ELeave(void) {
    if(!s_p0eActive || !s_p0eDepth)return;
    const unsigned long long now=P0ETicks();
    const P0EScopeFrame f=s_p0eStack[--s_p0eDepth];
    if(now<f.beginTick)return;
    const unsigned long long span=now-f.beginTick;
    const unsigned long long self=span>=f.childTicks?span-f.childTicks:0;
    s_p0eTaskTicks[f.category]+=self;
    if(s_p0eDepth)s_p0eStack[s_p0eDepth-1u].childTicks+=span;
}
extern "C" void geP0ECommandNote(int category) {
    if(s_p0eActive && s_p0eDepth && category>=0 && category<GE_P0E_COUNT)
        ++s_p0eTaskOps[(unsigned)category];
}
#endif
extern "C" unsigned long long geP0GraphicsBegin(void) {
    /* Called by one graphics worker or by synchronous original-scheduler
     * fallback; a task is never executed concurrently on both paths. */
    s_p0bSampleActive=0;
#if GE_XBOX_PHASE0E_PROFILE
    s_p0eActive=0;
#endif
    if(!P0Load(&s_stageActive) || ((s_gfxTaskOrdinal++ & P0_SAMPLE_MASK)!=0u))return 0;
    const unsigned long long now=P0NowUs();
    if(!now)return 0;
    memset(s_p0bTaskUs,0,sizeof(s_p0bTaskUs));
    memset(s_p0bTaskCalls,0,sizeof(s_p0bTaskCalls));
    s_p0bSampleActive=1;
#if GE_XBOX_PHASE0E_PROFILE
    memset(s_p0eTaskTicks,0,sizeof(s_p0eTaskTicks));
    memset(s_p0eTaskOps,0,sizeof(s_p0eTaskOps));
    s_p0eDepth=0;s_p0eOverflows=0;
    s_p0eBeginTick=P0ETicks();
    s_p0eActive=1;
#endif
    return now;
}
extern "C" void geP0GraphicsEnd(unsigned long long start) {
    if(!start){s_p0bSampleActive=0;return;}
#if GE_XBOX_PHASE0E_PROFILE
    const unsigned long long p0eEndTick=s_p0eActive?P0ETicks():0;
    s_p0eActive=0;
#endif
    const unsigned delta=P0Delta(start,P0NowUs());
    /* End detailed collection before publishing counters. */
    s_p0bSampleActive=0;
#if GE_XBOX_PHASE0E_PROFILE
    const unsigned long long taskTicks=p0eEndTick>=s_p0eBeginTick?
        p0eEndTick-s_p0eBeginTick:0;
    if(delta && taskTicks && !s_p0eDepth){
        for(unsigned i=0;i<GE_P0E_COUNT;++i){
            /* Product is <2^64 under the P0 10-second sample cap. */
            unsigned us=(unsigned)((s_p0eTaskTicks[i]*(unsigned long long)delta)/taskTicks);
            if(us)P0Add(&s_p0eTotalUs[i],(LONG)us);
            if(s_p0eTaskOps[i])P0Add(&s_p0eTotalOps[i],(LONG)s_p0eTaskOps[i]);
        }
    }
    if(s_p0eOverflows || s_p0eDepth)
        P0Add(&s_p0eTotalOverflows,(LONG)(s_p0eOverflows+(s_p0eDepth?1u:0u)));
#endif
    for(unsigned i=0;i<GE_P0B_COUNT;++i){
        if(s_p0bTaskCalls[i]){
            P0Add(&s_p0bTotalUs[i],(LONG)s_p0bTaskUs[i]);
            P0Add(&s_p0bTotalCalls[i],(LONG)s_p0bTaskCalls[i]);
        }
    }
    if(!delta)return;
    if(delta>33000u)P0Add(&s_p0bOver33,1);
    if(delta>50000u)P0Add(&s_p0bOver50,1);
    LONG units=(LONG)(delta/100u);
    if(units<1)units=1;
    P0Add(&s_gfxCount,1);
    P0Add(&s_gfx100us,units);
    P0Max(&s_gfxMax100us,units);
}
#if GE_XBOX_PHASE0E_PROFILE
static unsigned P0ETotal(int category) {
    const LONG n=P0Load(&s_p0eTotalUs[category])-s_p0.gfxExclusiveStartUs[category];
    return n>0?(unsigned)n:0u;
}
static unsigned P0EAvg(int category,unsigned tasks) {
    return tasks?P0ETotal(category)/tasks:0u;
}
static unsigned P0EOps(int category) {
    const LONG n=P0Load(&s_p0eTotalOps[category])-s_p0.gfxExclusiveStartOps[category];
    return n>0?(unsigned)n:0u;
}
#endif
static unsigned P0BGfxTotal(int category) {
    const LONG n=P0Load(&s_p0bTotalUs[category])-s_p0.gfxDetailStartTime[category];
    return n>0?(unsigned)n:0u;
}
static unsigned P0BGfxCalls(int category) {
    const LONG n=P0Load(&s_p0bTotalCalls[category])-s_p0.gfxDetailStartCalls[category];
    return n>0?(unsigned)n:0u;
}
static unsigned P0BGfxAvg(int category,unsigned tasks) {
    return tasks?P0BGfxTotal(category)/tasks:0u;
}
extern "C" void geP0StageExit(int stage) {
    if(!s_p0.active || s_p0.stage!=stage)return;
#if GE_XBOX_CINEMATIC_TRACE
    /* Stage boundary, not a frame. Game-thread frame serials are not assumed
     * to match graphics-worker serials (at most two frames can be pending). */
    if(s_cineFrames) {
        GENativePlatform_TraceIssue(
            "CINE MAIN stage=%d frames=%u over28=%u over50=%u max_game_cpu_us=%u roomloads=%u roomload_kib=%u max_visible=%u top=%u\n",
            stage,s_cineFrames,s_cineGameOver28,s_cineGameOver50,
            s_cineMaxGameUs,s_cineLoads,s_cineLoadBytes/1024u,s_cineMaxVisible,s_cineTopCount);
        for(unsigned i=0;i<s_cineTopCount;++i) {
            const CineFrame& f=s_cineTop[i];
            GENativePlatform_TraceIssue(
                "CINE MAINFRAME stage=%d gameframe=%u mode=%u camera_room=%u visible=%u gap_since_prior_begin_us=%u game_cpu_us=%u logic_us=%u scene_us=%u prep_us=%u submit_us=%u vis_us=%u props_us=%u bg_us=%u sky_us=%u player_us=%u roomload_us=%u loaded_rooms=%u load_kib=%u wait_tick=%u wait_gfx=%u wait_demo=%u\n",
                stage,f.frame,f.mode,f.room,f.visible,f.gameGapUs,f.gameTotalUs,
                f.phase[GE_P0_LOGIC],f.phase[GE_P0_RENDER],f.phase[GE_P0_PREP],f.phase[0],
                f.scope[GE_CINE_VIS],f.scope[GE_CINE_PROPS],f.scope[GE_CINE_BG],
                f.scope[GE_CINE_SKY],f.scope[GE_CINE_PLAYER],f.scope[GE_CINE_ROOMLOAD],
                f.loads,f.loadBytes/1024u,
                f.wait[GE_CINE_WAIT_TICK],f.wait[GE_CINE_WAIT_GFX],f.wait[GE_CINE_WAIT_DEMO]);
        }
        GENativePlatform_TraceIssue(
            "CINE SCHED stage=%d skipped_tick=%u skipped_gpu_full=%u skipped_demo_deadline=%u\n",
            stage,s_cineSkipped[GE_CINE_WAIT_TICK],s_cineSkipped[GE_CINE_WAIT_GFX],
            s_cineSkipped[GE_CINE_WAIT_DEMO]);
        for(unsigned i=0;i<s_cineCpuTopCount;++i) {
            const CineFrame& f=s_cineCpuTop[i];
            GENativePlatform_TraceIssue(
                "CINE MAINCPU stage=%d gameframe=%u mode=%u gap_us=%u game_cpu_us=%u logic_us=%u scene_us=%u vis_us=%u props_us=%u bg_us=%u roomload_us=%u loads=%u wait_tick=%u wait_gfx=%u wait_demo=%u\n",
                stage,f.frame,f.mode,f.gameGapUs,f.gameTotalUs,
                f.phase[GE_P0_LOGIC],f.phase[GE_P0_RENDER],
                f.scope[GE_CINE_VIS],f.scope[GE_CINE_PROPS],f.scope[GE_CINE_BG],
                f.scope[GE_CINE_ROOMLOAD],f.loads,
                f.wait[GE_CINE_WAIT_TICK],f.wait[GE_CINE_WAIT_GFX],f.wait[GE_CINE_WAIT_DEMO]);
        }
    }
    P0Store(&s_cineActive,0);
#endif
    s_p0.active=0;P0Store(&s_stageActive,0);
    if(s_p0.frames<32u)return; /* No stage transition chatter. */
    const LONG gfxN=P0Load(&s_gfxCount)-s_p0.gfxCountStart;
    const LONG gfx100=P0Load(&s_gfx100us)-s_p0.gfx100usStart;
    const unsigned sampleGfxCount=gfxN>0?(unsigned)gfxN:0u;
    const unsigned averageGfxUs=sampleGfxCount?(unsigned)((unsigned long long)(unsigned)gfx100*100ull/sampleGfxCount):0u;
    unsigned serviceSamples=s_hostSamples-s_p0.hostTimeSamplesStart;
    unsigned serviceCost=serviceSamples?
        (unsigned)((s_hostCostSumUs-s_p0.hostTimeSumStart)/serviceSamples):0u;
    unsigned hostMaxGap=s_hostMaxGapMs;
    unsigned inputSamples=s_hostInputSamples-s_p0.hostInputSamplesStart;
    unsigned audioSamples=s_hostAudioSamples-s_p0.hostAudioSamplesStart;
    unsigned inputAvg=inputSamples?(unsigned)((s_hostInputSumUs-s_p0.hostInputSumStart)/inputSamples):0u;
    unsigned audioAvg=audioSamples?(unsigned)((s_hostAudioSumUs-s_p0.hostAudioSumStart)/audioSamples):0u;
    /* Summary is intentionally 3 records, <1KB total excluding trace prefix. */
    GENativePlatform_TraceIssue(
        "PH0 FRAME stage=%d frames=%u intervals=%u p50_ms=%u p95_ms=%u p99_ms=%u over33=%u over50=%u over66=%u sample=1/16\n",
        stage,s_p0.frames,s_p0.intervals,P0Percentile(50),P0Percentile(95),
        P0Percentile(99),s_p0.over33,s_p0.over50,s_p0.over66);
    GENativePlatform_TraceIssue(
        "PH0 WORK stage=%d samples=%u logic_avg_us=%u render_avg_us=%u prep_avg_us=%u submit_avg_us=%u roomdefrag_avg_us=%u memadefrag_avg_us=%u memasearch_calls=%u memasearch_passes=%u visible_avg=%u visible_max=%u gfx_samples=%u gfx_avg_us=%u gfx_max_us=%u\n",
        stage,s_p0.samples,P0Avg(&s_p0.phases[1]),P0Avg(&s_p0.phases[2]),
        P0Avg(&s_p0.phases[3]),P0Avg(&s_p0.phases[0]),
        P0Avg(&s_p0.scopes[GE_P0_ROOM_DEFRAG]),
        P0Avg(&s_p0.scopes[GE_P0_MEMA_DEFRAG]),s_p0.memaSearchCalls,
        s_p0.memaSearchPasses,
        s_p0.visibleSamples?(unsigned)(s_p0.visibleSum/s_p0.visibleSamples):0u,
        s_p0.visibleMax,sampleGfxCount,averageGfxUs,
        (unsigned)(P0Load(&s_gfxMax100us)*100u));
    GENativePlatform_TraceIssue(
        "PH0 SERVICE stage=%d polls=%u gap_max_ms=%u gap_gt8=%u gap_gt20=%u service_samples=%u service_avg_us=%u input_avg_us=%u audio_avg_us=%u vi_drops=%u audio_underrun=%u audio_overrun=%u pad_removed=%u pad_open_ok=%u pad_open_fail=%u pad_read_fail=%u\n",
        stage,s_hostCalls-s_p0.hostCountStart,hostMaxGap,
        s_hostGapOver8-s_p0.hostLongStart,s_hostGapOver20-s_p0.hostVeryLongStart,
        serviceSamples,serviceCost,inputAvg,audioAvg,
        (unsigned)P0Load(&s_retraceDrops)-s_p0.viDropStart,
        GEAudio_Underruns()-s_p0.audioUnderStart,
        GEAudio_Overruns()-s_p0.audioOverStart,
        (unsigned)(P0Load(&s_pad[GE_P0_PAD_REMOVE])-s_p0.padStart[GE_P0_PAD_REMOVE]),
        (unsigned)(P0Load(&s_pad[GE_P0_PAD_OPEN_OK])-s_p0.padStart[GE_P0_PAD_OPEN_OK]),
        (unsigned)(P0Load(&s_pad[GE_P0_PAD_OPEN_FAILED])-s_p0.padStart[GE_P0_PAD_OPEN_FAILED]),
        (unsigned)(P0Load(&s_pad[GE_P0_PAD_READ_FAILED])-s_p0.padStart[GE_P0_PAD_READ_FAILED]));
    /* PH0B sums are per *sampled graphics task*. DL includes native draw
     * and submit, draw can include a flush; do not add these as exclusive
     * CPU slices. All totals are bounded; no per-frame or per-draw logs. */
    if(sampleGfxCount){
        GENativePlatform_TraceIssue(
            "PH0B GFX stage=%d samples=%u task_over33=%u task_over50=%u dl_avg_us=%u draw_avg_us=%u upload_avg_us=%u present_avg_us=%u\n",
            stage,sampleGfxCount,
            (unsigned)(P0Load(&s_p0bOver33)-s_p0.gfxOver33Start),
            (unsigned)(P0Load(&s_p0bOver50)-s_p0.gfxOver50Start),
            P0BGfxAvg(GE_P0B_DL,sampleGfxCount),
            P0BGfxAvg(GE_P0B_DRAW,sampleGfxCount),
            P0BGfxAvg(GE_P0B_UPLOAD,sampleGfxCount),
            P0BGfxAvg(GE_P0B_PRESENT,sampleGfxCount));
        GENativePlatform_TraceIssue(
            "PH0B D3D stage=%d samples=%u flush_avg_us=%u vb_lock_avg_us=%u tex_lock_avg_us=%u up_draw_avg_us=%u vb_draw_avg_us=%u shader_create_avg_us=%u flush_calls=%u vb_locks=%u up_draws=%u vb_draws=%u tex_locks=%u shaders=%u\n",
            stage,sampleGfxCount,
            P0BGfxAvg(GE_P0B_FLUSH,sampleGfxCount),
            P0BGfxAvg(GE_P0B_VB_LOCK,sampleGfxCount),
            P0BGfxAvg(GE_P0B_TEX_LOCK,sampleGfxCount),
            P0BGfxAvg(GE_P0B_DRAW_UP,sampleGfxCount),
            P0BGfxAvg(GE_P0B_DRAW_VB,sampleGfxCount),
            P0BGfxAvg(GE_P0B_SHADER_CREATE,sampleGfxCount),
            P0BGfxCalls(GE_P0B_FLUSH),
            P0BGfxCalls(GE_P0B_VB_LOCK),
            P0BGfxCalls(GE_P0B_DRAW_UP),
            P0BGfxCalls(GE_P0B_DRAW_VB),
            P0BGfxCalls(GE_P0B_TEX_LOCK),
            P0BGfxCalls(GE_P0B_SHADER_CREATE));
#if GE_XBOX_PHASE0E_PROFILE
        unsigned p0eCaptured=0;
        for(unsigned i=0;i<GE_P0E_COUNT;++i)p0eCaptured+=P0ETotal((int)i);
        GENativePlatform_TraceIssue(
            "PH0E SPLIT stage=%d samples=%u covered_avg_us=%u dispatch_avg_us=%u vertex_avg_us=%u triangle_avg_us=%u matrix_avg_us=%u state_avg_us=%u texstate_avg_us=%u rect_avg_us=%u control_avg_us=%u material_avg_us=%u import_avg_us=%u clip_avg_us=%u pack_avg_us=%u submit_avg_us=%u\n",
            stage,sampleGfxCount,p0eCaptured/sampleGfxCount,
            P0EAvg(GE_P0E_DISPATCH,sampleGfxCount),
            P0EAvg(GE_P0E_VERTEX,sampleGfxCount),
            P0EAvg(GE_P0E_TRIANGLE,sampleGfxCount),
            P0EAvg(GE_P0E_MATRIX,sampleGfxCount),
            P0EAvg(GE_P0E_STATE,sampleGfxCount),
            P0EAvg(GE_P0E_TEXTURE_STATE,sampleGfxCount),
            P0EAvg(GE_P0E_RECT,sampleGfxCount),
            P0EAvg(GE_P0E_CONTROL,sampleGfxCount),
            P0EAvg(GE_P0E_MATERIAL,sampleGfxCount),
            P0EAvg(GE_P0E_IMPORT,sampleGfxCount),
            P0EAvg(GE_P0E_CLIP,sampleGfxCount),
            P0EAvg(GE_P0E_PACK,sampleGfxCount),
            P0EAvg(GE_P0E_SUBMIT,sampleGfxCount));
        GENativePlatform_TraceIssue(
            "PH0E OPCODES stage=%d samples=%u total=%u vtx=%u tri=%u matrix=%u state=%u texstate=%u rect=%u control=%u overflow=%u\n",
            stage,sampleGfxCount,
            P0EOps(GE_P0E_DISPATCH)+P0EOps(GE_P0E_VERTEX)+
            P0EOps(GE_P0E_TRIANGLE)+P0EOps(GE_P0E_MATRIX)+
            P0EOps(GE_P0E_STATE)+P0EOps(GE_P0E_TEXTURE_STATE)+
            P0EOps(GE_P0E_RECT)+P0EOps(GE_P0E_CONTROL),
            P0EOps(GE_P0E_VERTEX),P0EOps(GE_P0E_TRIANGLE),
            P0EOps(GE_P0E_MATRIX),P0EOps(GE_P0E_STATE),
            P0EOps(GE_P0E_TEXTURE_STATE),P0EOps(GE_P0E_RECT),
            P0EOps(GE_P0E_CONTROL),
            (unsigned)(P0Load(&s_p0eTotalOverflows)-s_p0.gfxExclusiveOverflowStart));
#endif
    }
}
#endif
