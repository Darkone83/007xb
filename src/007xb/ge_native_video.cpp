/* Native GoldenEye frame boundary on the existing 007xb D3D8 device. */
#include <xtl.h>
#include <string.h>
#include "xb_video.h"
#include "xb_render.h"
#include "xb_logo.h"
#include "xb_ge_audio.h"
#include "xb_ge_fast3d.h"
#include "ge_native_platform.h"
#include "ge_native_video.h"
#include "ge_native_gfx.h"
#include "ge_native_frame_stats.h"
#include "ge_native_error.h"
#include "ge_phase0_profile.h"
#include "engine/port/fast3d/gfx_window_manager_api.h"

extern "C" uint64_t sysGetMicroseconds(void);
#if defined(GE_XBOX_NATIVE32)
extern "C" void GEOSNativeSchedulerSnapshot(unsigned* retraces,unsigned* dropped);
extern "C" void geNativeFast3DTriSetupCounters(unsigned* prepared,unsigned* reused);
extern "C" unsigned geNativeFast3DTriSetupCacheEnabled(void);
extern "C" void geNativeFast3DVertexBakeCounters(unsigned* prepared,unsigned* reused);
extern "C" unsigned geNativeFast3DVertexCacheEnabled(void);
/* 6B diagnostics, cumulative, sampled only at display-window boundaries.
 * Indices: room markers, triangle runs, short runs, material, vertices,
 * camera bounds, uploads, backend refuses. */
extern "C" void geNativeRoomPipelineCounters(unsigned out[8]);
extern "C" void geNativeFast3DTextureCacheCounters(unsigned* capacity,
    unsigned* peak,unsigned* hits,unsigned* misses,unsigned* evictions);
#endif

static XBVideo *s_video=0;
static XBRenderer *s_draw=0;
static XBLogo *s_logo=0;
/* RXDK's Interlocked declarations take LPLONG. Keep storage volatile and
 * remove that qualifier only at the atomic API boundary below. */
static volatile LONG s_rendering=0,s_failed=0,s_showDiagnostics=0;
static unsigned s_frames=0;
static int s_ready=0,s_scene=0;
static int s_nativeWidth=640,s_nativeHeight=480;
static DWORD s_logoState=0;
static int s_logoStateValid=0;
static DWORD s_ownerThread=0,s_renderThread=0;
static int s_traceStage=-1;
static unsigned s_traceFrame=0,s_activeTraceFrame=0;
static unsigned s_stageCompletedFrames=0;
static GEFramePerf s_framePerf;
static uint64_t s_perfTaskStart=0,s_perfPresentEnd=0;
static unsigned s_perfWorkUs=0,s_perfPresentUs=0;
static bool s_perfPresented=false;
static char s_perfLine1[128]="FRAME TIMING: COLLECTING";
static char s_perfLine2[128]="";
static char s_perfRoomLine[128]="ROOMHW: COLLECTING";
static char s_perfRoomRejectLine[128]="";
static unsigned s_roomLastStats[8]={0};
static unsigned s_roomLastGpuTriangles=0;
static unsigned RoomWindowDelta(unsigned now,unsigned &last) {
    /* Reset on backend reattach, otherwise monotonic cumulative counters. */
    const unsigned delta=(now>=last)?now-last:now;
    last=now;return delta;
}
#if !GE_NATIVE_TRACE_VERBOSE
/* Quiet builds have NO async trace writer.  Keep a bounded in-memory record
 * on the render thread and flush it only during a stage transition (game
 * thread), never while the GPU is building or presenting a frame. */
#define GE_DIP_HISTORY 8u
struct GEDeferredDip { int stage; unsigned completed; GEFramePerfWindow window; };
static GEDeferredDip s_dipHistory[GE_DIP_HISTORY];
static unsigned s_dipHistoryCount=0,s_dipHistoryDropped=0;
#endif
/* The title intentionally submits blank switch-screen tasks before legal.
 * Serialize checkpoints with whole graphics tasks until that real frame
 * presents; keep the same device ownership rule after diagnostics retire. */
static CRITICAL_SECTION s_bootLock;
static int s_bootLockReady=0,s_bootDisplay=0;
static int s_bootFirstTask=1,s_bootFinalTask=0;
static uintptr_t s_bootFrameEnd=0;
static char s_bootLabel[64];

/* Called with the device lock held, after timings have been captured.
 * The on-screen summary remains available in the default issue-only mode. */
