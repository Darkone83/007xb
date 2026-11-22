#ifndef GFX_RENDERING_API_H
#define GFX_RENDERING_API_H
/* GoldenEye PC port GfxRenderingAPI, same order and signatures as upstream. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct ShaderProgram;
struct GfxClipParameters { bool z_is_from_0_to_1; bool invert_y; };
enum FilteringMode { FILTER_NONE, FILTER_LINEAR, FILTER_THREE_POINT, FILTER_TRILINEAR };
enum MipmapFilteringMode { MIPMAP_DISABLED, MIPMAP_NEAREST, MIPMAP_LINEAR };

/* Native object-space transfer. Resident rooms keep immutable geometry on
 * Xbox; transient model runs carry short-lived unique vertices plus ordered
 * indices. GPU allocations remain exclusively on the rendering thread. */
struct GfxNativeRoomVertex {
    float x,y,z;
    float u0,v0;
    uint32_t color;   /* dynamic source input #1: RGB|alpha, packed ARGB */
    float u1,v1;      /* native second GPU texture stage (two-cycle material) */
    uint32_t color1;  /* dynamic source input #2: RGB|alpha, packed ARGB */
    float fog;        /* G_VTX-time RSP fog factor, 0..255, dynamic only */
};
struct GfxNativeRoomDraw {
    unsigned room,revision,signature;
    const void* command_address;
    const struct GfxNativeRoomVertex* vertices; /* null on draw probe */
    unsigned vertex_count;
    float matrix[16]; /* original RSP modelview-projection, row-vector order */
    float aspect_offset,aspect_scale;
    unsigned cull_mode, texture_units;
    float min_bound[3],max_bound[3]; /* object-space conservative AABB */
    /* For transient native runs only: preserve N64 triangle order using an
     * original-vertex-slot index stream. Static room packet owners ignore
     * these trailing fields. 0/NULL denote old non-indexed form. */
    const uint16_t* indices;
    unsigned unique_vertex_count;
    /* Phase 9B: authoritative Fast3D source texture IDs for TEXEL0/TEXEL1.
     * UINT32_MAX denotes an unused stage. A GPU packet must never silently
     * sample a different backend-selected texture after state invalidation. */
    uint32_t texture_ids[2];
    /* Fog does NOT use D3D's fixed fog unit: the native NV2A final
     * combiner reads specular alpha and D3DRS_FOGCOLOR directly. */
    uint32_t fog_color;
    unsigned fog_enabled;
    /* Conservative per-vertex proof: UVs stay inside the RDP tile clamp /
     * wrap window and the physical image without crossing a wrap seam.
     * Native backend may remap padded image coordinates only in that case. */
    unsigned uv_address_safe;
};
/* Direct typed handoff for the CPU fallback when the combiner has at most
 * two constant/RSP color inputs, no fog, no grayscale and no shader tile
 * clamps. Unlike the legacy 32-float stream, each input is a packed N64 RGBA
 * value. Per-vertex transforms, clipping and combiner mapping still run in
 * the existing Xbox draw pipeline, preserving draw order and material state.
 * The RSP never keeps these pointers after a synchronous callback returns. */
struct GfxCompactVertex {
    float x,y,z,w;                   /* existing clip-space coordinates */
    float u0,v0,u1,v1;               /* already normalized per texture */
    uint32_t rgba[2];               /* the first two Fast3D combiner inputs */
};
struct GfxRenderingAPI {
    const char* (*get_name)(void);
    int (*get_max_texture_size)(void);
    struct GfxClipParameters (*get_clip_parameters)(void);
    void (*unload_shader)(struct ShaderProgram*);
    void (*load_shader)(struct ShaderProgram*);
    struct ShaderProgram* (*create_and_load_new_shader)(uint64_t,uint32_t);
    struct ShaderProgram* (*lookup_shader)(uint64_t,uint32_t);
    void (*shader_get_info)(struct ShaderProgram*,uint8_t*,bool[2]);
    void (*clear_shaders)(void);
    uint32_t (*new_texture)(void);
    void (*select_texture)(int,uint32_t,bool);
    void (*upload_texture)(const uint8_t*,uint32_t,uint32_t,bool);
    void (*set_sampler_parameters)(int,bool,uint32_t,uint32_t,bool);
    void (*set_depth_mode)(bool,bool,bool,bool,uint16_t);
    void (*set_depth_range)(float,float);
    void (*set_viewport)(int,int,int,int);
    void (*set_scissor)(int,int,int,int);
    void (*set_use_alpha)(bool,bool);
    void (*draw_triangles)(float[],size_t,size_t);
    void (*init)(void);
    void (*on_resize)(void);
    void (*start_frame)(void);
    void (*end_frame)(void);
    void (*finish_render)(void);
    int (*create_framebuffer)();
    void (*update_framebuffer_parameters)(int,uint32_t,uint32_t,uint32_t,bool,bool,bool,bool);
    bool (*start_draw_to_framebuffer)(int,float);
    void (*copy_framebuffer)(int,int,int,int,bool,bool);
    void (*clear_framebuffer)(bool,bool);
    void (*resolve_msaa_color_buffer)(int,int);
    void* (*get_framebuffer_texture_id)(int);
    void (*select_texture_fb)(int);
    void (*delete_texture)(uint32_t);
    void (*set_texture_filter)(enum FilteringMode);
    enum FilteringMode (*get_texture_filter)(void);
    void (*set_mipmap_filter)(enum MipmapFilteringMode);
    void (*set_anisotropy_level)(int);
    int (*get_max_anisotropy_level)(void);
    /* Optional: GPU-resident original-room packets. Creation returns false
     * and never draws; a later draw call returns true only if submitted. */
    bool (*native_room_draw)(const struct GfxNativeRoomDraw*);
    bool (*native_room_prepare)(const struct GfxNativeRoomDraw*);
    /* Phase 7: transient, nonresident object-space triangle runs.  The Xbox
     * backend may reject BEFORE drawing; frontend then runs original TRI
     * commands. Materials, RDP state, vertex lifetime and draw order stay
     * with Fast3D. This uses a frame-streamed hardware VB, NOT a room cache. */
    bool (*native_stream_draw)(const struct GfxNativeRoomDraw*);
    /* Optional, appended to preserve the established rendering ABI. */
    void (*draw_compact_triangles)(const struct GfxCompactVertex*,size_t);
};
#endif
