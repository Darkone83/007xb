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
};
#endif