static void ReportFramePerf(void) {
    const GEFramePerfWindow& p=s_framePerf.window;
    if(!p.frames)return;
    const unsigned work=GEFramePerf::Average(p.workUs,p.frames);
    const unsigned present=GEFramePerf::Average(p.presentUs,p.frames);
    const unsigned fps10=p.gapUs?(unsigned)((10000000ull*p.gaps)/p.gapUs):0;
    const unsigned max10=p.maxGapUs/100u,work10=work/100u,present10=present/100u;
    geXboxSnprintf(s_perfLine1,sizeof(s_perfLine1),
        "FPS %u.%u  MAX GAP %u.%u MS  OVER 35 MS %u",
        fps10/10u,fps10%10u,max10/10u,max10%10u,p.over35ms);
    geXboxSnprintf(s_perfLine2,sizeof(s_perfLine2),
        "GFX %u.%u MS  PRESENT %u.%u MS  BATCHES %u",
        work10/10u,work10%10u,present10/10u,present10%10u,
        GEFramePerf::Average(p.batches,p.frames));
    /* ROOMHW is a TOTAL across the same window as FPS/GFX, not the one
     * arbitrary frame where ReportFramePerf happened to run (old 6B bug). */
    unsigned now[8]={0},delta[8]={0};
#if defined(GE_XBOX_NATIVE32)
    geNativeRoomPipelineCounters(now);
#endif
    for(unsigned i=0;i<8u;++i)
        delta[i]=RoomWindowDelta(now[i],s_roomLastStats[i]);
    const unsigned hardware=RoomWindowDelta(
        ge_xbox_d3d8_room_hardware_triangles_total(),s_roomLastGpuTriangles);
    geXboxSnprintf(s_perfRoomLine,sizeof(s_perfRoomLine),
        "ROOMHW %u TRI / %u FR  ROOMS %u  RUNS %u  UP %u",
        hardware,p.frames,delta[0],delta[1],delta[6]);
    geXboxSnprintf(s_perfRoomRejectLine,sizeof(s_perfRoomRejectLine),
        "ROOM SKIP SHORT %u  MAT %u  VTX %u  NEAR %u  GPU %u",
        delta[2],delta[3],delta[4],delta[5],delta[7]);
#if GE_NATIVE_TRACE_VERBOSE
    const unsigned gap=GEFramePerf::Average(p.gapUs,p.gaps);
    GENativePlatform_TraceDeferred(
        "FRAME PERF stage=%d completed=%u samples=%u gaps=%u "
        "gap_us_avg=%u gap_us_max=%u over35ms=%u over50ms=%u over100ms=%u "
        "gfx_us_avg=%u gfx_us_max=%u present_us_avg=%u present_us_max=%u "
        "task_us_avg=%u task_us_max=%u draw_calls_avg=%u batches_avg=%u "
        "tri_in_avg=%u tri_out_avg=%u uploads=%u upload_kib=%u shader_creates=%u "
        "worst_gfx_us=%u worst_present_us=%u worst_task_us=%u worst_batches=%u worst_tri_in=%u "
        "worst_tri_out=%u worst_uploads=%u worst_upload_bytes=%u worst_shader_creates=%u "
        "trace_dropped=%u\n",
        s_traceStage,s_stageCompletedFrames,p.frames,p.gaps,gap,p.maxGapUs,
        p.over35ms,p.over50ms,p.over100ms,work,p.maxWorkUs,present,p.maxPresentUs,
        GEFramePerf::Average(p.taskUs,p.frames),p.maxTaskUs,
        GEFramePerf::Average(p.drawCalls,p.frames),GEFramePerf::Average(p.batches,p.frames),
        GEFramePerf::Average(p.inputTriangles,p.frames),GEFramePerf::Average(p.outputTriangles,p.frames),
        p.uploads,(unsigned)(p.uploadBytes/1024u),p.shaderCreates,
        p.worstWorkUs,p.worstPresentUs,p.worstTaskUs,p.worstBackend.gpu_batches,
        p.worstBackend.input_triangles,p.worstBackend.output_triangles,
        p.worstBackend.texture_uploads,p.worstBackend.texture_upload_bytes,
        p.worstBackend.shader_creates,GENativePlatform_TraceDeferredDropped());
#else
    /* A long replay/title loop often includes one loading hitch per window.
     * Keeping only maximum single-frame gaps displaced sustained 20 FPS
     * gameplay windows from the eight-slot history. Rank by average gap
     * instead; retain isolated >100ms hitches when room is available. */
    const unsigned gapAverage=GEFramePerf::Average(p.gapUs,p.gaps);
    if(p.frames>=30u && (gapAverage>25000u || p.maxGapUs>100000u)) {
        unsigned slot=s_dipHistoryCount;
        if(slot<GE_DIP_HISTORY) ++s_dipHistoryCount;
        else {
            /* Retain the eight worst dip windows, not merely the first
             * eight in a long recording.  Do not evict a more severe hitch. */
            slot=0;
            for(unsigned i=1;i<GE_DIP_HISTORY;++i)
                if(GEFramePerf::Average(s_dipHistory[i].window.gapUs,s_dipHistory[i].window.gaps) <
                   GEFramePerf::Average(s_dipHistory[slot].window.gapUs,s_dipHistory[slot].window.gaps))
                    slot=i;
            ++s_dipHistoryDropped;
            if(gapAverage<=GEFramePerf::Average(s_dipHistory[slot].window.gapUs,
              s_dipHistory[slot].window.gaps))slot=GE_DIP_HISTORY;
        }
        if(slot<GE_DIP_HISTORY) {
            GEDeferredDip& record=s_dipHistory[slot];
            record.stage=s_traceStage;
            record.completed=s_stageCompletedFrames;
            record.window=p;
        }
    }
#endif
    /* Keep the previous Present timestamp across windows, but never stages. */
    s_framePerf.ClearWindow();
}

