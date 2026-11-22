#ifndef XB_GE_FAST3D_H
#define XB_GE_FAST3D_H

/* Shared C/C++ performance-counter ABI. Keep this header paired with the backend. */
#define GE_XBOX_BACKEND_STATS_ABI 3

/* The actual GoldenEye fast3d rendering API; no alternate graphics protocol. */
#include "gfx_rendering_api.h"

#ifdef __cplusplus
extern "C" {
#endif
/* Work submitted during the current original-engine frame. Upload bytes are
 * physical swizzled storage written, including the one-time noise image. */
typedef struct GEBackendFrameStats {
    unsigned draw_calls;
    unsigned gpu_batches;
    unsigned input_triangles;
    unsigned output_triangles;
    unsigned texture_uploads;
    unsigned texture_upload_bytes;
    unsigned shader_creates;
    unsigned shader_probes;               /* O(1) material cache probes */
    unsigned software_address_triangles;  /* CPU NPOT wrap/mirror candidates */
    unsigned screen_rejects;              /* trivially invisible triangles */
    unsigned vb_locks;                    /* potentially blocking VB locks */
    unsigned small_up_submissions;        /* sub-threshold batches */
    unsigned vb_submissions;              /* streamed draws */
    unsigned up_submissions;              /* UP fallback + small draws */
    /* Native-only counters identify the submission path used by cutscenes;
     * all are per-frame and do not include the original XYZRHW UP path. */
    unsigned native_up_submissions;        /* GPU-transformed XYZ via UP */
    unsigned native_indexed_submissions;   /* dynamic VB+IB: two locks/draw */
    unsigned native_stream_vb_submissions; /* expanded native XYZ stream VB */
    unsigned native_static_submissions;    /* resident room VB/IB reuse */
    unsigned draw_api_us;              /* CPU in api_draw, INCLUDING nested flushes */
    unsigned submit_us;                /* CPU in flushNativeBatch (GPU submission+states) */
    unsigned upload_api_us;            /* CPU in texture upload (including prior batch barrier) */
    unsigned fast_swizzles;            /* uploads using XGSwizzleRect on this frame */
} GEBackendFrameStats;
void ge_xbox_d3d8_attach(void* xbox_device,unsigned width,unsigned height);
void ge_xbox_d3d8_detach(void);
unsigned ge_xbox_d3d8_unsupported(void);
unsigned ge_xbox_d3d8_drawn_triangles(void);
unsigned ge_xbox_d3d8_room_hardware_triangles(void);
/* Cumulative submitted hardware-room triangles (for 120-frame stats windows). */
unsigned ge_xbox_d3d8_room_hardware_triangles_total(void);
/* Cumulative transient native-triangle count; also recorded by Back overlay. */
unsigned ge_xbox_d3d8_dynamic_hardware_triangles_total(void);
/* Cumulative GPU triangles using alpha, 2-cycle or both texture stages. */
unsigned ge_xbox_d3d8_material_hardware_triangles_total(void);
unsigned ge_xbox_d3d8_texture_bytes(void);
void ge_xbox_d3d8_frame_stats(GEBackendFrameStats* out);
/* Explicit boundary for external D3D8 draws embedded in a Fast3D list
 * (e.g. the custom Darkone83 lines/cel logo). */
void ge_xbox_d3d8_external_begin(void);
void ge_xbox_d3d8_external_end(void);
/* Called from a task-owned GBI marker, never from the game/network thread. */
void ge_xbox_d3d8_lan_view(unsigned enabled);
/* Draw the 440x299 I8 gunbarrel backdrop as one cached GPU quad. */
bool ge_xbox_draw_gunbarrel_bg(const unsigned char* image,int xOffset,
                               unsigned topRGB,unsigned bottomRGB,
                               float x0,float x1,float y0,float y1);
unsigned ge_xbox_d3d8_atlas_rows(void);
unsigned ge_xbox_d3d8_atlas_builds(void);
unsigned ge_xbox_d3d8_stream_vb_draws(void);
unsigned ge_xbox_d3d8_stream_up_draws(void);
/* Cumulative GPU room-cache hits/draws and resident bytes, for A/B tests. */
void ge_xbox_d3d8_room_material_cache_stats(unsigned *hits,
                                            unsigned *draws,
                                            unsigned *residentBytes);
/* Invalidate all static-room GPU meshes on a stage allocator reset. */
void ge_xbox_d3d8_static_room_cache_reset(void);
extern struct GfxRenderingAPI gfx_xbox_d3d8_api;
#ifdef __cplusplus
}
#endif
#endif
