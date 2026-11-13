#ifndef _UNK_0C0A70_H_
#define _UNK_0C0A70_H_
#include <ultra64.h>

/* Counts per authored VI tick, shared by live and recorded input timing.
 * The native Xbox osGetCount bridge uses this game's 46.5525 MHz clock. */
#ifdef REFRESH_PAL
#define FRAMETIMING_COUNTS_PER_TICK 931050U
#else
#define FRAMETIMING_COUNTS_PER_TICK 775875U
#endif

extern s32 lastFrameCounter;
extern s32 currentFrameCounter;
extern s32 speedgraphframes;

#if defined (BUGFIX_R1)
extern f32 jpD_800484CC;
extern f32 jpD_800484D0;
#endif

extern s32 previousFrameCounter;
extern s32 halfFrameCounter;
extern s32 isFrameCounterOdd;
extern s32 halfMinusPreviousCounter;
extern u32 copy_of_osgetcount_value_0;
extern u32 copy_of_osgetcount_value_1;
extern s32 frameDelay;

void waitForNextFrame(void);

#endif