static void SetBootLabel(const char *message) {
    unsigned n=0;
    while(n+1u<sizeof(s_bootLabel) && message[n]) {
        s_bootLabel[n]=message[n];++n;
    }
    s_bootLabel[n]=0;
}
/* Caller holds s_bootLock. An overlay uses the existing original scene. */
static void DrawBootStep(const char *message,int standalone) {
    IDirect3DDevice8 *d=s_video->Device();
    if(!s_logoStateValid || FAILED(d->CaptureStateBlock(s_logoState))) {
        GENativePlatform_SetError("D3D8 startup checkpoint state capture failed");
        s_failed=1;return;
    }
    D3DVIEWPORT8 viewport={0,0,(DWORD)s_video->Width(),(DWORD)s_video->Height(),0.0f,1.0f};
    if(FAILED(d->SetViewport(&viewport))) {
        d->ApplyStateBlock(s_logoState);
        GENativePlatform_SetError("D3D8 startup checkpoint viewport failed");
        s_failed=1;return;
    }
    d->SetPixelShader(0);
    if(standalone)s_video->Begin();
    /* Startup checkpoints remain active, but no debug text is drawn.
     * Preserve the known-good D3D8 state and frame lifecycle. */
    (void)message;
    if(standalone)s_video->End();
    if(FAILED(d->ApplyStateBlock(s_logoState))) {
        GENativePlatform_SetError("D3D8 startup checkpoint state restore failed");
        s_failed=1;
    }
}

/* Back-button statistics: keep all text inside a title-safe 4:3 viewport,
 * including 720p pillarboxed gameplay and anamorphic 480p/576i widescreen.
 * This is diagnostic-only drawing; the original game viewport is restored. */
#define GE_STATS_MAX_ROWS 16u
struct GEStatsOverlayRow {
    char text[128];
    DWORD colour;
    unsigned section;
};
static unsigned GEStatsWrap(GEStatsOverlayRow *rows,unsigned count,
                            const char *source,DWORD colour,unsigned section,
                            unsigned maxChars) {
    if(!source)return count;
    if(maxChars>=sizeof(rows[0].text))maxChars=sizeof(rows[0].text)-1u;
    while(*source && count<GE_STATS_MAX_ROWS) {
        while(*source==' ' || *source=='\n')++source;
        if(!*source)break;
        unsigned take=0;
        while(source[take] && source[take]!='\n' && take<maxChars)++take;
        if(take==maxChars && source[take] && source[take]!='\n') {
            unsigned space=take;
            while(space && source[space]!=' ')--space;
            if(space>=maxChars/3u)take=space;
        }
        if(!take)break;
        memcpy(rows[count].text,source,take);
        rows[count].text[take]=0;
        rows[count].colour=colour;
        rows[count].section=section;
        ++count;
        source+=take;
    }
    return count;
}
static void GEStatsDrawBackground(IDirect3DDevice8 *d,
                                  float x,float y,float width,float height) {
    const float right=x+width,bottom=y+height;
    const DWORD colour=0xC0000000u;
    XBScreenVertex v[6]={
        {x,y,0.5f,1.0f,colour},{right,y,0.5f,1.0f,colour},
        {x,bottom,0.5f,1.0f,colour},{right,y,0.5f,1.0f,colour},
        {right,bottom,0.5f,1.0f,colour},{x,bottom,0.5f,1.0f,colour}
    };
    d->SetTexture(0,0);
    d->SetPixelShader(0);
    d->SetVertexShader(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);
    d->SetRenderState(D3DRS_ZENABLE,FALSE);
    d->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
    d->SetRenderState(D3DRS_LIGHTING,FALSE);
    d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
    d->SetRenderState(D3DRS_FOGENABLE,FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
    d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
    d->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);
    d->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
    d->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
    d->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);
    d->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);
    d->SetTextureStageState(1,D3DTSS_ALPHAOP,D3DTOP_DISABLE);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST,2u,v,sizeof(v[0]));
}
static void GEStatsDrawOverlay(IDirect3DDevice8 *d,int traceReady) {
    const float width=(float)s_video->Width(),height=(float)s_video->Height();
    if(width<160.0f || height<120.0f)return;
    /* A 4:3 area on a 16:9 display can be narrower than the backbuffer.
     * Widescreen 480p uses non-square pixels: its 4:3 area is 75% of the
     * encoded 640-pixel width, not 640 pixels. */
    float contentWidth=width;
    if(s_video->IsWidescreen()) {
        if(width*9.0f>=height*16.0f)contentWidth=height*(4.0f/3.0f);
        else contentWidth=width*0.75f;
    }
    if(contentWidth>width)contentWidth=width;
    const float contentLeft=(width-contentWidth)*0.5f;
    const float insetX=contentWidth*0.06f;
    const float insetY=height*0.06f;
    const float left=contentLeft+insetX;
    const float usableWidth=contentWidth-2.0f*insetX;
    const float outputScale=height/480.0f;
    const float fontScale=1.10f*outputScale;
    const float pad=9.0f*outputScale;
    const float lineStep=10.0f*fontScale;
    const float sectionGap=5.0f*fontScale;
    const float shadow=1.1f*outputScale;
    unsigned maxChars=(unsigned)((usableWidth-2.0f*pad)/(6.0f*fontScale));
    if(maxChars<12u)maxChars=12u;
    if(maxChars>126u)maxChars=126u;
    GEStatsOverlayRow rows[GE_STATS_MAX_ROWS];
    unsigned count=0;
    count=GEStatsWrap(rows,count,"007XB - ORIGINAL GAME / FAST3D / LIBAUDIO",
        0xFFE5EBF3u,0u,maxChars);
    count=GEStatsWrap(rows,count,traceReady?
        (GE_NATIVE_TRACE_VERBOSE?"LOG: VERBOSE":"LOG: ISSUES ONLY"):
        "TRACE WRITE FAILED",0xFFFFB065u,1u,maxChars);
    count=GEStatsWrap(rows,count,s_perfLine1,0xFFFFFFFFu,2u,maxChars);
    count=GEStatsWrap(rows,count,s_perfLine2,0xFFFFFFFFu,2u,maxChars);
    count=GEStatsWrap(rows,count,s_perfRoomLine,0xFFC4E9FFu,2u,maxChars);
    count=GEStatsWrap(rows,count,s_perfRoomRejectLine,0xFFFFD2AAu,2u,maxChars);
    if(!count)return;
    unsigned widest=0;
    for(unsigned i=0;i<count;++i) {
        const unsigned length=(unsigned)strlen(rows[i].text);
        if(length>widest)widest=length;
    }
    float panelWidth=2.0f*pad+(float)widest*6.0f*fontScale;
    if(panelWidth>usableWidth)panelWidth=usableWidth;
    const float panelHeight=2.0f*pad+(float)count*lineStep+sectionGap;
    GEStatsDrawBackground(d,left,insetY,panelWidth,panelHeight);
    float y=insetY+pad;
    for(unsigned i=0;i<count;++i) {
        if(i && rows[i].section==2u && rows[i-1].section!=2u)y+=sectionGap;
        s_draw->DrawText(d,left+pad+shadow,y+shadow,fontScale,
                         rows[i].text,0xFF000000u);
        s_draw->DrawText(d,left+pad,y,fontScale,rows[i].text,rows[i].colour);
        y+=lineStep;
    }
}

