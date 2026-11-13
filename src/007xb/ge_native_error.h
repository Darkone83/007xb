#ifndef GE_NATIVE_ERROR_H
#define GE_NATIVE_ERROR_H
#include "xb_render.h"
/* Keep long resource/compiler diagnostics readable on a 640-pixel output. */
static void geNativeDrawErrorLines(XBRenderer *draw,IDirect3DDevice8 *device,
                                    const char *message,float top) {
    const char *text=message?message:"UNKNOWN ERROR";
    for(unsigned row=0;row<12u && *text;++row) {
        char line[65];unsigned n=0;
        while(n<64u && *text && *text!='\n' && *text!='\r')line[n++]=*text++;
        line[n]=0;
        if(*text=='\r')++text;
        if(*text=='\n')++text;
        draw->DrawText(device,24.0f,top+18.0f*(float)row,1.4f,line,0xFFE6EAF2u);
    }
}
#endif
