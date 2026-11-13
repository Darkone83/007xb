#ifndef _RANDOM_H_
#define _RANDOM_H_
#include <ultra64.h>

extern u64 g_randomSeed;

void randomSetSeed(u32 param_1);
u32 randomGetNext(void);
u32 randomGetNextFrom(u64 *param_1);

#if defined(GE_XBOX_NATIVE32)
/* Replay-only bounded history. Capturing is memory-only; Dump is called only
 * after the existing replay RNG check has failed. The exported unlabelled
 * entry point remains available for function pointers and legacy callers. */
u32 randomGetNextAt(const char *file, u32 line);
void randomReplayTraceStart(void);
void randomReplayTracePacket(u32 packet, s32 tick);
void randomReplayTraceStop(void);
void randomReplayTraceDump(void);
/* Read-only snapshot of the first replay RNG mismatch for ISO/xemu testing.
 * The game thread publishes a complete snapshot; the graphics thread reads
 * immutable lines. It never adjusts the global seed or replay scheduling. */
void randomReplayTraceFreezeScreen(u32 packet, s32 tick, u8 expected,
                                   u8 actual, u64 state, s32 gunSync100);
const char *randomReplayTraceScreenLine(u32 index);
#if !defined(GE_NATIVE_RANDOM_IMPLEMENTATION)
#define randomGetNext() randomGetNextAt(__FILE__, (u32)__LINE__)
#endif
#endif

// 4294967295 == UINT_MAX
#define RANDOMGETNEXT_F32() ((f32) (u32)randomGetNext() * (1.0f / 4294967295))

#endif