static void WM_Init(const GfxWindowInitSettings *) {}
static void WM_Close(void) {}
static int WM_Mode(int index,int *w,int *h) {
    if(index!=0 || !w || !h || !s_video)return 0;
    *w=s_video->Width();*h=s_video->Height();return 1;
}
static int WM_CurrentMode(int *w,int *h) { return WM_Mode(0,w,h); }
static int WM_NumModes(void) { return 1; }
static int32_t WM_Fullscreen(void) { return 1; }
static void WM_FullscreenCallback(void (*)(bool)) {}
static void WM_BoolSetting(bool) {}
static void WM_IntSetting(int32_t) {}
static void WM_Refresh(uint32_t *hz) { if(hz)*hz=s_video?(uint32_t)s_video->RefreshRate():60u; }
static void WM_Closest(int32_t,int32_t,bool) {}
static void WM_SetDimensions(uint32_t,uint32_t,int32_t,int32_t) {}
static void WM_Dimensions(uint32_t *w,uint32_t *h,int32_t *x,int32_t *y) {
    if(w)*w=s_video?(unsigned)s_video->Width():640u;
    if(h)*h=s_video?(unsigned)s_video->Height():480u;
    if(x)*x=0;if(y)*y=0;
}
static void WM_Centered(int32_t w,int32_t h,int32_t *x,int32_t *y) {
    if(x)*x=((s_video?s_video->Width():640)-w)/2;
    if(y)*y=((s_video?s_video->Height():480)-h)/2;
}
static void WM_Events(void) {}
static bool WM_Start(void) {
    if(!s_video || s_failed || GENativePlatform_HasFatalError())return false;
    if(!s_scene) {
        if(FAILED(s_video->Device()->BeginScene())) {
            GENativePlatform_SetError("D3D8 BeginScene failed");s_failed=1;return false;
        }
        s_scene=1;
    }
    return true;
}
static void WM_SwapBegin(void) {
    if(s_scene) {
        if(s_bootDisplay && !s_bootFinalTask && s_bootLabel[0])
            DrawBootStep(s_bootLabel,0);
        /* Never draw RNG history or a trace-write warning during normal
         * rendering.  On ISO-mounted xemu a failed D: log otherwise caused
         * full D3D8 CaptureStateBlock/ApplyStateBlock and up to 14 text draws
         * on EVERY game frame.  ramromreplay.c already logs the complete RNG
         * state and caller ring to the issue trace on the first mismatch.
         * Keep the on-screen FPS overlay only when explicitly enabled. */
        if(s_showDiagnostics && s_draw && s_logoStateValid &&
           SUCCEEDED(s_video->Device()->CaptureStateBlock(s_logoState))) {
            IDirect3DDevice8 *d=s_video->Device();
            D3DVIEWPORT8 originalViewport;
            const bool savedViewport=SUCCEEDED(d->GetViewport(&originalViewport));
            D3DVIEWPORT8 fullViewport={0u,0u,(DWORD)s_video->Width(),
                (DWORD)s_video->Height(),0.0f,1.0f};
            /* A scene can leave a 4:3 viewport active. Draw the diagnostics
             * in full backbuffer coordinates, then restore that viewport. */
            const bool fullViewportSet=SUCCEEDED(d->SetViewport(&fullViewport));
            if(fullViewportSet) {
                /* Fast3D can leave an NV2A hardware scissor around its last
                 * viewport. Clear it for the overlay so 4:3 edges cannot
                 * trim text, then let the backend restore the game scissor. */
                D3DRECT fullClip={0,0,(LONG)s_video->Width(),(LONG)s_video->Height()};
                d->SetScissors(1,FALSE,&fullClip);
                GEStatsDrawOverlay(d,GENativePlatform_TraceReady());
            }
            if(savedViewport)d->SetViewport(&originalViewport);
            d->ApplyStateBlock(s_logoState);
            ge_xbox_d3d8_external_end();
        }
        s_video->Device()->EndScene();s_scene=0;
    }
}
static void WM_SwapEnd(void) {
    const int trace=s_traceStage>=0 && s_activeTraceFrame>0 && s_activeTraceFrame<=3;
    if(trace)GENativePlatform_Trace("FRAME PRESENT_BEGIN stage=%d frame=%u\n",
        s_traceStage,s_activeTraceFrame);
    const uint64_t presentBegin=sysGetMicroseconds();
    const HRESULT result=s_video->Device()->Present(0,0,0,0);
    const uint64_t presentEnd=sysGetMicroseconds();
    if(presentEnd>=presentBegin && presentEnd-presentBegin<=10000000ull)
        geP0BRecord(GE_P0B_PRESENT,(unsigned)(presentEnd-presentBegin));
    if(SUCCEEDED(result)) {
        s_perfWorkUs=GEFramePerf::Elapsed(s_perfTaskStart,presentBegin);
        s_perfPresentUs=GEFramePerf::Elapsed(presentBegin,presentEnd);
        s_perfPresentEnd=presentEnd;s_perfPresented=true;
    }
    if(trace)GENativePlatform_Trace("FRAME PRESENT_RETURN stage=%d frame=%u result=%08lX\n",
        s_traceStage,s_activeTraceFrame,(unsigned long)result);
    if(FAILED(result)) {
        if(!s_failed)GENativePlatform_TraceIssue(
            "VIDEO PRESENT_FAILED stage=%d completed=%u result=%08X\n",
            s_traceStage,s_stageCompletedFrames,(unsigned)result);
        GENativePlatform_SetError("D3D8 Present failed");s_failed=1;
    }
}
static double WM_Time(void) { return (double)GetTickCount()*0.001; }
static int32_t WM_FPS(void) { return s_video?s_video->RefreshRate():60; }
static bool WM_NoVsyncOverride(void) { return false; }
static void *WM_Handle(void) { return s_video?s_video->Device():0; }
static void WM_Title(const char *) {}
static int WM_Interval(void) { return 1; }
static bool WM_SetInterval(int interval) { return interval==1; }

