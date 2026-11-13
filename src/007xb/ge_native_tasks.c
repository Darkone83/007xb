/* Typed bridge: the game owns OSTask and Gfx; the Xbox host sees opaque pointers. */
#include <ultra64.h>
#include <stddef.h>
#include <string.h>
#include "ge_native_platform.h"
extern int geNativeRunGraphics(void *commands, unsigned bytes);
extern int GEAudio_IsReady(void);
typedef char GE_TASK_BYTES[(sizeof(OSTask)==64)?1:-1];
typedef char GE_TASK_DATA_OFFSET[(offsetof(OSTask_t,data_ptr)==48)?1:-1];
typedef char GE_COMMAND_BYTES[(sizeof(Gfx)==8)?1:-1];

int geNativeTaskGraphics(void *opaque) {
    OSTask *task=(OSTask *)opaque;
    if(!task || task->t.type!=M_GFXTASK || !task->t.data_ptr ||
       !task->t.data_size || (task->t.data_size & 7u)) {
        GENativePlatform_SetError("Invalid original graphics task"); return 0;
    }
    return geNativeRunGraphics(geNativeAddressToPointer((uintptr_t)task->t.data_ptr),task->t.data_size);
}
int geNativeTaskAudio(void *opaque) {
    OSTask *task=(OSTask *)opaque;
    /* PORT libaudio's a* macros execute the actual CPU mixer while
       alAudioFrame builds the task. Completion only acknowledges that
       finished synthesis; running the mixer twice would corrupt its state. */
    return task && task->t.type==M_AUDTASK && GEAudio_IsReady();
}

/* Reserved host-render command. Both payload words live in the original
 * frame's display-list arena, so the scheduler never reads stack storage. */
Gfx *geNativeLogoEmit(Gfx *dl,float radians,float scale,float alpha) {
    u32 r,s;
    unsigned a;
    if(alpha<0.0f)alpha=0.0f;if(alpha>1.0f)alpha=1.0f;
    a=(unsigned)(alpha*255.0f+0.5f);
    memcpy(&r,&radians,4);memcpy(&s,&scale,4);
    dl[0].words.w0=0x48000000u|a;dl[0].words.w1=r;
    dl[1].words.w0=s;dl[1].words.w1=0;
    return dl+2;
}
