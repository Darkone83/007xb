#ifndef XB_GE_FAST3D_H
#define XB_GE_FAST3D_H

/* Shared C/C++ performance-counter ABI. Keep this header paired with the backend. */
#define GE_XBOX_BACKEND_STATS_ABI 2

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
    unsigned draw_api_us;              /* CPU in api_draw, INCLUDING nested flushes */
    unsigned submit_us;                /* CPU in flushNativeBatch (GPU submission+states) */
    unsigned upload_api_us;            /* CPU in texture upload (including prior batch barrier) */
    unsigned fast_swizzles;            /* uploads using XGSwizzleRect on this frame */
} GEBackendFrameStats;
void ge_xbox_d3d8_attach(void* xbox_device,unsigned width,unsigned height);
void ge_xbox_d3d8_detach(void);
unsigned ge_xbox_d3d8_unsupported(void);
unsigned ge_xbox_d3d8_drawn_triangles(void);
unsigned ge_xbox_d3d8_texture_bytes(void);
void ge_xbox_d3d8_frame_stats(GEBackendFrameStats* out);
/* Explicit boundary for external D3D8 draws embedded in a Fast3D list
 * (e.g. the custom Darkone83 lines/cel logo). */
void ge_xbox_d3d8_external_begin(void);
void ge_xbox_d3d8_external_end(void);
unsigned ge_xbox_d3d8_atlas_rows(void);
unsigned ge_xbox_d3d8_atlas_builds(void);
unsigned ge_xbox_d3d8_stream_vb_draws(void);
unsigned ge_xbox_d3d8_stream_up_draws(void);
extern struct GfxRenderingAPI gfx_xbox_d3d8_api;
#ifdef __cplusplus
}
#endif
#endif