static GfxWindowManagerAPI s_window={
    WM_Init,WM_Close,WM_Mode,WM_CurrentMode,WM_NumModes,
    WM_Fullscreen,WM_FullscreenCallback,WM_BoolSetting,WM_BoolSetting,
    WM_IntSetting,WM_Fullscreen,WM_Fullscreen,WM_BoolSetting,
    WM_Refresh,WM_BoolSetting,WM_Closest,WM_SetDimensions,WM_Dimensions,WM_Centered,
    WM_Events,WM_Start,WM_SwapBegin,WM_SwapEnd,WM_Time,WM_FPS,
    WM_IntSetting,WM_NoVsyncOverride,WM_Handle,WM_Title,WM_Interval,WM_SetInterval
};

extern "C" int GENativeVideo_Bind(void *video,void *renderer,void *logo) {
    if(!s_bootLockReady) {
        InitializeCriticalSection(&s_bootLock);
        s_bootDisplay=1;s_bootLockReady=1;
    }
    s_video=(XBVideo *)video;s_draw=(XBRenderer *)renderer;s_logo=(XBLogo *)logo;
    s_ownerThread=GetCurrentThreadId();
    return s_video && s_video->Device() && s_draw && s_logo && s_logo->Points();
}
extern "C" int GENativeVideo_Start(void) {
    if(s_ready)return 1;
    if(!s_video || !s_video->Device())return 0;
    if(FAILED(s_video->Device()->CreateStateBlock(D3DSBT_ALL,&s_logoState))) {
        GENativePlatform_SetError("Cannot preserve D3D8 state for startup screens and the custom logo");return 0;
    }
    s_logoStateValid=1;
    geNativeGfxDisplayAspect(videoGetDisplayAspectRatio());
    if(!geNativeGfxInit(&s_window,&gfx_xbox_d3d8_api,
                      (unsigned)s_video->Width(),(unsigned)s_video->Height()))return 0;
    geNativeGfxResolution(s_nativeWidth,s_nativeHeight);
    GENativePlatform_Trace("VIDEO mode=%s width=%d height=%d refresh=%d wide=%d depth=%d\n",
        s_video->ModeName(),s_video->Width(),s_video->Height(),s_video->RefreshRate(),
        s_video->IsWidescreen()?1:0,s_video->HasDepth()?1:0);
    s_ready=1;return 1;
}
extern "C" void GENativeVideo_BootStep(const char *message) {
    if(!message || !*message || !s_bootLockReady)return;
    EnterCriticalSection(&s_bootLock);
    if(s_bootDisplay && s_ready && !s_failed && !GENativePlatform_HasFatalError()) {
        SetBootLabel(message);
        const LONG previous=InterlockedExchange(const_cast<LONG *>(&s_rendering),1);
        DrawBootStep(s_bootLabel,1);
        InterlockedExchange(const_cast<LONG *>(&s_rendering),previous);
    }
    LeaveCriticalSection(&s_bootLock);
}
extern "C" void GENativeVideo_BootFrameReady(const void *legalDlEnd) {
    if(!legalDlEnd || !s_bootLockReady)return;
    EnterCriticalSection(&s_bootLock);
    if(s_bootDisplay && !s_bootFrameEnd)s_bootFrameEnd=(uintptr_t)legalDlEnd;
    LeaveCriticalSection(&s_bootLock);
}
extern "C" void GENativeVideo_TraceStage(int stage) {
    if(!s_bootLockReady)return;
#if !GE_NATIVE_TRACE_VERBOSE
    GEDeferredDip captured[GE_DIP_HISTORY];
    unsigned capturedCount=0,omitted=0;
#endif
    EnterCriticalSection(&s_bootLock);
    ReportFramePerf();
#if !GE_NATIVE_TRACE_VERBOSE
    capturedCount=s_dipHistoryCount;
    for(unsigned i=0;i<capturedCount;++i)captured[i]=s_dipHistory[i];
    omitted=s_dipHistoryDropped;
    s_dipHistoryCount=s_dipHistoryDropped=0;
#endif
    s_traceStage=stage;s_traceFrame=0;s_activeTraceFrame=0;
    s_stageCompletedFrames=0;
    s_framePerf.Reset();s_perfPresented=false;
    strcpy(s_perfLine1,"FRAME TIMING: COLLECTING");s_perfLine2[0]=0;
    strcpy(s_perfRoomLine,"ROOMHW: COLLECTING");s_perfRoomRejectLine[0]=0;
    /* Keep cumulative baselines across stage changes; stats must represent
     * only the new stage/window rather than the entire running session. */
    unsigned nowRoomStats[8]={0};
#if defined(GE_XBOX_NATIVE32)
    geNativeRoomPipelineCounters(nowRoomStats);
#endif
    for(unsigned i=0;i<8u;++i)s_roomLastStats[i]=nowRoomStats[i];
    s_roomLastGpuTriangles=ge_xbox_d3d8_room_hardware_triangles_total();
    LeaveCriticalSection(&s_bootLock);
#if defined(GE_XBOX_NATIVE32)
    /* Cumulative counters: compare consecutive entries to isolate dropped
     * VI messages from heavy gameplay, without writing during a frame. */
    unsigned viRetraces=0,viDropped=0;
    GEOSNativeSchedulerSnapshot(&viRetraces,&viDropped);
    GENativePlatform_TraceIssue("SCHED VI stage_enter=%d retraces=%u dropped=%u\n",
        stage,viRetraces,viDropped);
    /* One snapshot per stage boundary; no I/O or formatting per triangle.
     * Both counts are cumulative to permit A/B delta measurements over the
     * previous scene without adding hot-path trace work. */
    unsigned triPrepared=0,triReused=0,vertPrepared=0,vertReused=0;
    geNativeFast3DTriSetupCounters(&triPrepared,&triReused);
    geNativeFast3DVertexBakeCounters(&vertPrepared,&vertReused);
    GENativePlatform_TraceIssue(
        "F3D PREP stage_enter=%d material_cache=%u vertex_cache=%u "
        "material_setups=%u material_reuses=%u vertex_bakes=%u vertex_reuses=%u\n",
        stage,geNativeFast3DTriSetupCacheEnabled(),geNativeFast3DVertexCacheEnabled(),
        triPrepared,triReused,vertPrepared,vertReused);
    /* The old 512-entry texture-cache limit can evict static Runway textures
     * repeatedly. Counts are cumulative and logged only on stage switches.
     * A/B build capacity 512/1024 while the GPU byte budget stays unchanged. */
    unsigned texCap=0,texPeak=0,texHits=0,texMiss=0,texEvict=0;
    geNativeFast3DTextureCacheCounters(&texCap,&texPeak,&texHits,&texMiss,&texEvict);
    GENativePlatform_TraceIssue(
        "F3D TEX stage_enter=%d slots=%u peak_allocated=%u hits=%u misses=%u evictions=%u gpu_bytes=%u\n",
        stage,texCap,texPeak,texHits,texMiss,texEvict,ge_xbox_d3d8_texture_bytes());
    /* Cumulative counters, sampled only at stage changes (not per frame).
     * This lets xemu traces prove the dynamic VB / strip atlas actually
     * operated on the Xbox, rather than merely in host tests. */
    GENativePlatform_TraceIssue(
        "NV2A PATH stage_enter=%d atlas_builds=%u atlas_rows=%u vb_draws=%u up_draws=%u\n",
        stage,ge_xbox_d3d8_atlas_builds(),ge_xbox_d3d8_atlas_rows(),
        ge_xbox_d3d8_stream_vb_draws(),ge_xbox_d3d8_stream_up_draws());
#endif
#if !GE_NATIVE_TRACE_VERBOSE
    /* A stage switch is not an active graphics frame.  Quiet trace storage
     * writes are synchronous by design; batch them here, never in Present. */
    for(unsigned i=0;i<capturedCount;++i) {
        const GEDeferredDip& d=captured[i];
        const GEFramePerfWindow& w=d.window;
        GENativePlatform_TraceIssue(
            "FRAME DIP stage=%d completed=%u samples=%u gaps=%u "
            "gap_us_avg=%u gap_us_max=%u over35ms=%u over50ms=%u over100ms=%u "
            "gfx_us_avg=%u gfx_us_max=%u present_us_avg=%u present_us_max=%u "
            "task_us_avg=%u task_us_max=%u draw_calls_avg=%u batches_avg=%u "
            "worst_gfx_us=%u worst_present_us=%u worst_task_us=%u "
            "worst_draw_calls=%u worst_batches=%u worst_tri_in=%u worst_tri_out=%u "
            "worst_uploads=%u worst_upload_bytes=%u worst_shader_creates=%u "
            "shader_probes_avg=%u software_addr_in_avg=%u screen_culls_avg=%u "
            "vb_locks_avg=%u vb_draws_avg=%u up_draws_avg=%u small_up_avg=%u "
            "draw_api_us_avg=%u submit_us_avg=%u upload_api_us_avg=%u fast_swizzles_avg=%u\n",
            d.stage,d.completed,w.frames,w.gaps,GEFramePerf::Average(w.gapUs,w.gaps),
            w.maxGapUs,w.over35ms,w.over50ms,w.over100ms,
            GEFramePerf::Average(w.workUs,w.frames),w.maxWorkUs,
            GEFramePerf::Average(w.presentUs,w.frames),w.maxPresentUs,
            GEFramePerf::Average(w.taskUs,w.frames),w.maxTaskUs,
            GEFramePerf::Average(w.drawCalls,w.frames),
            GEFramePerf::Average(w.batches,w.frames),w.worstWorkUs,
            w.worstPresentUs,w.worstTaskUs,w.worstBackend.draw_calls,w.worstBackend.gpu_batches,
            w.worstBackend.input_triangles,w.worstBackend.output_triangles,
            w.worstBackend.texture_uploads,w.worstBackend.texture_upload_bytes,
            w.worstBackend.shader_creates,
            GEFramePerf::Average(w.shaderProbes,w.frames),
            GEFramePerf::Average(w.softwareAddressTriangles,w.frames),
            GEFramePerf::Average(w.screenRejects,w.frames),
            GEFramePerf::Average(w.vbLocks,w.frames),
            GEFramePerf::Average(w.vbDraws,w.frames),
            GEFramePerf::Average(w.upDraws,w.frames),
            GEFramePerf::Average(w.smallUPDraws,w.frames),
            GEFramePerf::Average(w.drawApiUs,w.frames),
            GEFramePerf::Average(w.submitUs,w.frames),
            GEFramePerf::Average(w.uploadApiUs,w.frames),
            GEFramePerf::Average(w.fastSwizzles,w.frames));
    }
    if(omitted)GENativePlatform_TraceIssue(
        "FRAME DIP history_overflow stage_before=%d omitted_windows=%u\n",
        capturedCount?captured[0].stage:stage,omitted);
#endif
}
extern "C" int GENativeVideo_Failed(void) { return s_failed!=0; }
extern "C" int GENativeVideo_Rendering(void) { return s_rendering!=0; }
extern "C" unsigned GENativeVideo_Frames(void) { return s_frames; }
extern "C" unsigned videoGetFrameCount(void) { return s_frames; }
extern "C" float videoGetDisplayAspectRatio(void) {
    return s_video && s_video->IsWidescreen()?16.0f/9.0f:4.0f/3.0f;
}
extern "C" void GENativeVideo_SetDiagnostics(int show) { InterlockedExchange(const_cast<LONG *>(&s_showDiagnostics),show?1:0); }
extern "C" void GENativeVideo_OnFatal(void) {
    s_failed=1;
    const DWORD self=GetCurrentThreadId();
    if(self==s_renderThread) {
        if(s_scene) { s_video->Device()->EndScene();s_scene=0; }
        InterlockedExchange(const_cast<LONG *>(&s_rendering),0);
    }
    /* A startup failure parks the host itself, so leave its diagnostic
       on screen before the fatal handler stops that thread. */
    if(self==s_ownerThread && s_video && s_draw && !s_rendering) {
        s_video->Begin();
        s_draw->DrawText(s_video->Device(),24,38,2.2f,"007XB - STARTUP STOPPED",0xFFFFB065u);
        geNativeDrawErrorLines(s_draw,s_video->Device(),GENativePlatform_Error(),92.0f);
        s_video->End();
    }
}

