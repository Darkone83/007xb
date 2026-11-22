#ifndef GE_NATIVE_INPUT_H
#define GE_NATIVE_INPUT_H

/* Native Xbox controls travel beside the six-byte N64 controller record.
 * Keep this header independent of the libultra controller declarations so
 * engine C files and the Xbox C++ bridge can share the same 32-bit fields. */
#ifdef __cplusplus
extern "C" {
#endif

struct GEGameXboxState {
    unsigned buttons,pressed;
    int move_x,move_y,look_x,look_y;
    int connected;
};
enum {
    GE_XBOX_A=1u<<0, GE_XBOX_B=1u<<1, GE_XBOX_X=1u<<2, GE_XBOX_Y=1u<<3,
    GE_XBOX_BLACK=1u<<4, GE_XBOX_WHITE=1u<<5,
    GE_XBOX_LT=1u<<6, GE_XBOX_RT=1u<<7,
    GE_XBOX_START=1u<<8, GE_XBOX_BACK=1u<<9,
    GE_XBOX_LTHUMB=1u<<10, GE_XBOX_RTHUMB=1u<<11,
    GE_XBOX_UP=1u<<12, GE_XBOX_DOWN=1u<<13,
    GE_XBOX_LEFT=1u<<14, GE_XBOX_RIGHT=1u<<15
};

/* Publish both input views under one lock. Stick axes use -80..80, with
 * up positive. ReadXbox consumes latched press edges once per game tick. */
void GEGameInput_UpdateXbox(unsigned port,int connected,unsigned short buttons,
                            int stickX,int stickY,const struct GEGameXboxState*);
int GEGameInput_ReadXbox(unsigned port,struct GEGameXboxState*);
void GEGameInput_ClearXboxEdges(unsigned port);
/* Host thread: captures physical input, releases its lock, then publishes LAN. */
void GEGameInput_PublishLanPhysical(void);

#ifdef __cplusplus
}
#endif
#endif
