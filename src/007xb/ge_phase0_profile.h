#ifndef GE_PHASE0_PROFILE_H
#define GE_PHASE0_PROFILE_H
/* Phase 0 only: bounded, sampled wall-time observations. No gameplay changes.
 * Build with GE_XBOX_PHASE0_PROFILE=0 to compile every hook away.
 * This header is C/C++ compatible and requires no allocations. */
#ifndef GE_XBOX_PHASE0_PROFILE
#define GE_XBOX_PHASE0_PROFILE 1
#endif
/* Phase 0E runs only on the existing Phase 0B selected graphics tasks.
 * A/B: compile with GE_XBOX_PHASE0E_PROFILE=0 to omit these probes. */
#ifndef GE_XBOX_PHASE0E_PROFILE
#define GE_XBOX_PHASE0E_PROFILE 0 /* PH0E measurements collected; normal A/B now OFF */
#endif

/* Instrument only the rendering task already selected by Phase 0.
 * Categories overlap (a display-list call invokes native draw and submit).
 * Do NOT add these durations together as exclusive CPU time. */
enum { GE_P0B_DL=0, GE_P0B_DRAW, GE_P0B_UPLOAD, GE_P0B_FLUSH,
       GE_P0B_PRESENT, GE_P0B_VB_LOCK, GE_P0B_TEX_LOCK,
       GE_P0B_DRAW_UP, GE_P0B_DRAW_VB, GE_P0B_SHADER_CREATE,
       GE_P0B_COUNT };

/* Phase 0E graphics-worker wall-time-exclusive, nested Fast3D categories. Each measured tick
 * belongs to exactly one category; child ticks are removed from parents. */
enum { GE_P0E_DISPATCH=0, GE_P0E_VERTEX, GE_P0E_TRIANGLE,
       GE_P0E_MATRIX, GE_P0E_STATE, GE_P0E_TEXTURE_STATE,
       GE_P0E_RECT, GE_P0E_CONTROL, GE_P0E_MATERIAL,
       GE_P0E_IMPORT, GE_P0E_CLIP, GE_P0E_PACK,
       GE_P0E_SUBMIT, GE_P0E_COUNT };

#if GE_XBOX_PHASE0_PROFILE
#ifdef __cplusplus
extern "C" {
#endif

enum { GE_P0_LOGIC=1, GE_P0_RENDER=2, GE_P0_PREP=3 };
enum { GE_P0_ROOM_DEFRAG=0, GE_P0_MEMA_DEFRAG=1, GE_P0_MEMA_SEARCH=2 };
enum { GE_P0_PAD_REMOVE=0, GE_P0_PAD_OPEN_OK=1, GE_P0_PAD_OPEN_FAILED=2,
       GE_P0_PAD_READ_FAILED=3 };

void geP0FrameBegin(int stage);
void geP0PhaseMark(int phase);
void geP0FrameEnd(void);
void geP0StageExit(int stage);
void geP0ScopeStart(int which);
void geP0ScopeEnd(int which);
void geP0MemaSearchEntry(void);
void geP0MemaSearchPass(void);
void geP0PadEvent(int what);
void geP0RetraceDrops(unsigned count);
void geP0VisibleProps(unsigned count);
void geP0HostBegin(void);
void geP0HostInputDone(void);
void geP0HostEnd(void);
/* Return 0 when this task is not selected for a timing sample. */
unsigned long long geP0GraphicsBegin(void);
void geP0GraphicsEnd(unsigned long long start);
/* Returns zero when this graphics task was not sampled. These calls never
 * sample or write logs outside the existing 1-in-16 graphics task. */
unsigned long long geP0BStart(void);
void geP0BEnd(int category,unsigned long long start);
void geP0BRecord(int category,unsigned microseconds);
#if GE_XBOX_PHASE0E_PROFILE
/* Enter returns true only if this task is selected AND a scope was pushed.
 * Root DISPATCH scopes are explicitly placed around gfx_run's DL region. */
int geP0EEnter(int category);
void geP0ELeave(void);
void geP0ECommandNote(int category);
#endif

#ifdef __cplusplus
}
#endif
#else
#define geP0FrameBegin(stage) ((void)0)
#define geP0PhaseMark(phase) ((void)0)
#define geP0FrameEnd() ((void)0)
#define geP0StageExit(stage) ((void)0)
#define geP0ScopeStart(which) ((void)0)
#define geP0ScopeEnd(which) ((void)0)
#define geP0MemaSearchEntry() ((void)0)
#define geP0MemaSearchPass() ((void)0)
#define geP0PadEvent(what) ((void)0)
#define geP0RetraceDrops(count) ((void)0)
#define geP0VisibleProps(count) ((void)0)
#define geP0HostBegin() ((void)0)
#define geP0HostInputDone() ((void)0)
#define geP0HostEnd() ((void)0)
#define geP0GraphicsBegin() (0)
#define geP0GraphicsEnd(start) ((void)0)
#define geP0BStart() (0)
#define geP0BEnd(category,start) ((void)0)
#define geP0BRecord(category,microseconds) ((void)0)
#endif
/* C++98 RXDK compatible: destructor handles DL branch/return paths. */
#if GE_XBOX_PHASE0_PROFILE && GE_XBOX_PHASE0E_PROFILE && defined(__cplusplus)
struct GEPhase0EScope {
    const int entered;
    explicit GEPhase0EScope(int category):entered(geP0EEnter(category)) {}
    ~GEPhase0EScope() { if(entered)geP0ELeave(); }
private:
    GEPhase0EScope(const GEPhase0EScope&);
    GEPhase0EScope& operator=(const GEPhase0EScope&);
};
#define GE_P0E_JOIN_I(a,b) a##b
#define GE_P0E_JOIN(a,b) GE_P0E_JOIN_I(a,b)
#define GE_P0E_SCOPE(category) GEPhase0EScope GE_P0E_JOIN(geP0eScope_,__LINE__)(category)
#else
#define GE_P0E_SCOPE(category) ((void)0)
#define geP0ECommandNote(category) ((void)0)
#endif

#endif /* GE_PHASE0_PROFILE_H */