extern "C" int geNativeRunGraphics(void *commands,unsigned bytes) {
    if(!s_ready || s_failed || GENativePlatform_HasFatalError())return 0;
    EnterCriticalSection(&s_bootLock);
    if(s_failed || GENativePlatform_HasFatalError()) {
        LeaveCriticalSection(&s_bootLock);return 0;
    }
    s_renderThread=GetCurrentThreadId();
    InterlockedExchange(const_cast<LONG *>(&s_rendering),1);
    if(s_traceFrame<4)++s_traceFrame;
    s_activeTraceFrame=s_traceFrame;
    const int trace=s_traceStage>=0 && s_activeTraceFrame<=3;
    if(trace)GENativePlatform_Trace("FRAME GFX_BEGIN stage=%d frame=%u dl=%08lX bytes=%u\n",
        s_traceStage,s_activeTraceFrame,(unsigned long)(uintptr_t)commands,bytes);
    const uintptr_t taskBegin=(uintptr_t)commands;
    s_bootFinalTask=s_bootDisplay && s_bootFrameEnd &&
        s_bootFrameEnd>=taskBegin && s_bootFrameEnd-taskBegin<(uintptr_t)bytes;
    if(s_bootDisplay && s_bootFirstTask) {
        s_bootFirstTask=0;
        SetBootLabel("FIRST GRAPHICS TASK / GPU HANDOFF");
        DrawBootStep(s_bootLabel,1);
    }
    if(s_bootFinalTask && !s_failed && !GENativePlatform_HasFatalError()) {
        SetBootLabel("LEGAL FRAME / GPU HANDOFF");
        DrawBootStep(s_bootLabel,1);
    }
    int result=0;
    s_perfPresented=false;
    s_perfTaskStart=sysGetMicroseconds();
    if(!s_failed && !GENativePlatform_HasFatalError())
        result=geNativeGfxFrame(commands,bytes);
    const uint64_t taskEnd=sysGetMicroseconds();
    if(s_scene)WM_SwapBegin();
    if(!result)s_failed=1;
    const int success=result && !s_failed && !GENativePlatform_HasFatalError();
    if(trace)GENativePlatform_Trace("FRAME GFX_RETURN stage=%d frame=%u success=%d dl_peak=%u\n",
        s_traceStage,s_activeTraceFrame,success,geNativeGfxMaxDLDepth());
    if(success) {
        ++s_frames;
        ++s_stageCompletedFrames;
        /* First three tasks carry synchronous startup checkpoints. Exclude
         * them and their interval from steady-state frame measurements. */
        if(s_stageCompletedFrames>3u && s_perfPresented) {
            GEBackendFrameStats backend;
            ge_xbox_d3d8_frame_stats(&backend);
            s_framePerf.Record(s_perfPresentEnd,s_perfWorkUs,s_perfPresentUs,
                GEFramePerf::Elapsed(s_perfTaskStart,taskEnd),backend);
            if(s_framePerf.window.frames>=120u)ReportFramePerf();
        }
        /* Keep a later crash distinguishable from an immediate first-frame
         * stop, without flushing a file on every steady-state frame. */
        if(s_traceStage>=0 && s_stageCompletedFrames%120u==0u)
            GENativePlatform_TraceDeferred("FRAME PROGRESS stage=%d completed=%u dl_peak=%u\n",
                s_traceStage,s_stageCompletedFrames,geNativeGfxMaxDLDepth());
        if(s_bootFinalTask) { s_bootDisplay=0;s_bootFrameEnd=0; }
    }
    s_bootFinalTask=0;
    s_activeTraceFrame=0;
    InterlockedExchange(const_cast<LONG *>(&s_rendering),0);
    LeaveCriticalSection(&s_bootLock);
    return success;
}
extern "C" void geNativeDrawLogo(float radians,float scale,float alpha) {
    if(!s_logoStateValid || !s_scene || !s_logo || !s_draw)return;
    /* This is issued between original RDP display-list commands, not at the
     * end of a frame. Make sure buffered Fast3D background geometry reaches
     * NV2A BEFORE the user-authorized Darkone83 logo is drawn. */
    ge_xbox_d3d8_external_begin();
    IDirect3DDevice8 *d=s_video->Device();
    if(FAILED(d->CaptureStateBlock(s_logoState))) {
        GENativePlatform_SetError("D3D8 logo state capture failed");s_failed=1;return;
    }
    D3DVIEWPORT8 viewport={0,0,(DWORD)s_video->Width(),(DWORD)s_video->Height(),0.0f,1.0f};
    d->SetViewport(&viewport);
    d->SetPixelShader(0);
    if(s_video->HasDepth())d->Clear(0,0,D3DCLEAR_ZBUFFER,0,1.0f,0);
    /* The game supplies its own rotation, growth and fade every frame. */
    s_draw->DrawCelLogo(d,*s_logo,s_video->Width(),s_video->Height(),
                       radians,alpha,s_video->HasDepth(),scale/1.1f);
    if(FAILED(d->ApplyStateBlock(s_logoState))) {
        GENativePlatform_SetError("D3D8 logo state restore failed");s_failed=1;
    } else {
        /* Force the native backend to re-establish state and VB binding for
         * subsequent N64 geometry, while preserving the logo draw order. */
        ge_xbox_d3d8_external_end();
    }
}
extern "C" void videoUpdateNativeResolution(int w,int h) {
    if(w<1 || h<1)return;
    /* Game and graphics now run on independent Xbox threads. Protect the
     * Fast3D coordinate-space globals against an in-flight GPU task. */
    if(s_bootLockReady)EnterCriticalSection(&s_bootLock);
    s_nativeWidth=w;s_nativeHeight=h;
    geNativeGfxResolution(w,h);
    if(s_bootLockReady)LeaveCriticalSection(&s_bootLock);
}
extern "C" void videoResetTextureCache(void) {
    if(!s_ready)return;
    /* boss.c invalidates the stage arena on the game thread. Wait until the
     * preceding D3D8/fast3d task releases its cache, otherwise the stage
     * reset can free a bound texture while the worker is rendering it. */
    if(s_bootLockReady)EnterCriticalSection(&s_bootLock);
    geNativeGfxTextureReset();
    /* The overlay state block also owns captured texture references. Refresh
     * it after the cache unbinds its images; never restore the discarded stage. */
    if(s_logoStateValid && FAILED(s_video->Device()->CaptureStateBlock(s_logoState))) {
        GENativePlatform_SetError("D3D8 stage texture state capture failed");
        s_failed=1;
    }
    if(s_bootLockReady)LeaveCriticalSection(&s_bootLock);
}
