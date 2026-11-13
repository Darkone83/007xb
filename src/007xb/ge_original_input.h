#ifndef GE_ORIGINAL_INPUT_H
#define GE_ORIGINAL_INPUT_H
#include "ge_native_input.h"
/* Exact game-visible PR/os.h controller record layouts, Win32 C linkage.
   Xbox XInput data is converted in the existing XBE input loop.  The
   original joy.c calls these osCont* entry points once it is linked. */
#ifdef __cplusplus
extern "C" {
#endif
/* N64 PR/os.h calls this member "errno", but the Xbox/MSVC CRT may
   define errno as a function-like macro. Use error_code here instead.
   Field order, offsets and ABI sizes remain identical. */
struct GEOSContStatus {unsigned short type;unsigned char status,error_code;};
struct GEOSContPad {unsigned short button;signed char stick_x,stick_y;unsigned char error_code;};

enum {
    GE_CONT_A=0x8000,GE_CONT_B=0x4000,GE_CONT_Z=0x2000,
    GE_CONT_START=0x1000,GE_CONT_UP=0x0800,GE_CONT_DOWN=0x0400,
    GE_CONT_LEFT=0x0200,GE_CONT_RIGHT=0x0100,GE_CONT_L=0x0020,
    GE_CONT_R=0x0010,GE_CONT_CUP=0x0008,GE_CONT_CDOWN=0x0004,
    GE_CONT_CLEFT=0x0002,GE_CONT_CRIGHT=0x0001
};
void GEGameInput_Reset(void);
void GEGameInput_Update(unsigned port,int connected,unsigned short buttons,
                         int stickX,int stickY);
int osContInit(void* msgQ,unsigned char* mask,struct GEOSContStatus* status);
int osContReset(void* msgQ,struct GEOSContStatus* status);
int osContStartReadData(void* msgQ);
int osContStartQuery(void* msgQ);
void osContGetReadData(struct GEOSContPad* pads);
void osContGetQuery(struct GEOSContStatus* status);
#ifdef __cplusplus
}
#endif
#endif
