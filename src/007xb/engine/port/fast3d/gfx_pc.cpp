#define NOMINMAX

#include "floatvtx.h" /* D245 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>


#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>
#include "gbiex.h" /* GE's G_TRI4 + PD extension opcodes (see header) */

#if defined(GE_XBOX_NATIVE32) && (G_XBOX_MP_VI_CANVAS_EXT == G_FLOATVTX_EXT || G_XBOX_MP_VI_CANVAS_EXT == 0x46 || (G_XBOX_MP_VI_CANVAS_EXT >= 0x48 && G_XBOX_MP_VI_CANVAS_EXT <= 0x4B))
#error Xbox VI canvas opcode conflicts with an existing Fast3D extension
#endif

#include "platform.h"
#include "portaddr.h"   /* Stage B: arena windows */
#include "envflag.h"
#include "ge_native_platform.h"
#include "ge_native_buffer.h"
#include "ge_native_gfx.h"
#include "ge_phase0_profile.h"

#include "gfx_pc.h"
#include "gfx_cc.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"
#include "gfx_screen_config.h"

static float ge_gfx_round(float x) { return (float)(x>=0.0f?floor(x+0.5f):ceil(x-0.5f)); }
static bool ge_gfx_isfinite(float x) { return x==x && x<=3.402823466e38f && x>=-3.402823466e38f; }
uintptr_t gfxFramebuffer;

#define ALIGN(x, a) (((x) + (a - 1)) & ~(a - 1))

#define SUPPORT_CHECK(x) assert(x)

// SCALE_M_N: upscale/downscale M-bit integer to N-bit
#define SCALE_5_8(VAL_) (((VAL_)*0xFF) / 0x1F)
#define SCALE_8_5(VAL_) ((((VAL_) + 4) * 0x1F) / 0xFF)
#define SCALE_4_8(VAL_) ((VAL_)*0x11)
#define SCALE_8_4(VAL_) ((VAL_) / 0x11)
#define SCALE_3_8(VAL_) (((VAL_) << 5) | ((VAL_) << 2) | ((VAL_) >> 1))
#define SCALE_8_3(VAL_) ((VAL_) / 0x24)

// Native Xbox menus use a 440x330 VI canvas; gameplay uses 320x240.
// Carry this per graphics task (not by reading live game-thread globals).
#ifndef GE_XBOX_MP_PORTRAIT_FULL_VIEWPORT
#define GE_XBOX_MP_PORTRAIT_FULL_VIEWPORT 1
#endif
#ifndef GE_XBOX_MP_VI_CANVAS_FIX
#define GE_XBOX_MP_VI_CANVAS_FIX 1
#endif
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_MP_VI_CANVAS_FIX
static uint32_t s_xboxLogicalWidth = SCREEN_WIDTH;
static uint32_t s_xboxLogicalHeight = SCREEN_HEIGHT;
#define GE_LOGICAL_WIDTH (s_xboxLogicalWidth)
#define GE_LOGICAL_HEIGHT (s_xboxLogicalHeight)
#else
#define GE_LOGICAL_WIDTH SCREEN_WIDTH
#define GE_LOGICAL_HEIGHT SCREEN_HEIGHT
#endif
#define HALF_SCREEN_WIDTH (GE_LOGICAL_WIDTH / 2.f)
#define HALF_SCREEN_HEIGHT (GE_LOGICAL_HEIGHT / 2.f)
#define RATIO_X (gfx_current_dimensions.width / (float)GE_LOGICAL_WIDTH)
#define RATIO_Y (gfx_current_dimensions.height / (float)GE_LOGICAL_HEIGHT)

#define MAX_BUFFERED 256
#define MAX_LIGHTS 4
#define MAX_VERTICES 128
#define MAX_VERTEX_COLORS 64

#if defined(GE_XBOX_NATIVE32)
/* Keep the front-end and RXDK backend slot capacity IDENTICAL. See the
 * backend's GE_NV2A_TEXTURE_CACHE_SLOTS A/B switch. GPU bytes stay capped. */
#ifndef GE_NV2A_TEXTURE_CACHE_SLOTS
#define GE_NV2A_TEXTURE_CACHE_SLOTS 1024
#endif
#if GE_NV2A_TEXTURE_CACHE_SLOTS != 512 && GE_NV2A_TEXTURE_CACHE_SLOTS != 1024
#error GE_NV2A_TEXTURE_CACHE_SLOTS must be 512 or 1024
#endif
#define TEXTURE_CACHE_MAX_SIZE GE_NV2A_TEXTURE_CACHE_SLOTS
#else
#define TEXTURE_CACHE_MAX_SIZE 256
#endif

/* Phase 5D: keep the original hash/key/eviction semantics while removing the
 * O(allocated texture slots) scan on every texture-cache MISS. A fixed free
 * bitmap chooses the same lowest-index unused slot as the original scan, and
 * an intrusive LRU list gives the same oldest non-pinned victim. No new GPU
 * texture storage or dynamic memory is allocated. 0 restores the old scan. */
#ifndef GE_XBOX_FAST3D_TEXTURE_LRU_O1
#if defined(GE_XBOX_NATIVE32)
#define GE_XBOX_FAST3D_TEXTURE_LRU_O1 1
#else
#define GE_XBOX_FAST3D_TEXTURE_LRU_O1 0
#endif
#endif

#define C0(pos, width) ((cmd->words.w0 >> (pos)) & ((1U << width) - 1))
#define C1(pos, width) ((cmd->words.w1 >> (pos)) & ((1U << width) - 1))

struct RGBA {
    uint8_t r, g, b, a;
};

struct NormalColor {
    union {
        struct { uint8_t r, g, b, a; };
        struct { int8_t x, y, z, w; };
    };
};

/* D540: set by the GL backend at init. gfx_rdp_affine: RDP-style
 * screen-affine shade colour. gfx_fog_vertex: 1 = per-vertex RSP fog
 * (default, D543), 0 = the D540 exact per-pixel fog (GE_FOGPIXEL=1). */
extern "C" {
int gfx_rdp_affine = 0;
int gfx_fog_vertex = 0;
}

#ifndef GE_XBOX_NATIVE_ROOM_BATCHES
#define GE_XBOX_NATIVE_ROOM_BATCHES 1
#endif
/* Phase 6C: GPU homogeneous clipping is appropriate for the restricted
 * unfogged, opaque room subset. 0 restores the conservative 6B whole-AABB
 * near/far test. Hardware clip is never enabled for effects requiring the
 * original RSP's specially interpolated fog/shade behavior. */
#ifndef GE_XBOX_ROOM_GPU_CLIP
#define GE_XBOX_ROOM_GPU_CLIP 0
#endif
/* Cold GPU packet creation can stall visible frames. Keep the original
 * one-upload-per-frame default even though the packet table is larger. */
#ifndef GE_XBOX_ROOM_UPLOADS_PER_FRAME
#define GE_XBOX_ROOM_UPLOADS_PER_FRAME 1u
#endif
/* Frontend and backend must use the same metadata/packet capacity. */
#ifndef GE_XBOX_ROOM_PACKET_RECORDS
#define GE_XBOX_ROOM_PACKET_RECORDS 128u
#endif
#if GE_XBOX_ROOM_PACKET_RECORDS != 48 && GE_XBOX_ROOM_PACKET_RECORDS != 128
#error GE_XBOX_ROOM_PACKET_RECORDS must be 48 or 128
#endif
/* In the CURRENT port G_COL stores a vertex_colors pointer, but the
 * G_VTX processing path reads colors directly from its source Vtx and
 * nowhere reads that pointer. Do not invalidate the entire room when the
 * no-op table pointer is set. 0 restores 6B's conservative exclusion. */
#ifndef GE_XBOX_ROOM_ALLOW_GCOL
#define GE_XBOX_ROOM_ALLOW_GCOL 0
#endif
/* These snapshots are written on G_VTX, then read ONLY inside an original
 * background-room display-list scope. No GPU resources belong to this side. */
struct NativeRoomRawSlot {
    float x,y,z;
    uint32_t materialColor;
    unsigned room,revision,matrixStamp;
    bool valid;
};
static NativeRoomRawSlot s_roomRawSlot[MAX_VERTICES];
static int s_roomScope=-1;
static unsigned s_roomRevision=0;
static bool s_roomHasColorOverride=false;
static uintptr_t s_roomPrimaryStart=0u;
static uintptr_t s_roomPrimaryEnd=0u;
static const Gfx* s_roomAttemptedStart=0;
static const Gfx* s_roomAttemptedUntil=0;
static uint32_t geRoomMix32(uint32_t h,uint32_t v) {
    return (h ^ v)*16777619u;
}
struct LoadedVertex {
    float x, y, z, w;
    float u, v;
    struct RGBA color;
    uint8_t fog;
    uint8_t clip_rej;
    float fog_n;   /* D540: fog sent to the GPU (0..255): the RSP value (default), or fog * w (GE_FOGPIXEL) */
};

static TextureCacheNode gfx_texture_cache[TEXTURE_CACHE_MAX_SIZE];
static unsigned gfx_texture_cache_used = 0;
static uint64_t gfx_texture_cache_clock = 0;
/* Cumulative, stage-transition-only measurements. No disk writes in hot path.
 * An eviction is the only situation where more slots can reduce texture
 * decode/upload work. A high miss count without evictions needs other work. */
static unsigned s_texCacheHits=0,s_texCacheMisses=0,s_texCacheEvictions=0;
static unsigned s_texCachePeakAllocated=0;
extern "C" void geNativeFast3DTextureCacheCounters(unsigned* capacity,
        unsigned* peak,unsigned* hits,unsigned* misses,unsigned* evictions) {
    if(capacity)*capacity=TEXTURE_CACHE_MAX_SIZE;
    if(peak)*peak=s_texCachePeakAllocated;
    if(hits)*hits=s_texCacheHits;
    if(misses)*misses=s_texCacheMisses;
    if(evictions)*evictions=s_texCacheEvictions;
}
/* Fixed storage keeps the old RXDK build free of STL allocations. Indices
 * are one-based so the zero-initialized table is already an empty cache.
 * Warm gunbarrel frames used to scan about 45,000 entries for 299 hits. */
#define TEXTURE_CACHE_BUCKETS (TEXTURE_CACHE_MAX_SIZE * 2)
static uint16_t gfx_texture_buckets[TEXTURE_CACHE_BUCKETS];
static uint16_t gfx_texture_next[TEXTURE_CACHE_MAX_SIZE];

#if GE_XBOX_FAST3D_TEXTURE_LRU_O1
/* Cache IDs are 1-based in both the hash chains and LRU list; zero is null.
 * The free bitmap uses physical slot indices so reinsertion chooses the same
 * lowest unused slot as the original linear scan, even after a cache clear. */
static uint16_t gfx_texture_lru_prev[TEXTURE_CACHE_MAX_SIZE];
static uint16_t gfx_texture_lru_next[TEXTURE_CACHE_MAX_SIZE];
static uint16_t gfx_texture_lru_oldest=0,gfx_texture_lru_newest=0;
static uint32_t gfx_texture_unused[(TEXTURE_CACHE_MAX_SIZE+31u)/32u];

static void gfx_texture_lru_unlink(unsigned index) {
    const uint16_t prev=gfx_texture_lru_prev[index];
    const uint16_t next=gfx_texture_lru_next[index];
    if(prev)gfx_texture_lru_next[prev-1u]=next;
    else gfx_texture_lru_oldest=next;
    if(next)gfx_texture_lru_prev[next-1u]=prev;
    else gfx_texture_lru_newest=prev;
    gfx_texture_lru_prev[index]=gfx_texture_lru_next[index]=0;
}
static void gfx_texture_lru_append(unsigned index) {
    const uint16_t id=(uint16_t)(index+1u);
    gfx_texture_lru_prev[index]=gfx_texture_lru_newest;
    gfx_texture_lru_next[index]=0;
    if(gfx_texture_lru_newest)
        gfx_texture_lru_next[gfx_texture_lru_newest-1u]=id;
    else gfx_texture_lru_oldest=id;
    gfx_texture_lru_newest=id;
}
static void gfx_texture_lru_touch(unsigned index) {
    if(gfx_texture_lru_newest==(uint16_t)(index+1u))return;
    gfx_texture_lru_unlink(index);
    gfx_texture_lru_append(index);
}
static void gfx_texture_lru_invalidate(unsigned index) {
    gfx_texture_lru_unlink(index);
    gfx_texture_unused[index>>5]|=(uint32_t)1u<<(index&31u);
}
static TextureCacheNode* gfx_texture_take_unused() {
    for(unsigned word=0;word<sizeof(gfx_texture_unused)/sizeof(gfx_texture_unused[0]);++word) {
        uint32_t bits=gfx_texture_unused[word];
        if(!bits)continue;
        unsigned bit=0;
        while(!(bits&1u)){bits>>=1;++bit;}
        gfx_texture_unused[word]&=~((uint32_t)1u<<bit);
        return &gfx_texture_cache[word*32u+bit];
    }
    return 0;
}
static TextureCacheNode* gfx_texture_oldest_except(const TextureCacheNode* pinned) {
    for(uint16_t id=gfx_texture_lru_oldest;id;id=gfx_texture_lru_next[id-1u]) {
        TextureCacheNode* node=&gfx_texture_cache[id-1u];
        if(node!=pinned)return node;
    }
    return 0;
}
static void gfx_texture_lru_clear() {
    gfx_texture_lru_oldest=gfx_texture_lru_newest=0;
    memset(gfx_texture_unused,0,sizeof(gfx_texture_unused));
    for(unsigned i=0;i<gfx_texture_cache_used;++i)
        gfx_texture_unused[i>>5]|=(uint32_t)1u<<(i&31u);
}
#endif

static unsigned gfx_texture_bucket(const TextureCacheKey& key) {
    uintptr_t h=(uintptr_t)key.texture_addr;
    h^=h>>5;
    h^=(uintptr_t)key.palette_addrs[0]>>4;
    h^=(uintptr_t)key.palette_addrs[1]>>7;
    h^=(uintptr_t)key.size_bytes*0x9e3779b1u;
    h^=(uintptr_t)key.palette_hash;
    h^=(unsigned)key.fmt<<7;
    h^=(unsigned)key.siz<<4;
    h^=(unsigned)key.palette_index;
    return (unsigned)h & (TEXTURE_CACHE_BUCKETS-1u);
}

static void gfx_texture_unlink(TextureCacheNode* node) {
    const unsigned index=(unsigned)(node-gfx_texture_cache);
    uint16_t* link=&gfx_texture_buckets[gfx_texture_bucket(node->first)];
    while(*link && *link!=index+1u)link=&gfx_texture_next[*link-1u];
    if(*link)*link=gfx_texture_next[index];
    gfx_texture_next[index]=0;
}

struct ColorCombiner {
    uint64_t shader_id0;
    uint32_t shader_id1;
    bool used_textures[2];
    struct ShaderProgram* prg[16];
    uint8_t shader_input_mapping[2][7];
};

struct NativeCombinerEntry { ColorCombinerKey first; ColorCombiner second; };
static NativeCombinerEntry color_combiner_pool[512];
static unsigned color_combiner_count = 0;
static NativeCombinerEntry* prev_combiner = 0;
/* One-based fixed chains preserve stable combiner/shader pointers without
 * scanning every material seen since the last renderer reset. */
#define COLOR_COMBINER_BUCKETS 1024
static uint16_t color_combiner_buckets[COLOR_COMBINER_BUCKETS];
static uint16_t color_combiner_next[512];
static unsigned gfx_color_combiner_bucket(const ColorCombinerKey& key) {
    uint32_t h=(uint32_t)key.combine_mode ^ (uint32_t)(key.combine_mode>>32);
    h^=(uint32_t)key.options*0x9e3779b1u;
    h^=(uint32_t)(key.options>>32);
    h^=h>>16;h*=0x7feb352du;h^=h>>15;
    return h & (COLOR_COMBINER_BUCKETS-1u);
}

static uint8_t* tex_upload_buffer = nullptr;

static struct RSP {
    float modelview_matrix_stack[11][4][4];
    uint8_t modelview_matrix_stack_size;

    float MP_matrix[4][4];
    float P_matrix[4][4];

    Light_t lookat[2];
    bool lookat_enabled;

    Light_t current_lights[MAX_LIGHTS + 1];
    float current_lights_coeffs[MAX_LIGHTS][3];
    float current_lookat_coeffs[2][3]; // lookat_x, lookat_y
    uint8_t current_num_lights;        // includes ambient light
    bool lights_changed;

    uint32_t geometry_mode;
    int16_t fog_mul, fog_offset;

    uint32_t extra_geometry_mode;

    uint32_t aspect_mode;
    float aspect_ofs;
    float aspect_scale;

    struct {
        // U0.16
        uint16_t s, t;
    } texture_scaling_factor;

    struct LoadedVertex loaded_vertices[MAX_VERTICES + 4];

    const struct NormalColor *vertex_colors; //[MAX_VERTEX_COLORS];
} rsp;

static unsigned geRoomMatrixStamp() {
    uint32_t h=2166136261u;
    const uint8_t *bytes=(const uint8_t*)rsp.MP_matrix;
    for(unsigned n=0;n<sizeof(rsp.MP_matrix);++n)h=geRoomMix32(h,bytes[n]);
    return h;
}


struct RawTexMetadata {
    uint16_t width, height;
    float h_byte_scale = 1, v_pixel_scale = 1;
};

struct LoadedTexture {
    const uint8_t* addr;
    uint32_t orig_size_bytes;
    uint32_t full_size_bytes; // full_image_line_size_bytes * height
    uint32_t size_bytes; // line_size_bytes * height
    uint32_t full_image_line_size_bytes;
    uint32_t line_size_bytes;
    uint32_t tex_flags;
    struct RawTexMetadata raw_tex_metadata;
    /* D229: G_IM_FMT_* of the gDPSetTextureImage active when this slot was
     * last written by a load command (0xFF = never / unknown). Used to tell
     * a CI8 index stream apart from real RGBA16 pixels when a later tile
     * re-declares the same TMEM in another format -- see import_texture. */
    uint8_t src_fmt = 0xFF;
    /* D75: set by gfx_dp_load_block when the load used dxt == 0. The RDP's
     * LoadBlock only swaps odd rows on the way INTO TMEM when its dxt counter
     * advances; TMEM reads always swap odd rows (addr ^ 4). With dxt == 0 the
     * load-side swap never happens, so the sampled image is the memory image
     * with the two 32-bit halves of every 8-byte group swapped on odd rows.
     * Rare relies on this for the Rareware logo's text mip textures. */
    bool dxt0 = false;
};

static struct RDP {
    uint16_t palette[256];
    const uint8_t* palette_addrs[2];
    uint32_t palette_fmt;
    uint32_t palette_hash; /* D217: exact FNV-1a, refreshed on first CI8 import after a load */
    bool palette_hash_dirty;
    uint32_t palette_bank_hash[16];
    uint16_t palette_bank_hash_valid;
    struct {
        const uint8_t* addr;
        uint8_t fmt; /* D229: was dropped before; needed to track CI sources */
        uint8_t siz;
        uint32_t width;
        uint32_t tex_flags;
        struct RawTexMetadata raw_tex_metadata;
    } texture_to_load;
    struct {
        uint8_t fmt;
        uint8_t siz;
        uint8_t cms, cmt;
        uint8_t masks, maskt; /* RC3: N64 tile mask; wrap period = 1<<mask */
        uint8_t shifts, shiftt;
        uint16_t uls, ult, lrs, lrt; // U10.2
        uint16_t width, height;      // in texels
        uint16_t tmem;               // 0-511, in 64-bit word units
        uint32_t line_size_bytes;
        uint8_t palette;
    } texture_tile[8];
    LoadedTexture loaded_texture[512]; // for each tmem location
    bool textures_changed[2];

    uint8_t first_tile_index;
    uint8_t tex_min_lod;
    uint8_t tex_max_lod;

    uint32_t other_mode_l, other_mode_h;
    uint64_t combine_mode;
    bool grayscale;
    bool tex_lod;
    bool tex_detail;

    uint8_t prim_lod_fraction;
    struct RGBA env_color, prim_color, fog_color, fill_color, grayscale_color;
    struct XYWidthHeight viewport, scissor;
    bool viewport_or_scissor_changed;
    void* z_buf_address;
    void* color_image_address;

    int16_t subpixel_ofs_x;
    int16_t subpixel_ofs_y;
} rdp;

static struct RenderingState {
    uint8_t depth_mode;
    bool alpha_blend;
    bool modulate;
    struct XYWidthHeight viewport, scissor;
    struct ShaderProgram* shader_program;
    TextureCacheNode* textures[SHADER_MAX_TEXTURES];
} rendering_state;

struct GfxDimensions gfx_current_window_dimensions;
int32_t gfx_current_window_position_x;
int32_t gfx_current_window_position_y;
struct GfxDimensions gfx_current_dimensions;
static struct GfxDimensions gfx_prev_dimensions;
struct XYWidthHeight gfx_current_game_window_viewport;
struct XYWidthHeight gfx_current_native_viewport;
float gfx_current_native_aspect = 4.f / 3.f;
bool gfx_framebuffers_enabled = true;
bool gfx_detail_textures_enabled = true;

static bool game_renders_to_framebuffer;
static int game_framebuffer;
static int game_framebuffer_msaa_resolved;

/* Safe-area (TV-overscan) crop. GE's own N64 game code insets its normal
 * single-player "Full" gameplay viewport a fixed margin from the true VI
 * framebuffer edges (src/fr.h: VIEWPORT_HEIGHT_DEFAULT_NTSC=220 of a
 * 240-line frame -- ~8% top+bottom; PAL's equivalent is already full-height,
 * no margin) and separately fills that margin with black rectangles
 * (src/fr.c viSetupScreensForNumPlayers). On a real CRT that margin falls
 * in the invisible overscan region; this port displays the full VI frame,
 * so the margin shows up as literal top/bottom black bars.
 *
 * g_gpSafeTop/g_gpSafeHeight cache the most recently set SP viewport's raw
 * (pre window-scale) Y bounds, in the same native-VI-Y units gfx_pc.cpp
 * uses elsewhere (see gfx_calc_and_set_viewport). gfx_adjust_viewport_or_
 * scissor remaps every screen-space Y (the 3D viewport itself AND RDP
 * fill-rect/texture-rect draws, which deliberately bypass the current SP
 * viewport and use the full VI canvas -- see gfx_draw_rectangle's
 * default_viewport) against this cached region instead of the full VI
 * canvas, so it fills the window edge to edge. Self-gating: front-end/menus
 * and PAL "Full" never inset their viewport (top=0, full height), so this
 * is a no-op there; -1 sentinel means "no viewport captured yet",
 * passthrough (identical to original behavior). */
static float g_gpSafeTop = 0.0f;
static float g_gpSafeHeight = -1.0f;
static bool g_safe_area_crop_enabled = true;

extern "C" void gfx_set_safe_area_crop(int on) {
    g_safe_area_crop_enabled = !!on;
}

/* D416: the crop above stretches the LAST set viewport's Y range over the
 * whole window. Correct for one full-screen viewport; in split-screen every
 * player's (half/quarter) viewport got stretched to fill the window, so the
 * last-drawn player covered the others (and the shuffled draw order made the
 * winner alternate frame to frame). While a 2+ player stage is running the
 * N64's own split layout is used unmodified. */
static bool g_split_screen = false;
/* D510: set while the port-layer overlay DL (F10 panel / FPS counter) runs.
 * That is PC chrome, not game content: it maps the logical canvas onto the
 * whole window, bypassing the safe-area crop (which follows the game's last
 * viewport and so differed between the front end and a level). */
static bool g_overlay_window_space = false;
extern "C" void gfx_set_split_screen(int on) {
    g_split_screen = !!on;
}

/* D447: output rect (Video.AspectMode = Original). 0 = fill the window (the
 * historical behaviour, bit-identical). >0 = letter/pillarbox the frame to
 * exactly this aspect, centred; set once per frame by the port layer before
 * gfx_start_frame. g_output_rect is top-left-origin window pixels. */
static float g_output_aspect = 0.0f;
/* Xbox SD pixels can be anamorphic. Use the display shape for camera/HUD
 * corrections while keeping real framebuffer dimensions for rasterization. */
static float s_nativeDisplayAspect = 0.0f;
static struct XYWidthHeight g_output_rect = { 0, 0, 0, 0 };

/* D509: the render worker applies a new output aspect at the start of its next
 * frame, but the game thread has already built the next one or two display lists
 * with the old projection (portNativeAspect reads the render side's last
 * aspect). Drawn into the new rect those frames are mis-projected for one or two
 * frames (visible as a flash, worst on the gun/hand model). Hold the previous
 * image for the frames that could carry the old aspect: gfx_run drops them through
 * the same path a minimised window uses. */
static int g_aspect_settle = 0;
#define ASPECT_SETTLE_FRAMES 2

extern "C" void gfx_set_output_aspect(float aspect) {
    const float a = aspect > 0.1f ? aspect : 0.0f;
    if (a != g_output_aspect) g_aspect_settle = ASPECT_SETTLE_FRAMES;
    g_output_aspect = a;
}

extern "C" void gfx_get_output_rect(int32_t *outX, int32_t *outY, int32_t *outW, int32_t *outH) {
    *outX = g_output_rect.x;
    *outY = g_output_rect.y;
    *outW = (int32_t)g_output_rect.width;
    *outH = (int32_t)g_output_rect.height;
}

uint32_t gfx_msaa_level = 1;

static bool dropped_frame;

static float buf_vbo[MAX_BUFFERED * (32 * 3)]; // 3 vertices in a triangle and 32 floats per vtx
static size_t buf_vbo_len;
static size_t buf_vbo_num_tris;

static struct GfxWindowManagerAPI* gfx_wapi;
static struct GfxRenderingAPI* gfx_rapi;

static uintptr_t segmentPointers[16];

struct FBInfo {
    uint32_t orig_width, orig_height;
    uint32_t applied_width, applied_height;
    bool upscale, autoresize;
};

static bool fbActive = 0;
struct NativeFramebufferEntry { int first; FBInfo second; bool valid; };
static NativeFramebufferEntry framebuffers[8];

static constexpr float clampf(const float x, const float min, const float max) {
    return (x < min) ? min : (x > max) ? max : x;
}

/* #92 perf probe (GE_PERFSTAT=1): per-frame counters, see gfx_run. */
static uint64_t s_perf_batches = 0, s_perf_tris = 0;

static void gfx_flush(void) {
    if (buf_vbo_len > 0) {
        GE_P0E_SCOPE(GE_P0E_SUBMIT);
        s_perf_batches++;
        s_perf_tris += buf_vbo_num_tris;
        gfx_rapi->draw_triangles(buf_vbo, buf_vbo_len, buf_vbo_num_tris);
        buf_vbo_len = 0;
        buf_vbo_num_tris = 0;
    }
}

/* Xbox combiner programs are created in memory by the RXDK backend.
 * The desktop GLSL disk cache is inapplicable to that backend. */
static bool s_shader_warm_pending = false;
static void gfx_shader_list_record(uint64_t, uint32_t) {}
static void gfx_shader_prewarm(void) { s_shader_warm_pending = false; }

static struct ShaderProgram* gfx_lookup_or_create_shader_program(uint64_t shader_id0, uint32_t shader_id1) {
    struct ShaderProgram* prg = gfx_rapi->lookup_shader(shader_id0, shader_id1);
    if (prg == NULL) {
        gfx_rapi->unload_shader(rendering_state.shader_program);
        prg = gfx_rapi->create_and_load_new_shader(shader_id0, shader_id1);
        rendering_state.shader_program = prg;
        gfx_shader_list_record(shader_id0, shader_id1);
    }
    return prg;
}

static const char* ccmux_to_string(uint32_t ccmux) {
    static const char* const tbl[] = {
        "G_CCMUX_COMBINED",
        "G_CCMUX_TEXEL0",
        "G_CCMUX_TEXEL1",
        "G_CCMUX_PRIMITIVE",
        "G_CCMUX_SHADE",
        "G_CCMUX_ENVIRONMENT",
        "G_CCMUX_1",
        "G_CCMUX_COMBINED_ALPHA",
        "G_CCMUX_TEXEL0_ALPHA",
        "G_CCMUX_TEXEL1_ALPHA",
        "G_CCMUX_PRIMITIVE_ALPHA",
        "G_CCMUX_SHADE_ALPHA",
        "G_CCMUX_ENV_ALPHA",
        "G_CCMUX_LOD_FRACTION",
        "G_CCMUX_PRIM_LOD_FRAC",
        "G_CCMUX_K5",
    };
    if (ccmux > 15) {
        return "G_CCMUX_0";

    } else {
        return tbl[ccmux];
    }
}

static const char* acmux_to_string(uint32_t acmux) {
    static const char* const tbl[] = {
        "G_ACMUX_COMBINED or G_ACMUX_LOD_FRACTION",
        "G_ACMUX_TEXEL0",
        "G_ACMUX_TEXEL1",
        "G_ACMUX_PRIMITIVE",
        "G_ACMUX_SHADE",
        "G_ACMUX_ENVIRONMENT",
        "G_ACMUX_1 or G_ACMUX_PRIM_LOD_FRAC",
        "G_ACMUX_0",
    };
    return tbl[acmux];
}

/* forward decl for the env-gated D75D probe (defined further down) */
static bool d75d_env_active(void);

static void gfx_generate_cc(struct ColorCombiner* comb, const ColorCombinerKey& key) {
    bool is_2cyc = (key.options & (uint64_t)SHADER_OPT_2CYC) != 0;

    uint8_t c[2][2][4] = { { { 0 } } };
    uint64_t shader_id0 = 0;
    uint32_t shader_id1 = key.options;
    uint8_t shader_input_mapping[2][7] = { { 0 } };
    bool used_textures[2] = { false, false };
    for (int i = 0; i < 2 && (i == 0 || is_2cyc); i++) {
        uint32_t rgb_a = (key.combine_mode >> (i * 28)) & 0xf;
        uint32_t rgb_b = (key.combine_mode >> (i * 28 + 4)) & 0xf;
        uint32_t rgb_c = (key.combine_mode >> (i * 28 + 8)) & 0x1f;
        uint32_t rgb_d = (key.combine_mode >> (i * 28 + 13)) & 7;
        uint32_t alpha_a = (key.combine_mode >> (i * 28 + 16)) & 7;
        uint32_t alpha_b = (key.combine_mode >> (i * 28 + 16 + 3)) & 7;
        uint32_t alpha_c = (key.combine_mode >> (i * 28 + 16 + 6)) & 7;
        uint32_t alpha_d = (key.combine_mode >> (i * 28 + 16 + 9)) & 7;

        if (rgb_a >= 8) {
            rgb_a = G_CCMUX_0;
        }
        if (rgb_b >= 8) {
            rgb_b = G_CCMUX_0;
        }
        if (rgb_c >= 16) {
            rgb_c = G_CCMUX_0;
        }
        if (rgb_d == 7) {
            rgb_d = G_CCMUX_0;
        }

        if (rgb_a == rgb_b || rgb_c == G_CCMUX_0) {
            // Normalize
            rgb_a = G_CCMUX_0;
            rgb_b = G_CCMUX_0;
            rgb_c = G_CCMUX_0;
        }
        if (alpha_a == alpha_b || alpha_c == G_ACMUX_0) {
            // Normalize
            alpha_a = G_ACMUX_0;
            alpha_b = G_ACMUX_0;
            alpha_c = G_ACMUX_0;
        }
        if (i == 1) {
            if (rgb_a != G_CCMUX_COMBINED && rgb_b != G_CCMUX_COMBINED && rgb_c != G_CCMUX_COMBINED &&
                rgb_d != G_CCMUX_COMBINED) {
                // First cycle RGB not used, so clear it away
                c[0][0][0] = c[0][0][1] = c[0][0][2] = c[0][0][3] = G_CCMUX_0;
            }
            if (rgb_c != G_CCMUX_COMBINED_ALPHA && alpha_a != G_ACMUX_COMBINED && alpha_b != G_ACMUX_COMBINED &&
                alpha_d != G_ACMUX_COMBINED) {
                // First cycle ALPHA not used, so clear it away
                c[0][1][0] = c[0][1][1] = c[0][1][2] = c[0][1][3] = G_ACMUX_0;
            }
        }

        c[i][0][0] = rgb_a;
        c[i][0][1] = rgb_b;
        c[i][0][2] = rgb_c;
        c[i][0][3] = rgb_d;
        c[i][1][0] = alpha_a;
        c[i][1][1] = alpha_b;
        c[i][1][2] = alpha_c;
        c[i][1][3] = alpha_d;
    }
    if (!is_2cyc) {
        for (int i = 0; i < 2; i++) {
            for (int k = 0; k < 4; k++) {
                c[1][i][k] = i == 0 ? G_CCMUX_0 : G_ACMUX_0;
            }
        }
    }
    {
        uint8_t input_number[32] = { 0 };
        int next_input_number = SHADER_INPUT_1;
        for (int i = 0; i < 2 && (i == 0 || is_2cyc); i++) {
            for (int j = 0; j < 4; j++) {
                uint32_t val = 0;
                switch (c[i][0][j]) {
                    case G_CCMUX_0:
                        val = SHADER_0;
                        break;
                    case G_CCMUX_1:
                        val = SHADER_1;
                        break;
                    case G_CCMUX_TEXEL0:
                        val = SHADER_TEXEL0;
                        used_textures[0] = true;
                        break;
                    case G_CCMUX_TEXEL1:
                        val = SHADER_TEXEL1;
                        used_textures[1] = true;
                        break;
                    case G_CCMUX_TEXEL0_ALPHA:
                        val = SHADER_TEXEL0A;
                        used_textures[0] = true;
                        break;
                    case G_CCMUX_TEXEL1_ALPHA:
                        val = SHADER_TEXEL1A;
                        used_textures[1] = true;
                        break;
                    case G_CCMUX_NOISE:
                        val = SHADER_NOISE;
                        break;
                    case G_CCMUX_PRIMITIVE:
                    case G_CCMUX_PRIMITIVE_ALPHA:
                    case G_CCMUX_PRIM_LOD_FRAC:
                    case G_CCMUX_SHADE:
                    case G_CCMUX_SHADE_ALPHA:
                    case G_CCMUX_ENVIRONMENT:
                    case G_CCMUX_ENV_ALPHA:
                    case G_CCMUX_LOD_FRACTION:
                        if (input_number[c[i][0][j]] == 0) {
                            shader_input_mapping[0][next_input_number - 1] = c[i][0][j];
                            input_number[c[i][0][j]] = next_input_number++;
                        }
                        val = input_number[c[i][0][j]];
                        break;
                    case G_CCMUX_COMBINED:
                        val = SHADER_COMBINED;
                        break;
                    default:
                        sysLogPrintf(LOG_WARNING, "Unsupported ccmux: %d", c[i][0][j]);
                        break;
                }
                shader_id0 |= (uint64_t)val << (i * 32 + j * 4);
            }
        }
    }
    /* TEMP D75D: one-shot decode of the logo model's combine mode (env-gated via d75d_lo) */
    {
        static int d75d_cc_once = 0;
        if (!d75d_cc_once && d75d_env_active() && key.combine_mode == 0x009ffe4f19ffe4f1ULL) {
            d75d_cc_once = 1;
            fprintf(stderr, "D75DCC: comb=0x%016llx is_2cyc=%d c0=(rgb a=%u b=%u c=%u d=%u)(al a=%u b=%u c=%u d=%u) utex0=%d\n",
                (unsigned long long)key.combine_mode, (int)is_2cyc,
                c[0][0][0], c[0][0][1], c[0][0][2], c[0][0][3],
                c[0][1][0], c[0][1][1], c[0][1][2], c[0][1][3], (int)used_textures[0]);
        }
    }
    {
        uint8_t input_number[16] = { 0 };
        int next_input_number = SHADER_INPUT_1;
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 4; j++) {
                uint32_t val = 0;
                switch (c[i][1][j]) {
                    case G_ACMUX_0:
                        val = SHADER_0;
                        break;
                    case G_ACMUX_TEXEL0:
                        val = SHADER_TEXEL0;
                        used_textures[0] = true;
                        break;
                    case G_ACMUX_TEXEL1:
                        val = SHADER_TEXEL1;
                        used_textures[1] = true;
                        break;
                    case G_ACMUX_LOD_FRACTION:
                        // case G_ACMUX_COMBINED: same numerical value
                        if (j != 2) {
                            val = SHADER_COMBINED;
                            break;
                        }
                        c[i][1][j] = G_CCMUX_LOD_FRACTION;
                        [[fallthrough]]; // for G_ACMUX_LOD_FRACTION
                    case G_ACMUX_1:
                        // case G_ACMUX_PRIM_LOD_FRAC: same numerical value
                        if (j != 2) {
                            val = SHADER_1;
                            break;
                        }
                        [[fallthrough]]; // for G_ACMUX_PRIM_LOD_FRAC
                    case G_ACMUX_PRIMITIVE:
                    case G_ACMUX_SHADE:
                    case G_ACMUX_ENVIRONMENT:
                        if (input_number[c[i][1][j]] == 0) {
                            shader_input_mapping[1][next_input_number - 1] = c[i][1][j];
                            input_number[c[i][1][j]] = next_input_number++;
                        }
                        val = input_number[c[i][1][j]];
                        break;
                }
                shader_id0 |= (uint64_t)val << (i * 32 + 16 + j * 4);
            }
        }
    }
    comb->shader_id0 = shader_id0;
    comb->shader_id1 = shader_id1;
    comb->used_textures[0] = used_textures[0];
    comb->used_textures[1] = used_textures[1];
    // comb->prg = gfx_lookup_or_create_shader_program(shader_id0, shader_id1);
    memcpy(comb->shader_input_mapping, shader_input_mapping, sizeof(shader_input_mapping));
}

static struct ColorCombiner* gfx_lookup_or_create_color_combiner(const ColorCombinerKey& key) {
    if (prev_combiner && prev_combiner->first == key) return &prev_combiner->second;
    const unsigned bucket=gfx_color_combiner_bucket(key);
    for(unsigned link=color_combiner_buckets[bucket];link;link=color_combiner_next[link-1u]) {
        NativeCombinerEntry* entry=&color_combiner_pool[link-1u];
        if(entry->first==key) {
            prev_combiner=entry;
            return &entry->second;
        }
    }
    gfx_flush();
    if (color_combiner_count == sizeof(color_combiner_pool)/sizeof(color_combiner_pool[0]))
        sysFatalError("Xbox combiner cache exhausted");
    prev_combiner=&color_combiner_pool[color_combiner_count++];
    memset(prev_combiner,0,sizeof(*prev_combiner));
    prev_combiner->first=key;
    gfx_generate_cc(&prev_combiner->second,key);
    const unsigned index=(unsigned)(prev_combiner-color_combiner_pool);
    color_combiner_next[index]=color_combiner_buckets[bucket];
    color_combiner_buckets[bucket]=(uint16_t)(index+1u);
    return &prev_combiner->second;
}

static void gfx_texture_invalidate(TextureCacheNode* node) {
    for (int i=0;i<2;++i) if (rendering_state.textures[i]==node) {
        rendering_state.textures[i]=0;
        rdp.textures_changed[i]=true;
    }
    gfx_texture_unlink(node);
#if GE_XBOX_FAST3D_TEXTURE_LRU_O1
    gfx_texture_lru_invalidate((unsigned)(node-gfx_texture_cache));
#endif
    node->used=false;
}

void gfx_texture_cache_clear() {
    gfx_flush();
    for (unsigned i=0;i<gfx_texture_cache_used;++i) gfx_texture_cache[i].used=false;
#if GE_XBOX_FAST3D_TEXTURE_LRU_O1
    gfx_texture_lru_clear();
#endif
    memset(gfx_texture_buckets,0,sizeof(gfx_texture_buckets));
    memset(gfx_texture_next,0,sizeof(gfx_texture_next));
    rdp.textures_changed[0]=rdp.textures_changed[1]=true;
    memset(rendering_state.textures,0,sizeof(rendering_state.textures));
}

static void gfx_texture_cache_release() {
    /* A stage reset discards every source allocation. Flush their draws before
     * releasing the GPU images as well: retaining unused images can exhaust
     * the native budget before the next stage reuses all of their cache slots. */
    gfx_texture_cache_clear();
    for(unsigned i=0;i<gfx_texture_cache_used;++i)
        gfx_rapi->delete_texture(gfx_texture_cache[i].second.texture_id);
    gfx_texture_cache_used=0;
    gfx_texture_cache_clock=0;
#if GE_XBOX_FAST3D_TEXTURE_LRU_O1
    gfx_texture_lru_clear();
#endif
}

extern "C" int gfx_texture_cache_count(void) {
    int result=0;
    for (unsigned i=0;i<gfx_texture_cache_used;++i) if(gfx_texture_cache[i].used) ++result;
    return result;
}

extern "C" u8* g_VtxBuffers[3];

static bool gfx_texture_cache_lookup(int i,const TextureCacheKey& key) {
    const unsigned bucket=gfx_texture_bucket(key);
    for(unsigned link=gfx_texture_buckets[bucket];link;link=gfx_texture_next[link-1u]) {
        TextureCacheNode* n=&gfx_texture_cache[link-1u];
        if(n->first==key) {
            ++s_texCacheHits;
            n->last_use=++gfx_texture_cache_clock;
#if GE_XBOX_FAST3D_TEXTURE_LRU_O1
            gfx_texture_lru_touch((unsigned)(n-gfx_texture_cache));
#endif
            gfx_rapi->select_texture(i,n->second.texture_id,n->second.linear_filter);
            gfx_rapi->set_sampler_parameters(i,n->second.linear_filter,n->second.cms,n->second.cmt,rdp.tex_lod);
            rendering_state.textures[i]=n;
            return true;
        }
    }
    ++s_texCacheMisses;
    TextureCacheNode* slot=0;
    /* Triangle setup imports unit 0 before unit 1. Preserve the other unit's
     * selected texture while allocating/reusing an ID: invalidating it here
     * would only mark it dirty after that unit's import pass has completed. */
    const TextureCacheNode* pinned=rendering_state.textures[1-i];
#if GE_XBOX_FAST3D_TEXTURE_LRU_O1
    slot=gfx_texture_take_unused();
#else
    TextureCacheNode* oldest=0;
    for (unsigned j=0;j<gfx_texture_cache_used;++j) {
        TextureCacheNode* n=&gfx_texture_cache[j];
        if(n==pinned) continue;
        if(!n->used && !slot) slot=n;
        if(n->used && (!oldest || n->last_use<oldest->last_use)) oldest=n;
    }
#endif
    if(!slot && gfx_texture_cache_used<TEXTURE_CACHE_MAX_SIZE) {
        slot=&gfx_texture_cache[gfx_texture_cache_used++];
        if(gfx_texture_cache_used>s_texCachePeakAllocated)
            s_texCachePeakAllocated=gfx_texture_cache_used;
        memset(slot,0,sizeof(*slot));
        slot->second.texture_id=gfx_rapi->new_texture();
    }
    if(!slot) {
        ++s_texCacheEvictions;
        gfx_flush();
#if GE_XBOX_FAST3D_TEXTURE_LRU_O1
        slot=gfx_texture_oldest_except(pinned);
#else
        slot=oldest;
#endif
        if(!slot) sysFatalError("Xbox texture cache corrupt");
        gfx_texture_invalidate(slot);
#if GE_XBOX_FAST3D_TEXTURE_LRU_O1
        /* Invalidation put the victim in the free bitmap; consume that slot
         * before linking the freshly imported image into the LRU. */
        gfx_texture_unused[(unsigned)(slot-gfx_texture_cache)>>5]&=
            ~((uint32_t)1u<<((unsigned)(slot-gfx_texture_cache)&31u));
#endif
    }
    const uint32_t id=slot->second.texture_id;
    memset(&slot->second,0,sizeof(slot->second));
    slot->second.texture_id=id;
    slot->first=key;slot->used=true;slot->last_use=++gfx_texture_cache_clock;
    const unsigned index=(unsigned)(slot-gfx_texture_cache);
    gfx_texture_next[index]=gfx_texture_buckets[bucket];
    gfx_texture_buckets[bucket]=(uint16_t)(index+1u);
#if GE_XBOX_FAST3D_TEXTURE_LRU_O1
    gfx_texture_lru_append(index);
#endif
    gfx_rapi->select_texture(i,id,false);
    gfx_rapi->set_sampler_parameters(i,false,0,0,rdp.tex_lod);
    rendering_state.textures[i]=slot;
    return false;
}

void gfx_texture_cache_delete(const uint8_t* addr) {
    gfx_flush();
    for(unsigned i=0;i<gfx_texture_cache_used;++i)
        if(gfx_texture_cache[i].used && gfx_texture_cache[i].first.texture_addr==addr)
            gfx_texture_invalidate(&gfx_texture_cache[i]);
}
void gfx_texture_cache_delete_range(const uint8_t* first,const uint8_t* last) {
    gfx_flush();
    const uintptr_t lo=(uintptr_t)first,hi=(uintptr_t)last;
    for(unsigned i=0;i<gfx_texture_cache_used;++i) {
        TextureCacheNode* n=&gfx_texture_cache[i];
        const uintptr_t p=(uintptr_t)n->first.texture_addr;
        if(n->used && p>=lo && p<hi) gfx_texture_invalidate(n);
    }
}

// D71 (docs/internals.md): texture sources arrive in two byte conventions.
// Raw N64 big-endian byte streams: ROM cart map (0x10xxxxxx), model-sidecar
// blobs (cart extension 0x10Cxxxxx), KSEG0 mirror (0x80xxxxxx) and V1
// dram/BSS/heap buffers (0x70xxxxxx, e.g. tex.c texture pool, rle_expand_8bit
// output). C-compiled u32 arrays in the exe image (.data/.rodata,
// 0x140xxxxxx on MinGW x64) instead store each N64 texel pair as a
// little-endian u32 — e.g. the rarewarelogo.c RGBA16 images — so the N64 byte
// order is recovered by bswap32 of every u32. Without this, the logo's gold
// texels (0xED0F...) decode from the swapped pairs (0x4FCC/0xCC4F) as bright
// green/pink — the garbled Rareware-logo pixels.
static bool gfx_tex_source_is_c_array(const uint8_t* addr) {
    const uintptr_t a = (uintptr_t)addr;
    if(geNativeAddressIsRam(a)) return false;
    if (a >= 0x10000000u && a < 0x20000000u) return false; // cart map + sidecar
    // V1 dram + KSEG0 mirror: the two mapped views (port/src/dram.c). D441:
    // was `a >= V1 && a < 0x90000000`, which went empty with the arena at
    // 0x90000000 and bswapped every DRAM texture (mirrored texel pairs).
    if (a >= PORT_DRAM_V1_BASE && a < PORT_DRAM_V1_BASE + PORT_DRAM_SIZE) return false;
    if (a >= PORT_DRAM_K0_BASE && a < PORT_DRAM_K0_BASE + PORT_DRAM_SIZE) return false;
    return true; // exe image: C-compiled array
}

struct NativeTextureCopy { const uint8_t* source; uint8_t* bytes; uint32_t size; };
static NativeTextureCopy s_c_array_tex_norms[128];
static unsigned s_c_array_tex_count=0;
static const uint8_t* gfx_tex_normalize_source(const uint8_t* addr,uint32_t extent) {
    if(!gfx_tex_source_is_c_array(addr)) return addr;
    if(extent>4u*1024u*1024u-3u) sysFatalError("Xbox texture extent exceeds scratch budget");
    const uint32_t n=(extent+3u)&~3u;
    NativeTextureCopy* slot=0;
    for(unsigned i=0;i<s_c_array_tex_count;++i)
        if(s_c_array_tex_norms[i].source==addr) {slot=&s_c_array_tex_norms[i];break;}
    if(slot && slot->size>=n) return slot->bytes;
    if(!slot) {
        if(s_c_array_tex_count>=128u) sysFatalError("Xbox compiled texture cache exhausted");
        slot=&s_c_array_tex_norms[s_c_array_tex_count++];
    }
    uint8_t* next=(uint8_t*)malloc(n?n:4u);
    if(!next) sysFatalError("Cannot allocate Xbox texture byte conversion");
    for(uint32_t i=0;i<n;i+=4u) {
        uint32_t v;memcpy(&v,addr+i,4);v=PD_BE32(v);memcpy(next+i,&v,4);
    }
    if(slot->bytes) free(slot->bytes);
    slot->bytes=next;slot->source=addr;slot->size=n;
    return next;
}

static void import_texture_rgba16(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    // SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);
    // TODO: this trips in some places with a garbage size in full_image_line_size_bytes
    // probably wherever framebuffer effects are used

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes / 2; i++, dest += 4) {
        const uint16_t col16 = (addr[2 * i] << 8) | addr[2 * i + 1];
        const uint8_t a = col16 & 1;
        const uint8_t r = col16 >> 11;
        const uint8_t g = (col16 >> 6) & 0x1f;
        const uint8_t b = (col16 >> 1) & 0x1f;
        dest[0] = SCALE_5_8(r);
        dest[1] = SCALE_5_8(g);
        dest[2] = SCALE_5_8(b);
        dest[3] = a ? 255 : 0;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes / 2;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_rapi->upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
    // DumpTexture(loaded_texture.otr_path, rgba32_buf, width, height);
}

static void import_texture_rgba32(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

    uint32_t *dest = (uint32_t *)tex_upload_buffer;
    const uint32_t *src = (const uint32_t *)addr;
    for (uint32_t i = 0; i < size_bytes; i += 4, ++dest, ++src) {
        *dest = PD_BE32(*src);
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes / 2;
    const uint32_t height = (size_bytes / 2) / rdp.texture_tile[tile].line_size_bytes;
	gfx_rapi->upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
    // DumpTexture(loaded_texture.otr_path, addr, width, height);
}

static void import_texture_ia4(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes * 2; i++, dest += 4) {
        const uint8_t byte = addr[i / 2];
        const uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
        /* N64 IA4 is I3:A1, matching image.c's packed channel output.
         * Splitting it as I2:A2 blackened dim texels and made opaque pixels
         * partially transparent (or discarded by the texture-edge test). */
        const uint8_t intensity = part >> 1;
        const uint8_t alpha = part & 1;
        const uint8_t c = SCALE_3_8(intensity);
        dest[0] = c;
        dest[1] = c;
        dest[2] = c;
        dest[3] = alpha ? 255 : 0;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes * 2;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_rapi->upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
    // DumpTexture(loaded_texture.otr_path, rgba32_buf, width, height);
}

static void import_texture_ia8(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes; i++, dest += 4) {
        const uint8_t intensity = SCALE_4_8(addr[i] >> 4);
        const uint8_t alpha = SCALE_4_8(addr[i] & 0xf);
        dest[0] = intensity;
        dest[1] = intensity;
        dest[2] = intensity;
        dest[3] = alpha;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_rapi->upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
    // DumpTexture(loaded_texture.otr_path, rgba32_buf, width, height);
}

static void import_texture_ia16(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes / 2; i++, dest += 4) {
        const uint8_t intensity = addr[2 * i];
        const uint8_t alpha = addr[2 * i + 1];
        dest[0] = intensity;
        dest[1] = intensity;
        dest[2] = intensity;
        dest[3] = alpha;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes / 2;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_rapi->upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
    // DumpTexture(loaded_texture.otr_path, rgba32_buf, width, height);
}

static void import_texture_i4(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes * 2; i++, dest += 4) {
        const uint8_t byte = addr[i / 2];
        const uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
        const uint8_t intensity = SCALE_4_8(part);
        dest[0] = intensity;
        dest[1] = intensity;
        dest[2] = intensity;
        dest[3] = intensity;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes * 2;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_rapi->upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
    // DumpTexture(loaded_texture.otr_path, rgba32_buf, width, height);
}

static void import_texture_i8(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes; i++, dest += 4) {
        const uint8_t intensity = addr[i];
        dest[0] = intensity;
        dest[1] = intensity;
        dest[2] = intensity;
        dest[3] = intensity;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_rapi->upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
    // DumpTexture(loaded_texture.otr_path, rgba32_buf, width, height);
}

static inline void palette_to_rgba32(const uint16_t palentry, uint8_t *rgba32_buf) {
    if (rdp.palette_fmt == G_TT_IA16) {
        /* D228: intensity/alpha were swapped here relative to the (correct)
         * direct IA16 decode in import_texture_ia16() a few lines above --
         * that one reads addr[2*i]=intensity, addr[2*i+1]=alpha, i.e.
         * intensity is the first (high, after gfx_dp_load_tlut's PD_BE16
         * swap) byte. This path had them backwards: a dim, fully-opaque
         * palette entry (e.g. intensity=0x58, alpha=0xff) decoded as a
         * bright, mostly-transparent one (intensity=0xff, alpha=0x58) --
         * the "white missing-texture patches" on AK47/NPC weapon models. */
        const uint8_t intensity = palentry >> 8;
        const uint8_t alpha = palentry & 0xff;
        rgba32_buf[0] = intensity;
        rgba32_buf[1] = intensity;
        rgba32_buf[2] = intensity;
        rgba32_buf[3] = alpha;
    } else {
        // assume G_TT_RGBA16
        const uint8_t a = palentry & 1;
        const uint8_t r = palentry >> 11;
        const uint8_t g = (palentry >> 6) & 0x1f;
        const uint8_t b = (palentry >> 1) & 0x1f;
        rgba32_buf[0] = SCALE_5_8(r);
        rgba32_buf[1] = SCALE_5_8(g);
        rgba32_buf[2] = SCALE_5_8(b);
        rgba32_buf[3] = a ? 255 : 0;
    }
}

static void import_texture_ci4(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
	const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    const uint32_t pal_idx = rdp.texture_tile[tile].palette; // 0-15
    const uint16_t* palette = (const uint16_t *)(rdp.palette + pal_idx * 16); // 16 pixel entries, 16 bits each
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

    for (uint32_t i = 0; i < size_bytes * 2; i++) {
        const uint8_t byte = addr[i / 2];
        const uint8_t idx = (byte >> (4 - (i % 2) * 4)) & 0xf;
        palette_to_rgba32(palette[idx], tex_upload_buffer +4 * i);
    }

    uint32_t result_line_size = rdp.texture_tile[tile].line_size_bytes;
    if (metadata->h_byte_scale != 1) {
        result_line_size *= metadata->h_byte_scale;
    }

    const uint32_t width = result_line_size * 2;
    const uint32_t height = size_bytes / result_line_size;

	gfx_rapi->upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
}

static void import_texture_ci8(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
	const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;

    for (uint32_t i = 0, j = 0; i < size_bytes; j += full_image_line_size_bytes - line_size_bytes) {
        for (uint32_t k = 0; k < line_size_bytes; i++, k++, j++) {
            const uint8_t idx = addr[j];
            palette_to_rgba32(rdp.palette[idx], tex_upload_buffer + 4 * i);
        }
    }

    uint32_t result_line_size = rdp.texture_tile[tile].line_size_bytes;
    if (metadata->h_byte_scale != 1) {
        result_line_size *= metadata->h_byte_scale;
    }

    const uint32_t width = result_line_size;
    const uint32_t height = size_bytes / result_line_size;

	gfx_rapi->upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
}

/* RC2 mip-contamination clamp (see import_texture). Default on;
 * Video.FixMipTextures = 0 restores the raw over-tall upload. */
bool g_fix_mip_textures = true;

/* D74 sub-tile UV pre-wrap (see gfx_sp_tri). Opt-in (never-run path); default
 * off. Video.WrapFix = 1. */
bool g_wrap_fix = false;

/* D236 pass 26: for a GE TEXTURETYPE_DETAIL binding, sample the BASE image
 * (tile 1) rather than the detail texture sitting at TMEM 0. See
 * gfx_lod_tile_offset. Video.DetailBaseTile. */
bool g_detail_base_tile = false;

/* D183 source-pitch de-stride (see import_texture). Default on;
 * GE_TEXPITCH=0 restores the old flat read for A/B. */
static bool gfx_tex_pitch_fix(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* e = getenv("GE_TEXPITCH");
        cached = (e && e[0] == '0') ? 0 : 1;
    }
    return cached != 0;
}

/* Preserve the D217/D476 content keys exactly, but hash only palettes a
 * draw actually reads. CI4 touches one 32-byte bank, whereas the previous
 * TLUT path hashed all 512 bytes even for every small CI4 palette load. */
static uint32_t gfx_palette_hash(uint8_t siz,uint8_t palette_index) {
    if(siz==G_IM_SIZ_4b) {
        const unsigned bank=palette_index&15u;
        const unsigned bit=1u<<bank;
        if(!(rdp.palette_bank_hash_valid&bit)) {
            uint32_t h=2166136261u;
            const uint8_t* pb=(const uint8_t*)(rdp.palette+bank*16u);
            for(unsigned k=0;k<16u*sizeof(uint16_t);++k) {h^=pb[k];h*=16777619u;}
            rdp.palette_bank_hash[bank]=h;
            rdp.palette_bank_hash_valid|=(uint16_t)bit;
        }
        return rdp.palette_bank_hash[bank];
    }
    if(rdp.palette_hash_dirty) {
        uint32_t h=2166136261u;
        const uint8_t* pb=(const uint8_t*)rdp.palette;
        for(unsigned k=0;k<sizeof(rdp.palette);++k) {h^=pb[k];h*=16777619u;}
        rdp.palette_hash=h;
        rdp.palette_hash_dirty=false;
    }
    return rdp.palette_hash;
}

static void import_texture(int i, int tile, bool importReplacement) {
    GE_P0E_SCOPE(GE_P0E_IMPORT);
    LoadedTexture& loaded_texture = rdp.loaded_texture[rdp.texture_tile[tile].tmem];
    const uint8_t fmt = rdp.texture_tile[tile].fmt;
    const uint8_t siz = rdp.texture_tile[tile].siz;
    const uint32_t tex_flags = loaded_texture.tex_flags;
    const uint8_t palette_index = rdp.texture_tile[tile].palette;

    /* D463: a SETTILE with line=0 makes every importer divide by zero
     * (height = size_bytes / line_size_bytes -- 0xc0000094 integer-divide on
     * Windows x64; the crash PC was import_texture_i8). The game emits this
     * legitimately: textrelated.c's outlined-text path renders ANY byte
     * < 0x21 through chars[*text - 0x21], a zeroed fontchar (w=h=0,
     * pixeldata=NULL), and gDPLoadTextureBlock(width=0) then expands to
     * gsDPTile(line=0) + a zero-area gSPTextureRectangle. The N64 RDP draws
     * nothing for a zero-area rect, so the decomp is correct -- fast3d must
     * tolerate it. Skip the import entirely (no cache entry, no upload);
     * the degenerate draw rasterizes no fragments, so whatever stays bound
     * on this unit is never sampled. Nonzero line sizes take the exact
     * original path below. */
    if (rdp.texture_tile[tile].line_size_bytes == 0) {
        static int d463_warned = 0;
        if (d463_warned < 8)
            sysLogPrintf(LOG_NOTE, "D463: texrect on zero-line tile -- import skipped "
                                   "(tile=%d fmt=%u siz=%u tmem=%u)",
                         tile, fmt, siz, rdp.texture_tile[tile].tmem);
        d463_warned++;
        return;
    }

    // D74: only fall back when the tmem slot was never written by a load
    // command. The old `rdp.tex_lod && tile >= first+detail` branch also
    // overwrote valid gDPLoadBlock data with line*tile.height, which (a) dropped
    // mip chains and (b) truncated sub-tiled textures to the sub-tile's row
    // count (e.g. the Rare-logo D_02005FF0 20x3 tile uploaded as 32x3).
    if (!loaded_texture.addr) {
        // set up miplevel 0; also acts as a catch-all for when .addr is NULL because my texture loader sucks
        loaded_texture.addr = rdp.texture_to_load.addr;
        loaded_texture.line_size_bytes = rdp.texture_tile[tile].line_size_bytes;
        loaded_texture.full_image_line_size_bytes = rdp.texture_tile[tile].line_size_bytes;
        loaded_texture.full_size_bytes = loaded_texture.full_image_line_size_bytes * rdp.texture_tile[tile].height;
        loaded_texture.size_bytes = loaded_texture.line_size_bytes * rdp.texture_tile[tile].height;
        if (siz == G_IM_SIZ_32b) {
            // HACK: fixup 32-bit LODed texture height
            loaded_texture.size_bytes <<= 1;
            loaded_texture.full_size_bytes <<= 1;
        }
        loaded_texture.orig_size_bytes = loaded_texture.size_bytes;
    }

    /* RC2 (docs/dev/TEXTURE-GLITCH-ANALYSIS.md): GE's texGetDepthAndSize() sums the
     * base level + every LOD mip into one gDPLoadBlock, so the block byte count
     * (-> upload height = size_bytes / row) runs ~1.3x taller than the base
     * image and the mip bytes render as garbage rows below it ("interlaced"
     * textures on the menu / Depot). Only when LOD is active (rdp.tex_lod) AND
     * the load is a plain full-width block (not a windowed gfx_dp_load_tile,
     * which legitimately has extra rows -- the D74 sub-tile / Rare-logo case):
     * clip to the SETTILESIZE base-tile height. GL then builds correct mips
     * from a correct base image. */
    if (g_fix_mip_textures && rdp.tex_lod &&
        loaded_texture.line_size_bytes == loaded_texture.full_image_line_size_bytes) {
        const uint32_t row = rdp.texture_tile[tile].line_size_bytes;
        const uint32_t tile_h =
            ((uint32_t)(rdp.texture_tile[tile].lrt - rdp.texture_tile[tile].ult) >> 2) + 1;
        if (row && tile_h > 1) {
            uint32_t base_bytes = row * tile_h;
            if (siz == G_IM_SIZ_32b)
                base_bytes <<= 1; /* mirrors the 32b height fixup above */
            if (base_bytes < loaded_texture.size_bytes) {
                loaded_texture.size_bytes = base_bytes;
                if (loaded_texture.full_size_bytes > base_bytes)
                    loaded_texture.full_size_bytes = base_bytes;
                loaded_texture.orig_size_bytes = base_bytes;
            }
        }
    }

    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* orig_addr = loaded_texture.addr;
    SUPPORT_CHECK(orig_addr);

    TextureCacheKey key;
    if (fmt == G_IM_FMT_CI) {
        /* D476: a CI4 texture decodes only its 16-entry bank
         * (rdp.palette + palette_index * 16, see import_texture_ci4), so key it
         * on that bank's content. The whole-table D217 hash made unrelated
         * TLUT loads elsewhere in the table re-import unchanged CI4 textures.
         * CI8 reads all 256 entries and keeps the whole-table hash. */
        const uint32_t ph=gfx_palette_hash(siz,palette_index);
        key = { orig_addr, { rdp.palette_addrs[0], rdp.palette_addrs[1] }, fmt, siz, palette_index,
                loaded_texture.size_bytes, ph }; // D217/D476: key on (used) palette content
    } else {
        key = { orig_addr, {}, fmt, siz, palette_index, loaded_texture.size_bytes, 0u };
    }
    /* D75: a dxt==0 load samples with swapped odd rows -> distinct image.
     * Only compiled-in (exe-image) assets: the runtime texture pipeline
     * (tex.c/image.c, 0x70xxxxxx) loads dxt==0 too, but D159 no-ops its
     * compensating pre-swap, so those are already linear. */
    const bool d75_swap = loaded_texture.dxt0 && siz != G_IM_SIZ_32b &&
                          gfx_tex_source_is_c_array(orig_addr) &&
                          rdp.texture_tile[tile].line_size_bytes != 0 &&
                          (rdp.texture_tile[tile].line_size_bytes & 7) == 0 &&
                          loaded_texture.size_bytes > rdp.texture_tile[tile].line_size_bytes;
    if (d75_swap) key.palette_hash ^= 0xD75D75D7u;

    /* Intro blood (M-201): textures the game regenerates IN PLACE in the
     * per-frame dynamic pool (dynAllocate, [g_VtxBuffers[0], g_VtxBuffers[2]))
     * -- e.g. the gun-barrel / death blood-drip image rebuilt by
     * die_blood_image_routine -- reuse the same addresses frame after frame,
     * so an address-only key returns a stale GL texture from an earlier frame.
     * Key those on a content hash (FNV-1a) instead; static textures keep the
     * free address key. */
    {
        /* Game code often reaches this memory through OS_K0_TO_PHYSICAL,
         * which fast3d resolves into the byte-identical KSEG0 mirror at
         * 0x80000000 (port/src/dram.c V2) -- normalise to the V1 view the
         * dyn pool pointers use before the range test. */
        const uint8_t* v1addr = orig_addr;
        if ((uintptr_t)v1addr >= PORT_DRAM_K0_BASE && (uintptr_t)v1addr < PORT_DRAM_K0_BASE + PORT_DRAM_SIZE) {
            v1addr -= (PORT_DRAM_K0_BASE - PORT_DRAM_V1_BASE);
        }
        if (g_VtxBuffers[0] && v1addr >= g_VtxBuffers[0] && v1addr < g_VtxBuffers[2]) {
            uint32_t h = 2166136261u;
            const uint32_t n = loaded_texture.size_bytes;
            for (uint32_t b = 0; b < n; b++) {
                h = (h ^ orig_addr[b]) * 16777619u;
            }
            key.palette_hash = h ? h : 1u;
        }
    }

    if (gfx_texture_cache_lookup(i, key)) {
        return;
    }

    // D71: importers read raw N64 byte streams; normalize C-array sources.
    const uint8_t* saved_addr = loaded_texture.addr;
    const uint32_t saved_full_line = loaded_texture.full_image_line_size_bytes;
    loaded_texture.addr =
        gfx_tex_normalize_source(orig_addr,
                                 loaded_texture.full_size_bytes > loaded_texture.size_bytes
                                     ? loaded_texture.full_size_bytes
                                     : loaded_texture.size_bytes);

    /* D183: honour the source row pitch. gfx_dp_load_tile can load a
     * sub-rectangle of a wider texture image: successive texel rows then sit
     * full_image_line_size_bytes apart in RAM while only line_size_bytes of
     * each row belong to the tile. Every import_texture_* except the CI8 one
     * reads the source flat (they only assert line == full -- and the asserts
     * compile out in the release build), so row r starts r*(full-line) bytes
     * early -> a progressive diagonal shear that reads as grey static /
     * "comb interlacing" (D176(b) Surface cliff walls, D182(2) file-select
     * spiral). Compact the strided rows into a contiguous scratch buffer once
     * here, so every importer sees line == full and no importer needs to know
     * about pitch. No-op when the load was already row-packed (gDPLoadBlock,
     * and any full-width gDPLoadTile), which is the overwhelming majority --
     * hence golden-safe. */
    GENativeBuffer destride_buf;
    if (gfx_tex_pitch_fix()) {
        const uint32_t src_line = loaded_texture.line_size_bytes;
        const uint32_t src_full = loaded_texture.full_image_line_size_bytes;
        if (src_line && src_full > src_line && loaded_texture.size_bytes > src_line) {
            const uint32_t rows = loaded_texture.size_bytes / src_line;
            destride_buf.resize((size_t)rows * src_line);
            for (uint32_t r = 0; r < rows; r++) {
                memcpy(&destride_buf[(size_t)r * src_line],
                       loaded_texture.addr + (size_t)r * src_full, src_line);
            }
            loaded_texture.addr = destride_buf.data();
            loaded_texture.full_image_line_size_bytes = src_line;
        }
    }

    /* D75: emulate the RDP's odd-row TMEM read swap for dxt==0 loads (see
     * LoadedTexture::dxt0). Done after destride so rows are contiguous. */
    GENativeBuffer d75_buf;
    if (d75_swap) {
        const uint32_t ln = rdp.texture_tile[tile].line_size_bytes;
        const uint32_t rows = loaded_texture.size_bytes / ln;
        d75_buf.assign(loaded_texture.addr, loaded_texture.addr + (size_t)rows * ln +
                       (loaded_texture.size_bytes - rows * ln));
        for (uint32_t r = 1; r < rows; r += 2) {
            uint8_t* row = &d75_buf[(size_t)r * ln];
            for (uint32_t b = 0; b + 8 <= ln; b += 8) {
                uint8_t t4[4];
                memcpy(t4, row + b, 4);
                memcpy(row + b, row + b + 4, 4);
                memcpy(row + b + 4, t4, 4);
            }
        }
        loaded_texture.addr = d75_buf.data();
    }

    /* GE_DTEX: dump the load parameters for the first N textures of a frame so
     * RC2 (mip-chain contamination -> over-tall upload) can be told apart from a
     * decode/row-swap bug. tile_h = base-tile height from SETTILESIZE; if the
     * computed upload height is much larger, the excess rows are LOD mip data.
     * docs/dev/TEXTURE-GLITCH-ANALYSIS.md sec 6b.
     * D250: import_texture() runs on every texture bind -- an uncached
     * getenv() here measured at ~50% of the hot render thread's total CPU
     * time on a texture-heavy level (Dam), via a live WPR/xperf profile.
     * Cache like every other env-gated probe in this codebase. */
    static int ge_dtex = -1;
    if (ge_dtex < 0) ge_dtex = getenv("GE_DTEX") != NULL;
    if (ge_dtex) {
        static int dtexCount = 0;
        if (dtexCount < 64) {
            const uint32_t row = rdp.texture_tile[tile].line_size_bytes;
            const uint32_t up_h = row ? (loaded_texture.size_bytes / row) : 0;
            const uint32_t tile_h =
                ((rdp.texture_tile[tile].lrt - rdp.texture_tile[tile].ult) >> 2) + 1;
            const uint32_t tile_w =
                ((rdp.texture_tile[tile].lrs - rdp.texture_tile[tile].uls) >> 2) + 1;
            sysLogPrintf(LOG_NOTE,
                "GE_DTEX[%d] addr=%p fmt=%u siz=%u lod=%d  tile=%ux%u  row=%u "
                "line=%u full=%u size=%u -> upload=%ux%u%s%s",
                dtexCount++, (void *)orig_addr, fmt, siz, (int)rdp.tex_lod,
                tile_w, tile_h, row, loaded_texture.line_size_bytes,
                loaded_texture.full_image_line_size_bytes,
                loaded_texture.size_bytes,
                row ? (row >> (siz ? siz - 1 : 0)) : 0, up_h,
                (up_h > tile_h + 1) ? "  <-- OVER-TALL (mip contamination?)" : "",
                (loaded_texture.full_image_line_size_bytes !=
                 loaded_texture.line_size_bytes) ? "  <-- STRIDED (pitch shear?)" : "");
        }
    }

    static int ge_texdump = -1;
    if (ge_texdump < 0) ge_texdump = getenv("GE_TEXDUMP") != NULL;
    if (ge_texdump) {
        static int tdc = 0;
        const uint16_t* pal = (const uint16_t*)rdp.palette;
        sysLogPrintf(LOG_NOTE,
            "GE_TEXI[%d] addr=%p fmt=%u siz=%u palfmt=%u palidx=%u size=%u "
            "tile=%dx%d pal[0..3]=%04x %04x %04x %04x", tdc++, (void*)orig_addr,
            fmt, siz, rdp.palette_fmt, palette_index, loaded_texture.size_bytes,
            ((rdp.texture_tile[tile].lrs - rdp.texture_tile[tile].uls) >> 2) + 1,
            ((rdp.texture_tile[tile].lrt - rdp.texture_tile[tile].ult) >> 2) + 1,
            pal[0], pal[1], pal[2], pal[3]);
        /* GE_TEXRAW=1 additionally writes the raw source bytes handed to the
         * importer (texdump/rNNN_f<fmt>_s<siz>_<w>x<h>.bin) -- lets a decode
         * bug be told apart from a source-data bug offline (D183). */
        const int tw = (int)(((rdp.texture_tile[tile].lrs - rdp.texture_tile[tile].uls) >> 2) + 1);
        const int th = (int)(((rdp.texture_tile[tile].lrt - rdp.texture_tile[tile].ult) >> 2) + 1);
        /* D219: same cap-exhausted-before-the-explosion problem as the
         * D172/GE_TEXDUMP probes -- never suppress the fire particle image
         * dump. NOTE: `tw`/`th` above come from gsDPSetTileSize (the DL sets
         * the fire tile's logical wrap size to 56x56, unrelated to its real
         * 16x14 pixel content), so they do NOT identify this texture -- a
         * first attempt at this exemption keyed on tw/th==16/14 and silently
         * never matched, burning a whole live-playtest cycle for nothing.
         * The real, always-correct signature is the load's byte count: 16 *
         * 14 * 2 bytes/texel (RGBA16) = 448, matching CALC_LRS(16,14,...) in
         * every one of assets/oddtextures.c's 15 fire DLs and confirmed
         * against the TEXEL1_bytes=448 field already proven out via the
         * gfx_sp_tri1 D172/D219 probe. */
        const bool is_fire_bytes = (loaded_texture.size_bytes == 448);
        static int ge_texraw = -1;
        if (ge_texraw < 0) ge_texraw = getenv("GE_TEXRAW") != NULL;
        if ((is_fire_bytes || tdc <= 400) && ge_texraw) {
            char nm[160];
            snprintf(nm, sizeof nm, "texdump/r%03d_f%u_s%u_%ux%u.bin", tdc - 1, fmt, siz, tw, th);
            FILE* bf = fopen(nm, "wb");
            if (bf) { fwrite(loaded_texture.addr, 1, loaded_texture.size_bytes, bf); fclose(bf); }
        }
        /* D75 round 3: GE_TEXDUMP_ADDR=<hex> -- dump the raw source bytes of
         * EVERY import from that address (bypassing the 400-cap) with byte
         * stats, so a late re-import into a scratch-arena alias can be told
         * apart from a never-loaded buffer. */
        static uint64_t ge_texdump_addr = 0;
        static int ge_texdump_addr_init = 0;
        if (!ge_texdump_addr_init) {
            ge_texdump_addr_init = 1;
            const char* ea = getenv("GE_TEXDUMP_ADDR");
            if (ea && *ea) ge_texdump_addr = strtoul(ea, NULL, 16);
        }
        if (ge_texdump_addr && (uint64_t)(uintptr_t)orig_addr == ge_texdump_addr) {
            unsigned mn = 255, mx = 0, nz = 0;
            for (uint32_t i = 0; i < loaded_texture.size_bytes; i++) {
                uint8_t b = loaded_texture.addr[i];
                if (b < mn) mn = b; if (b > mx) mx = b; if (b) nz++;
            }
            char nm2[160];
            snprintf(nm2, sizeof nm2, "texdump/a%05d_%ux%u.bin", tdc - 1, tw, th);
            FILE* bf2 = fopen(nm2, "wb");
            if (bf2) { fwrite(loaded_texture.addr, 1, loaded_texture.size_bytes, bf2); fclose(bf2); }
            extern uint32_t num_dls;
            sysLogPrintf(LOG_NOTE,
                "GE_TEXA[%d] dls=%u addr=%p fmt=%u siz=%u size=%u min=%u max=%u nonzero=%u/%u file=%s",
                tdc - 1, num_dls, (void*)orig_addr, fmt, siz,
                loaded_texture.size_bytes, mn, mx, nz, loaded_texture.size_bytes, nm2);
        }
    }

    /* D161: a CI-format tile drawn with the TLUT disabled (G_TT_NONE) must NOT
     * do a palette lookup -- the N64 RDP feeds the raw TMEM texel straight into
     * the colour pipe, i.e. it behaves as a plain intensity (I) texture. GE's
     * Depot ceiling emits exactly this (CI8 + gsDPSetTextureLUT(G_TT_NONE));
     * decoding it against the stale rdp.palette produced the blue-speckle roof
     * (docs/dev/TEXTURE-GLITCH-ANALYSIS.md, B2). Route CI4/CI8 -> I4/I8 here. */
    uint8_t fmt_eff = fmt;
    uint8_t siz_eff = siz;
    if (fmt == G_IM_FMT_CI && rdp.palette_fmt == G_TT_NONE) {
        fmt_eff = G_IM_FMT_I;
    }

    /* D229: green/pulsating sky water. GE's texSelect loads CI8 mipmap chains
     * with gDPLoadBlock (SetTexImage format = CI; the 16b "size" is only the
     * fast3d 4KB-per-block convention), then the sky-water draw re-declares
     * the same TMEM slot as RGBA/16b (sub_GAME_7F09343C). On N64 GE's custom
     * RSP ucode expands the indices through the TLUT into real 16-bit pixels
     * in TMEM, so the RGBA16 tile samples blue water. fast3d has no such
     * expansion: it uploads the raw index bytes as 16-bit texels (g = 2*idx
     * mod 32 dominates) -> green mottle. Route the import through the CI8
     * palette path instead -- the port-layer equivalent of the ucode's
     * expand-at-load. Only fires when the tile format genuinely disagrees
     * with the loaded source, so real RGBA16 textures are untouched. */
    if (fmt_eff == G_IM_FMT_RGBA && siz == G_IM_SIZ_16b &&
        loaded_texture.src_fmt == G_IM_FMT_CI) {
#ifdef PORT
        static int ge_d229_a = -1;
        if (ge_d229_a < 0) ge_d229_a = getenv("GE_D229") != NULL;
        if (ge_d229_a) {
            static int n_ci8r = 0;
            if (n_ci8r++ < 8)
                sysLogPrintf(LOG_NOTE, "D229: RGBA16 tile over CI8 source -> ci8 import (addr=%p size=%u palidx=%u)",
                             (const void *)orig_addr, loaded_texture.size_bytes, palette_index);
        }
#endif
        fmt_eff = G_IM_FMT_CI;
        siz_eff = G_IM_SIZ_8b;
    }

    if (fmt_eff == G_IM_FMT_RGBA) {
        if (siz_eff == G_IM_SIZ_16b) {
            import_texture_rgba16(tile, loaded_texture, rdp.tex_lod);
        } else if (siz_eff == G_IM_SIZ_32b) {
            import_texture_rgba32(tile, loaded_texture, rdp.tex_lod);
        } else {
            sysFatalError("Bad size for RGBA texture in tile %d: %02x", tile, siz);
        }
    } else if (fmt_eff == G_IM_FMT_IA) {
        if (siz_eff == G_IM_SIZ_4b) {
            import_texture_ia4(tile, loaded_texture, rdp.tex_lod);
        } else if (siz_eff == G_IM_SIZ_8b) {
            import_texture_ia8(tile, loaded_texture, rdp.tex_lod);
        } else if (siz_eff == G_IM_SIZ_16b) {
            import_texture_ia16(tile, loaded_texture, rdp.tex_lod);
        } else {
            sysFatalError("Bad size for IA texture in tile %d: %02x", tile, siz);
        }
    } else if (fmt_eff == G_IM_FMT_CI) {
        if (siz_eff == G_IM_SIZ_4b) {
            import_texture_ci4(tile, loaded_texture, rdp.tex_lod);
        } else if (siz_eff == G_IM_SIZ_8b) {
            /* D245 (M-201): for the D229 case (RGBA16 tile over a CI8 load --
             * the IsWater sky water), upload only the base level: the load
             * carries the whole mip chain (32x32 base + mips = 1400 B), which
             * made a 32x43 image whose GL REPEAT period (43 rows) differs from
             * the N64's mask period (32). See the tri-path counterpart. */
            const uint8_t maskt = rdp.texture_tile[tile].maskt;
            /* Crop by the TILE line size (the importer's row width): for a
             * LoadBlock, loaded_texture.line_size_bytes is the whole block, so
             * testing against it never cropped (M-201 follow-up: the mip rows
             * then showed as coloured dashes on Frigate's water). */
            const uint32_t d245_row = rdp.texture_tile[tile].line_size_bytes;
            if (fmt == G_IM_FMT_RGBA && siz == G_IM_SIZ_16b && maskt > 0 && maskt < 12 &&
                d245_row > 0 && loaded_texture.size_bytes > d245_row * (1u << maskt)) {
                LoadedTexture lt = loaded_texture;
                lt.size_bytes = d245_row * (1u << maskt);
                import_texture_ci8(tile, lt, rdp.tex_lod);
            } else {
                import_texture_ci8(tile, loaded_texture, rdp.tex_lod);
            }
        } else {
            sysFatalError("Bad size for CI texture in tile %d: %02x", tile, siz);
        }
    } else if (fmt_eff == G_IM_FMT_I) {
        if (siz_eff == G_IM_SIZ_4b) {
            import_texture_i4(tile, loaded_texture, rdp.tex_lod);
        } else if (siz_eff == G_IM_SIZ_8b) {
            import_texture_i8(tile, loaded_texture, rdp.tex_lod);
        } else {
            sysFatalError("Bad size for I texture in tile %d: %02x", tile, siz);
        }
    } else {
        sysFatalError("Bad texture format in tile %d: %02x %02x", tile, fmt, siz);
    }

    loaded_texture.addr = saved_addr;
    loaded_texture.full_image_line_size_bytes = saved_full_line;
}

static void gfx_normalize_vector(float v[3]) {
    float s = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= s;
    v[1] /= s;
    v[2] /= s;
}

static void gfx_transposed_matrix_mul(float res[3], const float a[3], const float b[4][4]) {
    res[0] = a[0] * b[0][0] + a[1] * b[0][1] + a[2] * b[0][2];
    res[1] = a[0] * b[1][0] + a[1] * b[1][1] + a[2] * b[1][2];
    res[2] = a[0] * b[2][0] + a[1] * b[2][1] + a[2] * b[2][2];
}

static void calculate_normal_dir(const Light_t* light, float coeffs[3]) {
    const float light_dir[3] = { light->dir[0] / 127.f, light->dir[1] / 127.f, light->dir[2] / 127.f };

    gfx_transposed_matrix_mul(coeffs, light_dir, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
    gfx_normalize_vector(coeffs);
}

static void calculate_normal_dir(const struct NormalColor *vcn, float coeffs[3]) {
    const float light_dir[3] = { vcn->x / 127.f, vcn->y / 127.f, vcn->z / 127.f };

    gfx_transposed_matrix_mul(coeffs, light_dir, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
    gfx_normalize_vector(coeffs);
}

static void gfx_matrix_mul(float res[4][4], const float a[4][4], const float b[4][4]) {
    float tmp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(res, tmp, sizeof(tmp));
}

static void gfx_sp_matrix(uint8_t parameters, const int32_t* addr) {
    float matrix[4][4];

    /* D144: a front-end 3D model (MISSION COMPLETE dossier, mode-select
     * wallets - the D75 family) can emit a gSPMatrix whose pointer field was
     * never resolved: w1 comes through as 0xFFFF... or another non-canonical
     * value, and seg_addr() then hands us a wild pointer -> AV reading addr[].
     * A real N64 matrix pointer always lands in mapped memory. Rather than
     * crash the menu transition, substitute identity for an unmapped source
     * (the model draws with the wrong transform - already a parked D75
     * cosmetic - but the screen and its "Next" -> menu path work). */
    const bool addr_bad = ((uintptr_t)addr < 0x10000 ||
                           (uintptr_t)addr >= 0x0000800000000000ULL);

    if (addr_bad) {
        memset(matrix, 0, sizeof(matrix));
        matrix[0][0] = matrix[1][1] = matrix[2][2] = matrix[3][3] = 1.0f;
    } else
#ifndef GBI_FLOATS
    // Original GBI where fixed point matrices are used
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j += 2) {
            int32_t int_part = addr[i * 2 + j / 2];
            uint32_t frac_part = addr[8 + i * 2 + j / 2];
            matrix[i][j] = (int32_t)((int_part & 0xffff0000) | (frac_part >> 16)) / 65536.0f;
            matrix[i][j + 1] = (int32_t)((int_part << 16) | (frac_part & 0xffff)) / 65536.0f;
        }
    }
#else
    // For a modified GBI where fixed point values are replaced with floats
    memcpy(matrix, addr, sizeof(matrix));
#endif

    if (parameters & G_MTX_PROJECTION) {
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.P_matrix, matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.P_matrix, matrix, rsp.P_matrix);
        }
    } else { // G_MTX_MODELVIEW
        if ((parameters & G_MTX_PUSH) && rsp.modelview_matrix_stack_size < 11) {
            ++rsp.modelview_matrix_stack_size;
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1],
                   rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 2], sizeof(matrix));
        }
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix,
                           rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
        }
        rsp.lights_changed = 1;
    }
    gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
}

static void gfx_sp_pop_matrix(uint32_t count) {
    while (count--) {
        if (rsp.modelview_matrix_stack_size > 0) {
            --rsp.modelview_matrix_stack_size;
            if (rsp.modelview_matrix_stack_size > 0) {
                gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1],
                               rsp.P_matrix);
            }
        }
    }
}

static float gfx_adjust_x_for_aspect_ratio(float x, float w = 1.f) {
    if (fbActive) {
        return x;
    } else {
        return (rsp.aspect_ofs * w + x) * rsp.aspect_scale / gfx_current_dimensions.aspect_ratio;
    }
}

static void gfx_adjust_width_height_for_scale(uint32_t& width, uint32_t& height) {
    width = ge_gfx_round(width * RATIO_Y);
    height = ge_gfx_round(height * RATIO_Y);
    if (width == 0) {
        width = 1;
    }
    if (height == 0) {
        height = 1;
    }
}

/* D146: a valid mapped-memory pointer check. A desynced / corrupt front-end
 * DL (D75 family) hands wild pointers to the RSP command handlers; abort()
 * over one bad menu model is the wrong trade for a breadth-first port. */
static inline bool fast3d_ptr_ok(const void *p) {
    uintptr_t v = (uintptr_t)p;
    return v >= 0x10000 && v < 0x0000800000000000ULL;
}

extern "C" u32 videoGetFrameCount(void); /* port/src/video.c (D75 probe) */

/* D75 DL-sequence dump (M-196, GE_D75D="lo-hi"): the M-195 slot-overwrite
 * hypothesis says every G_VTX batch encodes dest_index=0, so a multi-batch
 * node's later batches clobber earlier ones and only the last survives. That
 * is ONLY harmful if a node's triangles are drawn AFTER a later node's G_VTX
 * has overwritten their slots -- i.e. only if the DL is structured batched
 * (all vertices for all nodes, then all triangles) rather than streamed
 * (each node: its vertices then its triangles). gfx_sp_tri1 copies vertex data
 * into buf_vbo eagerly at triangle time, so streaming order makes cross-node
 * overwrites harmless exactly like the N64 RDP. This probe records, per frame,
 * a compact V/T token stream (V=G_VTX batch, T=triangle) plus max-triangle-
 *index vs cumulative-vertices-loaded, to decide which structure GE's front-end
 * model DLs actually use. Zero cost unless GE_D75D is set. Remove once D75 is
 * root-caused. */
static int d75d_lo = -1, d75d_hi = 0x7FFFFFFF;
static int d75d_init_done = 0;
static uint32_t d75d_frame = 0xFFFFFFFFu;
static char d75d_seq[4096];
static int d75d_seqlen = 0;
static uint32_t d75d_maxtri = 0, d75d_vtxloaded = 0, d75d_batchhi = 0, d75d_nv = 0, d75d_nt = 0;
static uint32_t d75d_rej_triv = 0, d75d_rej_cull = 0, d75d_emitted = 0;
static uint32_t d75d_emit_on = 0, d75d_emit_off = 0; // emitted tris: NDC bbox intersects [-1,1]^2 vs not
static float d75d_ndc_minx=1e9f,d75d_ndc_maxx=-1e9f,d75d_ndc_miny=1e9f,d75d_ndc_maxy=-1e9f;
static uint32_t d75d_emit_invis = 0; // emitted tris with SHADER_OPT_INVISIBLE (G_BL_0+G_BL_CLR_MEM)
static int d75d_cmin=999, d75d_cmax=-1; // min/max over all vertex color channels of emitted tris
static uint32_t d75d_oml = 0xFFFFFFFFu; static int d75d_oml_set = 0;
static float d75d_zmin=1e9f,d75d_zmax=-1e9f; static uint32_t d75d_znan=0; static int d75d_use_tex=-1;
static uint64_t d75d_comb=0; static int d75d_prim[4]={-1,-1,-1,-1}; static int d75d_env[4]={-1,-1,-1,-1};
static int d75d_comb_utex=-1; // comb->used_textures[0] (combiner's own claim)
// D75D round 2: logo-mode-only sampling (the frame-level fields above mix the
// first emitted tri -- often a title quad -- with the LAST tri's combine mode,
// which is what made 'comb claims 0' look like an anomaly). These are updated
// on EVERY emitted tri whose rdp.combine_mode is the logo word, so comb and
// utex always describe the same triangle.
static int d75d_lg_utex=-1, d75d_lg_tex=-1; static uint32_t d75d_lg_cnt=0;
static int d75d_lg_set=0; static uint32_t d75d_lg_tmem,d75d_lg_fmt,d75d_lg_siz;
static int d75d_lg_shifts,d75d_lg_shifft,d75d_lg_uls,d75d_lg_ult,d75d_lg_lrs,d75d_lg_lrt;
static const uint8_t* d75d_lg_texaddr=nullptr; static uint32_t d75d_lg_texid,d75d_lg_texbytes;
static int d75d_lg_u0,d75d_lg_v0;

static void d75d_init(void) {
    if (d75d_init_done) return;
    d75d_init_done = 1;
    const char* e = getenv("GE_D75D");
    if (e && *e) {
        d75d_lo = atoi(e);
        const char* dash = strchr(e, '-');
        if (dash) d75d_hi = atoi(dash + 1);
    }
}
static bool d75d_env_active(void) { d75d_init(); return d75d_lo >= 0; }
static void d75d_flush(uint32_t f) {
    if (f == 0xFFFFFFFFu || d75d_seqlen == 0) return;
    fprintf(stderr,
        "D75D: f=%u nv=%u nt=%u maxtri=%u vtxloaded=%u batchhi=%u rej_triv=%u rej_cull=%u emitted=%u emit_on=%u emit_off=%u ndc_x=[%.3f,%.3f] ndc_y=[%.3f,%.3f] invis=%u col=[%d,%d] oml=0x%08x omset=%d z=[%.4f,%.4f] znan=%u utex=%d cutex=%d comb=0x%016llx prim=(%d,%d,%d,%d) env=(%d,%d,%d,%d) lg_cnt=%u lg_utex=%d lg_tex=%d lg_tile(tmem=%u fmt=%u siz=%u sh=%d st=%d uls=%d ult=%d lrs=%d lrt=%d) lg_texaddr=%p lg_texid=%u lg_texbytes=%u lg_uv0=(%d,%d) seq=%.8s%s\n",
        f, d75d_nv, d75d_nt, d75d_maxtri, d75d_vtxloaded, d75d_batchhi,
        d75d_rej_triv, d75d_rej_cull, d75d_emitted, d75d_emit_on, d75d_emit_off,
        d75d_ndc_minx, d75d_ndc_maxx, d75d_ndc_miny, d75d_ndc_maxy,
        d75d_emit_invis, d75d_cmin, d75d_cmax, (unsigned)d75d_oml, d75d_oml_set,
        d75d_zmin, d75d_zmax, d75d_znan, d75d_use_tex, d75d_comb_utex,
        (unsigned long long)d75d_comb, d75d_prim[0],d75d_prim[1],d75d_prim[2],d75d_prim[3],
        d75d_env[0],d75d_env[1],d75d_env[2],d75d_env[3],
        d75d_lg_cnt, d75d_lg_utex, d75d_lg_tex,
        (unsigned)d75d_lg_tmem,(unsigned)d75d_lg_fmt,(unsigned)d75d_lg_siz,
        d75d_lg_shifts,d75d_lg_shifft,d75d_lg_uls,d75d_lg_ult,d75d_lg_lrs,d75d_lg_lrt,
        (const void*)d75d_lg_texaddr,(unsigned)d75d_lg_texid,(unsigned)d75d_lg_texbytes,
        d75d_lg_u0,d75d_lg_v0,
        d75d_seq, (d75d_seqlen >= 4096) ? "...[trunc]" : "");
    d75d_seqlen = 0; d75d_maxtri = 0; d75d_vtxloaded = 0; d75d_batchhi = 0; d75d_nv = 0; d75d_nt = 0;
    d75d_rej_triv = 0; d75d_rej_cull = 0; d75d_emitted = 0; d75d_emit_on = 0; d75d_emit_off = 0;
    d75d_ndc_minx=1e9f;d75d_ndc_maxx=-1e9f;d75d_ndc_miny=1e9f;d75d_ndc_maxy=-1e9f;
    d75d_emit_invis = 0; d75d_cmin=999; d75d_cmax=-1; d75d_oml=0xFFFFFFFFu; d75d_oml_set=0;
    d75d_zmin=1e9f;d75d_zmax=-1e9f;d75d_znan=0;d75d_use_tex=-1;
    d75d_comb=0; for(int i=0;i<4;i++){d75d_prim[i]=-1;d75d_env[i]=-1;} d75d_comb_utex=-1;
    d75d_lg_utex=-1; d75d_lg_tex=-1;
}
static void d75d_frame_check(uint32_t f) {
    if (f != d75d_frame) { d75d_flush(f); d75d_frame = f; }
}
static void d75d_note_vtx(uint32_t f, uint32_t count, uint32_t dest) {
    d75d_init();
    if (d75d_lo < 0 || f < (uint32_t)d75d_lo || f > (uint32_t)d75d_hi) return;
    d75d_frame_check(f);
    d75d_nv++;
    d75d_vtxloaded += count;
    if (dest + count > d75d_batchhi) d75d_batchhi = dest + count;
    if (d75d_seqlen < 4096) d75d_seq[d75d_seqlen++] = 'V';
}
/* TEMP D306/D308: env-gated z-fighting FLICKER detector (docs/dev/findings.md
 * §D306/§D308). A coarse grid of screen cells remembers the depth values seen
 * recently at each cell; when a new value arrives that is close to a
 * recently-seen-but-different one (within GE_ZFTOL, default 2 units of 24-bit
 * quantized NDC z), that is the screen-space signature of genuine z-fighting
 * (the winner alternates between two near-equal depths as the camera moves).
 * Tessellation noise (adjacent tris of one surface) does not alternate. Gated
 * on GE_ZF; optional NDC box filter GE_ZFBOX="x0 y0 x1 y1"; caps output per
 * run. Remove once D306/D308 are resolved. */
static int zf_on = -1;
static int zf_tol = 2;
static float zfbox[4] = { -1.05f, -1.05f, 1.05f, 1.05f };
static int zfbox_on = 0;
#define ZF_GRIDX 96
#define ZF_GRIDY 54
#define ZF_HIST 6
struct ZfCell { uint32_t hist[ZF_HIST]; uint8_t n; uint16_t osc; };
static ZfCell zf_grid[ZF_GRIDX * ZF_GRIDY];
static uint32_t zf_hits = 0;
#define ZF_MIN_OSC 5   // a fight must recur this many times at the same cell
static void zf_note_emit(uint32_t f, struct LoadedVertex* const* v_arr) {
    if (zf_on < 0) {
        const char* e = getenv("GE_ZF");
        zf_on = (e && e[0]) ? 1 : 0;
        const char* t = getenv("GE_ZFTOL");
        if (t) zf_tol = atoi(t);
        const char* b = getenv("GE_ZFBOX");
        if (b && sscanf(b, "%f %f %f %f", &zfbox[0], &zfbox[1], &zfbox[2], &zfbox[3]) == 4)
            zfbox_on = 1;
    }
    if (!zf_on || zf_hits >= 600) return;
    float cx=0.f, cy=0.f, czw=0.f, wsum=0.f; int ok=0;
    for (int i = 0; i < 3; i++) {
        float w = v_arr[i]->w;
        if (w <= 0.f) continue;
        cx += (v_arr[i]->x / w) * w; cy += (v_arr[i]->y / w) * w; czw += (v_arr[i]->z / w) * w; wsum += w;
        ok = 1;
    }
    if (!ok || wsum <= 0.f) return;
    cx /= wsum; cy /= wsum; czw /= wsum;
    if (cx < -1.0f || cx > 1.0f || cy < -1.0f || cy > 1.0f) return; // on-screen only
    if (zfbox_on && (cx < zfbox[0] || cx > zfbox[2] || cy < zfbox[1] || cy > zfbox[3])) return;
    uint32_t z24 = (uint32_t)(czw * 255.f); // NDC z in [-1,1] -> 24-bit [0,65535]
    int gx = (int)((cx + 1.0f) * 0.5f * ZF_GRIDX); if (gx >= ZF_GRIDX) gx = ZF_GRIDX - 1;
    int gy = (int)((cy + 1.0f) * 0.5f * ZF_GRIDY); if (gy >= ZF_GRIDY) gy = ZF_GRIDY - 1;
    ZfCell* c = &zf_grid[gy * ZF_GRIDX + gx];
    int dup = 0;
    for (uint8_t k = 0; k < c->n; k++) if (c->hist[k] == z24) { dup = 1; break; }
    if (!dup) {
        // TRUE oscillation: new value close to an OLDER history entry while a
        // different value sat between them (a b a), AND the cell's history is
        // not a monotonic drift (tessellation gradient of one surface).
        int anyup = 0, anydn = 0;
        for (uint8_t k = 1; k < c->n; k++) {
            if (c->hist[k] > c->hist[k - 1]) anyup = 1;
            else if (c->hist[k] < c->hist[k - 1]) anydn = 1;
        }
        if (!(anyup && anydn)) goto zf_push;   // monotonic drift = gradient, not a fight
        for (uint8_t k = 0; k + 1 < c->n; k++) {
            long dz = (long)z24 - (long)c->hist[k];
            if (dz < 0) dz = -dz;
            if (dz > 0 && dz <= (long)zf_tol) {
                uint16_t o = ++c->osc;
                if (o < ZF_MIN_OSC || (o % 10) != 0 && o != ZF_MIN_OSC) goto zf_push;
                zf_hits++;
                fprintf(stderr,
                        "ZF: f=%u z24=%u dprev=%ld osc=%u ndc=(%.3f,%.3f) oml=0x%08x comb=0x%016llx wavg=%.1f\n",
                        f, z24, dz, (unsigned)o, cx, cy, rdp.other_mode_l,
                        (unsigned long long)rdp.combine_mode, wsum / 3.0f);
                if (zf_hits >= 600) return;
            }
        }
zf_push:
        if (c->n < ZF_HIST) c->hist[c->n++] = z24;
        else { memmove(c->hist, c->hist + 1, sizeof(uint32_t) * (ZF_HIST - 1)); c->hist[ZF_HIST - 1] = z24; }
    }
}

/* TEMP D303: env-gated detector for the "extra long flash straight up" quad
 * (M-?? / docs/dev/findings.md §D303). Logs any emitted triangle whose NDC bbox
 * is tall + thin (aspect > 3, height > 25% of frame) and mostly on-screen — the
 * screen-space signature of the reported artifact, whatever its source. Gated
 * on GE_D303; caps output. Remove once D303 is resolved. */
static int d303_on = -1;
static uint32_t d303_hits = 0;
static void d303_note_emit(uint32_t f, struct LoadedVertex* const* v_arr) {
    if (d303_on < 0) { const char* e = getenv("GE_D303"); d303_on = (e && e[0]) ? 1 : 0; }
    if (!d303_on || d303_hits >= 200) return;
    float mnx=1e9f,mxx=-1e9f,mny=1e9f,mxy=-1e9f, wsum = 0.f;
    int ok = 0;
    for (int i = 0; i < 3; i++) {
        float w = v_arr[i]->w;
        if (w <= 0.f) continue;
        wsum += w;
        float nx = v_arr[i]->x / w, ny = v_arr[i]->y / w;
        if (nx<mnx)mnx=nx; if(nx>mxx)mxx=nx; if(ny<mny)mny=ny; if(ny>mxy)mxy=ny;
        ok = 1;
    }
    if (!ok) return;
    float h = mxy - mny, wd = mxx - mnx;
    if (h < 0.45f || wd <= 0.f || h / wd < 3.0f) return;          // VERY tall + thin
    if (mxy < 0.1f || mny > 0.9f) return;                          // must reach upper screen region
    d303_hits++;
    int bl = (rdp.other_mode_l >> 24) & 3u, blc = (rdp.other_mode_l >> 20) & 3u;
    fprintf(stderr,
        "D303: f=%u ndc=(%.3f,%.3f)-(%.3f,%.3f) h=%.3f w=%.3f asp=%.1f bl=%d blc=%d oml=0x%08x comb=0x%016llx prim=(%u,%u,%u) env=(%u,%u,%u) wavg=%.1f c0=(%d,%d,%d)\n",
        f, mnx, mny, mxx, mxy, h, wd, (wd > 0.f ? h / wd : 99.0f), bl, blc,
        rdp.other_mode_l, (unsigned long long)rdp.combine_mode,
        rdp.prim_color.r, rdp.prim_color.g, rdp.prim_color.b,
        rdp.env_color.r, rdp.env_color.g, rdp.env_color.b, wsum / 3.0f,
        v_arr[0]->color.r, v_arr[0]->color.g, v_arr[0]->color.b);
}

/* D474 (low-end perf): one cached gate for the per-triangle / per-vertex debug
 * hooks (D75D, ZF, D303). All three are off for players; without this every
 * triangle paid several calls + frame-counter reads just to return. Same
 * enable semantics as each hook's own lazy init (non-empty env value). */
static int s_tri_dbg = -1;
static inline bool tri_dbg(void) {
    if (s_tri_dbg < 0) {
        const char* a = getenv("GE_D75D");
        const char* b = getenv("GE_ZF");
        const char* c = getenv("GE_D303");
        const char* d = getenv("GE_D526");
        s_tri_dbg = ((a && *a) || (b && *b) || (c && *c) || (d && *d)) ? 1 : 0;
    }
    return s_tri_dbg != 0;
}

static void d75d_note_tri(uint32_t f, uint32_t maxidx) {
    d75d_init();
    if (d75d_lo < 0 || f < (uint32_t)d75d_lo || f > (uint32_t)d75d_hi) return;
    d75d_frame_check(f);
    d75d_nt++;
    if (maxidx > d75d_maxtri) d75d_maxtri = maxidx;
    if (d75d_seqlen < 4096) d75d_seq[d75d_seqlen++] = 'T';
}
static void d75d_note_rej(uint32_t f, int which) {
    d75d_init();
    if (d75d_lo < 0 || f < (uint32_t)d75d_lo || f > (uint32_t)d75d_hi) return;
    d75d_frame_check(f);
    if (which == 0) d75d_rej_triv++; else d75d_rej_cull++;
}
static inline int gfx_lod_tile_offset(const int i); // fwd (defined below; D75D round 2)

static void d75d_note_emit(uint32_t f, struct LoadedVertex* const* v_arr) {
    d75d_init();
    if (d75d_lo < 0 || f < (uint32_t)d75d_lo || f > (uint32_t)d75d_hi) return;
    d75d_frame_check(f);
    d75d_emitted++;
    // NDC bbox of this triangle (x/w, y/w); count on-screen vs off
    float mnx=1e9f,mxx=-1e9f,mny=1e9f,mxy=-1e9f;
    for (int i = 0; i < 3; i++) {
        float w = v_arr[i]->w;
        if (w == 0.f) continue;
        float nx = v_arr[i]->x / w, ny = v_arr[i]->y / w;
        if (nx<mnx)mnx=nx; if(nx>mxx)mxx=nx; if(ny<mny)mny=ny; if(ny>mxy)mxy=ny;
    }
    if (mxx < -1e8f) return; // all w==0, degenerate
    if (mxx >= -1.05f && mnx <= 1.05f && mxy >= -1.05f && mny <= 1.05f) d75d_emit_on++; else d75d_emit_off++;
    if (mnx<d75d_ndc_minx)d75d_ndc_minx=mnx; if(mxx>d75d_ndc_maxx)d75d_ndc_maxx=mxx;
    if (mny<d75d_ndc_miny)d75d_ndc_miny=mny; if(mxy>d75d_ndc_maxy)d75d_ndc_maxy=mxy;
    // invisible flag + oml + vertex color range
    if ((rdp.other_mode_l & (3u << 24)) == ((uint32_t)G_BL_0 << 24) &&
        (rdp.other_mode_l & (3u << 20)) == ((uint32_t)G_BL_CLR_MEM << 20)) d75d_emit_invis++;
    if (!d75d_oml_set) { d75d_oml = rdp.other_mode_l; d75d_oml_set = 1; }
    for (int i = 0; i < 3; i++) {
        int cr=v_arr[i]->color.r, cg=v_arr[i]->color.g, cb=v_arr[i]->color.b;
        if (cr<d75d_cmin)d75d_cmin=cr; if (cr>d75d_cmax)d75d_cmax=cr;
        if (cg<d75d_cmin)d75d_cmin=cg; if (cg>d75d_cmax)d75d_cmax=cg;
        if (cb<d75d_cmin)d75d_cmin=cb; if (cb>d75d_cmax)d75d_cmax=cb;
    }
}
// called at emit with the texunit-0 bound flag
static void d75d_note_emit_z(uint32_t f, struct LoadedVertex* const* v_arr, int has_tex, int comb_utex) {
    d75d_init();
    if (d75d_lo < 0 || f < (uint32_t)d75d_lo || f > (uint32_t)d75d_hi) return;
    d75d_frame_check(f);
    for (int i = 0; i < 3; i++) {
        float w = v_arr[i]->w;
        if (w == 0.f) { d75d_znan++; continue; }
        float nz = v_arr[i]->z / w;
        if (!ge_gfx_isfinite(nz)) { d75d_znan++; continue; }
        if (nz<d75d_zmin)d75d_zmin=nz; if(nz>d75d_zmax)d75d_zmax=nz;
    }
    if (d75d_use_tex < 0) d75d_use_tex = has_tex;
    if (d75d_comb_utex < 0) d75d_comb_utex = comb_utex;
    if (rdp.combine_mode == 0x009ffe4f19ffe4f1ULL) {
        d75d_lg_utex = comb_utex; d75d_lg_tex = has_tex; d75d_lg_cnt++;
        if (!d75d_lg_set && v_arr[0] && v_arr[0]->w != 0.f) {
            d75d_lg_set = 1;
            const uint32_t t0 = rdp.first_tile_index + gfx_lod_tile_offset(0);
            d75d_lg_tmem = rdp.texture_tile[t0].tmem; d75d_lg_fmt = rdp.texture_tile[t0].fmt;
            d75d_lg_siz = rdp.texture_tile[t0].siz;
            d75d_lg_shifts = rdp.texture_tile[t0].shifts; d75d_lg_shifft = rdp.texture_tile[t0].shiftt;
            d75d_lg_uls = rdp.texture_tile[t0].uls; d75d_lg_ult = rdp.texture_tile[t0].ult;
            d75d_lg_lrs = rdp.texture_tile[t0].lrs; d75d_lg_lrt = rdp.texture_tile[t0].lrt;
            if (rendering_state.textures[0]) {
                d75d_lg_texaddr = rendering_state.textures[0]->first.texture_addr;
                d75d_lg_texid = rendering_state.textures[0]->second.texture_id;
                d75d_lg_texbytes = rendering_state.textures[0]->first.size_bytes;
            }
            d75d_lg_u0 = (int)v_arr[0]->u; d75d_lg_v0 = (int)v_arr[0]->v;
        }
    }
    d75d_comb = rdp.combine_mode;
    d75d_prim[0]=rdp.prim_color.r; d75d_prim[1]=rdp.prim_color.g; d75d_prim[2]=rdp.prim_color.b; d75d_prim[3]=rdp.prim_color.a;
    d75d_env[0]=rdp.env_color.r; d75d_env[1]=rdp.env_color.g; d75d_env[2]=rdp.env_color.b; d75d_env[3]=rdp.env_color.a;
}

/* D526 (docs/dev/findings.md §D526): per-triangle draw-state census
 * probe, text-only. Retained as a generic transparency/texture triage
 * tool (the D195/D266/D268/D273 bug class; the P13 Dam-ending grate it
 * targeted, D526, was confirmed faithful N64 behaviour on 1964/GEPD,
 * so no fix was needed). GE_D526="lo-hi" = sim-frame range (the same
 * dash syntax as GE_D75D); optional GE_D526BOX="x0 y0 x1 y1" targets
 * an NDC region (same syntax as GE_ZFBOX; default = on-screen);
 * GE_D526MAX caps per-triangle lines (default 400). For every emitted
 * triangle in range whose NDC bbox intersects the region, one line:
 * frame, NDC bbox, z/w, geometry_mode, other_mode_l/h, combine word,
 * sampled tile 0 (and 1 in 2-cycle) tmem/fmt/siz, first_tile_index,
 * prim alpha, vertex alpha min/max, use_alpha/modulate/invisible.
 * Plus one per-frame line aggregating the in-region tris by
 * (comb, oml, omh, alpha) signature, so a suspect surface's signature
 * stands out without enumerating every tri. Zero cost unless
 * GE_D75D/GE_ZF/GE_D303/GE_D526 is set (folded into the D474 tri_dbg
 * gate). */
static int d526_on = -1;
static int d526_lo = -1, d526_hi = 0x7FFFFFFF, d526_max = 400;
static uint32_t d526_lines = 0;
static float d526_box[4] = { -1.05f, -1.05f, 1.05f, 1.05f };
static int d526_box_on = 0;
#define D526_SIGS 32
static struct { uint64_t comb; uint32_t oml, omh; int alpha; uint32_t cnt; } d526_sigs[D526_SIGS];
static int d526_nsigs = 0;
static uint32_t d526_sig_frame = 0, d526_sig_total = 0;

static void d526_flush_sigs(uint32_t f) {
    if (d526_nsigs == 0) { d526_sig_frame = f; return; }
    fprintf(stderr, "D526F: f=%u inbox=%u sigs: ", f, d526_sig_total);
    for (int i = 0; i < d526_nsigs; i++)
        fprintf(stderr, "(comb=0x%016llx oml=0x%08x omh=0x%08x a=%d)x%u ",
                (unsigned long long)d526_sigs[i].comb, d526_sigs[i].oml,
                d526_sigs[i].omh, d526_sigs[i].alpha, d526_sigs[i].cnt);
    fprintf(stderr, "\n");
    d526_nsigs = 0; d526_sig_total = 0;
    d526_sig_frame = f;
}
static void d526_atexit(void) { d526_flush_sigs(0xFFFFFFFFu); }
static void d526_init(void) {
    if (d526_on >= 0) return;
    d526_on = 0;
    const char* e = getenv("GE_D526");
    if (e && *e) {
        d526_on = 1;
        d526_lo = atoi(e);
        const char* dash = strchr(e, '-');
        if (dash) d526_hi = atoi(dash + 1);
    }
    if (d526_on) {
        const char* m = getenv("GE_D526MAX");
        if (m && *m) d526_max = atoi(m);
        const char* b = getenv("GE_D526BOX");
        if (b && *b && sscanf(b, "%f %f %f %f",
                              &d526_box[0], &d526_box[1], &d526_box[2], &d526_box[3]) == 4)
            d526_box_on = 1;
        atexit(d526_atexit);
    }
}
static void d526_note_emit(uint32_t f, struct LoadedVertex* const* v_arr, int use_alpha, int use_modulate) {
    d526_init();
    if (!d526_on) return;
    if (f < (uint32_t)d526_lo || f > (uint32_t)d526_hi) return;
    if (f != d526_sig_frame) d526_flush_sigs(f);
    float mnx = 1e9f, mxx = -1e9f, mny = 1e9f, mxy = -1e9f;
    float zsum = 0.f, wsum = 0.f; int vmin = 255, vmax = -1, ok = 0;
    for (int i = 0; i < 3; i++) {
        float w = v_arr[i]->w;
        if (w <= 0.f) continue;
        float nx = v_arr[i]->x / w, ny = v_arr[i]->y / w;
        if (nx < mnx) mnx = nx; if (nx > mxx) mxx = nx;
        if (ny < mny) mny = ny; if (ny > mxy) mxy = ny;
        zsum += v_arr[i]->z / w; wsum += w;
        int a = v_arr[i]->color.a;
        if (a < vmin) vmin = a; if (a > vmax) vmax = a;
        ok = 1;
    }
    if (!ok) return;
    if (d526_box_on) {
        if (mxx < d526_box[0] || mnx > d526_box[2] || mxy < d526_box[1] || mny > d526_box[3]) return;
    } else if (mxx < -1.05f || mnx > 1.05f || mxy < -1.05f || mny > 1.05f) return; // off-screen
    d526_sig_total++;
    int found = 0;
    for (int i = 0; i < d526_nsigs; i++) {
        if (d526_sigs[i].comb == rdp.combine_mode && d526_sigs[i].oml == rdp.other_mode_l &&
            d526_sigs[i].omh == rdp.other_mode_h && d526_sigs[i].alpha == use_alpha) {
            d526_sigs[i].cnt++; found = 1; break;
        }
    }
    if (!found && d526_nsigs < D526_SIGS) {
        d526_sigs[d526_nsigs].comb = rdp.combine_mode;
        d526_sigs[d526_nsigs].oml = rdp.other_mode_l;
        d526_sigs[d526_nsigs].omh = rdp.other_mode_h;
        d526_sigs[d526_nsigs].alpha = use_alpha;
        d526_sigs[d526_nsigs].cnt = 1;
        d526_nsigs++;
    }
    if (d526_lines++ >= (uint32_t)d526_max) return;
    const uint32_t fi = rdp.first_tile_index;
    const uint32_t t0 = fi + gfx_lod_tile_offset(0);
    const uint32_t omh = rdp.other_mode_h;
    const int two = ((omh & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE);
    const uint32_t t1 = two ? fi + gfx_lod_tile_offset(1) : t0;
    const int inv = (rdp.other_mode_l & (3u << 24)) == ((uint32_t)G_BL_0 << 24) &&
                    (rdp.other_mode_l & (3u << 20)) == ((uint32_t)G_BL_CLR_MEM << 20);
    fprintf(stderr,
        "D526: f=%u ndc=(%.3f,%.3f)-(%.3f,%.3f) zw=%.4f gm=0x%08x oml=0x%08x omh=0x%08x comb=0x%016llx fi=%u "
        "tile0[tmem=%u fmt=%u siz=%u]",
        f, mnx, mny, mxx, mxy, (wsum > 0.f ? zsum / wsum : -1.f),
        rsp.geometry_mode, rdp.other_mode_l, omh,
        (unsigned long long)rdp.combine_mode, fi,
        rdp.texture_tile[t0].tmem, rdp.texture_tile[t0].fmt, rdp.texture_tile[t0].siz);
    if (two)
        fprintf(stderr, " tile1[tmem=%u fmt=%u siz=%u]",
                rdp.texture_tile[t1].tmem, rdp.texture_tile[t1].fmt, rdp.texture_tile[t1].siz);
    fprintf(stderr, " pa=%u va=[%d,%d] alpha=%d mod=%d inv=%d 2cyc=%d\n",
            rdp.prim_color.a, vmin, vmax, use_alpha, use_modulate, inv, two);
}

/* The loaded-vertex bake cache lives next to gfx_sp_tri1 below. */
static void geTriVertexInvalidated(unsigned slot);

static void gfx_sp_vertex(size_t n_vertices, size_t dest_index, const Vtx* vertices) {
    const size_t d75_di0 = dest_index; /* D75 probe: loop below mutates dest_index */
    SUPPORT_CHECK(n_vertices <= MAX_VERTICES);

    if (!fast3d_ptr_ok(vertices) || dest_index + n_vertices > MAX_VERTICES) {
        return; /* D146 */
    }

#if defined(GE_XBOX_NATIVE32) && GE_XBOX_NATIVE_ROOM_BATCHES
    const bool nativeRoomSource=(s_roomScope>=0 && !s_roomHasColorOverride &&
        !(rsp.geometry_mode&(G_LIGHTING|G_TEXTURE_GEN)));
    const unsigned nativeRoomMatrixStamp=nativeRoomSource?geRoomMatrixStamp():0u;
#endif
    for (size_t i = 0; i < n_vertices; i++, dest_index++) {
        const Vtx* v = &vertices[i];
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_NATIVE_ROOM_BATCHES
        NativeRoomRawSlot &raw=s_roomRawSlot[dest_index];
        raw.valid=nativeRoomSource;
        if(nativeRoomSource) {
            raw.x=(float)v->v.ob[0];raw.y=(float)v->v.ob[1];raw.z=(float)v->v.ob[2];
            raw.materialColor=((uint32_t)v->v.cn[3]<<24)|((uint32_t)v->v.cn[0]<<16)|
                              ((uint32_t)v->v.cn[1]<<8)|((uint32_t)v->v.cn[2]);
            raw.room=(unsigned)s_roomScope;raw.revision=s_roomRevision;
            raw.matrixStamp=nativeRoomMatrixStamp;
        }
#endif
        struct LoadedVertex* d = &rsp.loaded_vertices[dest_index];
        geTriVertexInvalidated((unsigned)dest_index);

        /* GE's Vtx layout (include/PR/gbi.h): ob[] position, tc[] texture
         * coords, and the last 4 bytes are either RGBA (Vtx_t.cn) or a
         * normal + alpha (Vtx_tn.n/a). Unlike PD there is no G_COL colour
         * table: colours/normals live in the vertex itself. */
        float x = v->v.ob[0] * rsp.MP_matrix[0][0] + v->v.ob[1] * rsp.MP_matrix[1][0] + v->v.ob[2] * rsp.MP_matrix[2][0] + rsp.MP_matrix[3][0];
        float y = v->v.ob[0] * rsp.MP_matrix[0][1] + v->v.ob[1] * rsp.MP_matrix[1][1] + v->v.ob[2] * rsp.MP_matrix[2][1] + rsp.MP_matrix[3][1];
        float z = v->v.ob[0] * rsp.MP_matrix[0][2] + v->v.ob[1] * rsp.MP_matrix[1][2] + v->v.ob[2] * rsp.MP_matrix[2][2] + rsp.MP_matrix[3][2];
        float w = v->v.ob[0] * rsp.MP_matrix[0][3] + v->v.ob[1] * rsp.MP_matrix[1][3] + v->v.ob[2] * rsp.MP_matrix[2][3] + rsp.MP_matrix[3][3];

        x = gfx_adjust_x_for_aspect_ratio(x, w);

        short U = v->v.tc[0] * rsp.texture_scaling_factor.s >> 16;
        short V = v->v.tc[1] * rsp.texture_scaling_factor.t >> 16;

        struct NormalColor vcn_data;
        if (rsp.geometry_mode & G_LIGHTING) {
            /* Lit vertices carry their normal in the last 3 bytes. */
            vcn_data.x = (int8_t)v->n.n[0];
            vcn_data.y = (int8_t)v->n.n[1];
            vcn_data.z = (int8_t)v->n.n[2];
            vcn_data.a = v->n.a;
        } else {
            vcn_data.r = v->v.cn[0];
            vcn_data.g = v->v.cn[1];
            vcn_data.b = v->v.cn[2];
            vcn_data.a = v->v.cn[3];
        }
        const struct NormalColor *vcn = &vcn_data;

        if (rsp.geometry_mode & G_LIGHTING) {
            if (rsp.lights_changed) {
                for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                    calculate_normal_dir(&rsp.current_lights[i], rsp.current_lights_coeffs[i]);
                }
                if (rsp.lookat_enabled) {
                    calculate_normal_dir(&rsp.lookat[0], rsp.current_lookat_coeffs[0]);
                    calculate_normal_dir(&rsp.lookat[1], rsp.current_lookat_coeffs[1]);
                }
                rsp.lights_changed = false;
            }

            int r = rsp.current_lights[rsp.current_num_lights - 1].col[0];
            int g = rsp.current_lights[rsp.current_num_lights - 1].col[1];
            int b = rsp.current_lights[rsp.current_num_lights - 1].col[2];

            for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                float intensity = 0;
                intensity += vcn->x * rsp.current_lights_coeffs[i][0];
                intensity += vcn->y * rsp.current_lights_coeffs[i][1];
                intensity += vcn->z * rsp.current_lights_coeffs[i][2];
                intensity /= 127.0f;
                if (intensity > 0.0f) {
                    r += intensity * rsp.current_lights[i].col[0];
                    g += intensity * rsp.current_lights[i].col[1];
                    b += intensity * rsp.current_lights[i].col[2];
                }
            }

            d->color.r = r > 255 ? 255 : r;
            d->color.g = g > 255 ? 255 : g;
            d->color.b = b > 255 ? 255 : b;

            /* D195/D72 correction: GE DOES use RSP-generated (environment-
             * mapped) texture coordinates -- shiny gold/silver weapon skins
             * and Control's reflective console glass all set G_TEXTURE_GEN,
             * the real GBI bit that gates this on real hardware. D72.1
             * originally removed this whole PD-inherited block to fix the
             * Rareware logo (a lit surface that does NOT set G_TEXTURE_GEN,
             * so it was never supposed to hit this path in the first place)
             * but over-corrected: it deleted the feature instead of gating
             * it on the bit that actually distinguishes the two cases. With
             * no envmap UV generation, every G_TEXTURE_GEN draw sampled its
             * texture at whatever raw tc[] happened to be authored (usually
             * ~0), landing on the same corner texel every frame regardless
             * of view angle -- for a typical dark-edged reflection map, a
             * solid near-black surface instead of a shiny/reflective one.
             * Gating on G_TEXTURE_GEN preserves D72.1's actual fix (the logo
             * never sets this bit, so it's unaffected) while restoring the
             * envmap effect for the draws that do. */
            if (rsp.geometry_mode & G_TEXTURE_GEN) {
                float dotx = 0, doty = 0;
                if (rsp.lookat_enabled) {
                    dotx += vcn->x * rsp.current_lookat_coeffs[0][0];
                    dotx += vcn->y * rsp.current_lookat_coeffs[0][1];
                    dotx += vcn->z * rsp.current_lookat_coeffs[0][2];
                    doty += vcn->x * rsp.current_lookat_coeffs[1][0];
                    doty += vcn->y * rsp.current_lookat_coeffs[1][1];
                    doty += vcn->z * rsp.current_lookat_coeffs[1][2];
                    dotx /= 127.0f;
                    doty /= 127.0f;
                } else {
                    float tvcn[3];
                    calculate_normal_dir(vcn, tvcn);
                    dotx = tvcn[0];
                    doty = tvcn[1];
                }

                dotx = clampf(dotx, -1.0f, 1.0f);
                doty = clampf(doty, -1.0f, 1.0f);

                if (rsp.geometry_mode & G_TEXTURE_GEN_LINEAR) {
                    dotx = acosf(-dotx) / 4.0f;
                    doty = acosf(-doty) / 4.0f;
                } else {
                    dotx = (dotx + 1.0f) / 4.0f;
                    doty = (doty + 1.0f) / 4.0f;
                }

                U = (int32_t)(dotx * rsp.texture_scaling_factor.s);
                V = (int32_t)(doty * rsp.texture_scaling_factor.t);
            }
        } else {
            d->color.r = vcn->r;
            d->color.g = vcn->g;
            d->color.b = vcn->b;
        }

        d->u = U;
        d->v = V;

        // trivial clip rejection
        d->clip_rej = 0;
        if (x < -w) {
            d->clip_rej |= 1; // CLIP_LEFT
        }
        if (x > w) {
            d->clip_rej |= 2; // CLIP_RIGHT
        }
        if (y < -w) {
            d->clip_rej |= 4; // CLIP_BOTTOM
        }
        if (y > w) {
            d->clip_rej |= 8; // CLIP_TOP
        }
        // if (z < -w) d->clip_rej |= 16; // CLIP_NEAR
        if (z > w) {
            d->clip_rej |= 32; // CLIP_FAR
        }

        d->x = x;
        d->y = y;
        d->z = z;
        d->w = w;

        if (rsp.geometry_mode & G_FOG) {
            if (fabsf(w) < 0.001f) {
                // To avoid division by zero
                w = 0.001f;
            }

            float winv = 1.0f / w;
            /* D540: the RSP divides by a negative w as-is: a vertex behind
             * the camera gets z/w at/beyond the far end of the range (full
             * fog), continuously. The old override (winv < 0 -> +32767)
             * flipped the sign, so a corner crossing behind the player
             * jumped no-fog <-> full-fog and big ground triangles popped
             * one at a time (Deck playtest). */
            if (!gfx_fog_vertex && winv < 0.0f) {
                winv = 32767;
            }

            float fog_z = z * winv * rsp.fog_mul + rsp.fog_offset;
            d->fog = clampf(fog_z, 0.f, 255.f);
            /* D543 default: the per-vertex RSP value (as the N64; D543's CPU
             * near clip lerps it for clip-created vertices like the RSP).
             * GE_FOGPIXEL=1 (D540): the GPU gets fog * w (z*mul + w*off,
             * linear in clip space) and the shader divides by w -- exact
             * per-pixel fog. The D503 full-fog snap is opt-in (GE_FOGSNAP=1). */
            d->fog_n = gfx_fog_vertex ? d->fog : d->z * rsp.fog_mul + d->w * rsp.fog_offset;
        } else {
            d->fog = rdp.fog_color.a;
            d->fog_n = gfx_fog_vertex ? d->fog : d->fog * d->w;
        }

        d->color.a = vcn->a; // can be required for SHADE_ALPHA even if fog is enabled
    }

#ifdef PORT
    /* TEMP D75 (M-19x): per-batch transformed-vertex range probe. Static
     * analysis of the nintendologo path exhausted itself clean (all 23 op=4
     * nodes visited every frame, GDLs byte-faithful to N64, every opcode
     * handled, G_VTX counts sum exactly to nv, G_TRI4 decode matches the
     * gSP4Triangles macro) -- so log what the pipeline actually computes:
     * for frames in [lo,hi] (env GE_D75V="lo-hi"), one line per G_VTX batch
     * with the resolved vertex ptr, raw first-vertex ob[], transformed
     * x/y/z/w ranges and the MP translation row. The logo's batches are
     * identifiable offline by vtx ptr == BaseAddr(0x70157a98)+file offset.
     * Remove once D75 is root-caused. */
    {
        static int d75v_lo = -1, d75v_hi = 0;
        /* D473: a separate init flag. d75v_lo == -1 also means "disabled", so
         * the old `if (d75v_lo < 0)` re-ran getenv on EVERY G_VTX when the env
         * was unset -- ~55% of render-thread CPU (D302/D250 class). */
        static bool d75v_init = false;
        if (!d75v_init) {
            d75v_init = true;
            const char* v = getenv("GE_D75V");
            d75v_lo = 1; d75v_hi = 0x7fffffff;
            if (!v || sscanf(v, "%d-%d", &d75v_lo, &d75v_hi) != 2)
                d75v_lo = -1;
        }
        if (d75v_lo >= 0 && (int)videoGetFrameCount() >= d75v_lo &&
            (int)videoGetFrameCount() <= d75v_hi) {
            float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f,
                  mnz = 1e30f, mxz = -1e30f, mnw = 1e30f, mxw = -1e30f;
            for (size_t i = 0; i < n_vertices; i++) {
                const struct LoadedVertex* d2 = &rsp.loaded_vertices[d75_di0 + i];
                if (d2->x < mnx) mnx = d2->x; if (d2->x > mxx) mxx = d2->x;
                if (d2->y < mny) mny = d2->y; if (d2->y > mxy) mxy = d2->y;
                if (d2->z < mnz) mnz = d2->z; if (d2->z > mxz) mxz = d2->z;
                if (d2->w < mnw) mnw = d2->w; if (d2->w > mxw) mxw = d2->w;
            }
            const Vtx* v0 = &vertices[0];
            sysLogPrintf(LOG_NOTE,
                "D75V: f=%u vtx=%p n=%zu di=%zu raw0=(%d,%d,%d) x[%.0f,%.0f] y[%.0f,%.0f] z[%.0f,%.0f] w[%.0f,%.0f] MPt=(%.1f,%.1f,%.1f)",
                videoGetFrameCount(), (const void*)vertices, n_vertices, d75_di0,
                (int)v0->v.ob[0], (int)v0->v.ob[1], (int)v0->v.ob[2],
                mnx, mxx, mny, mxy, mnz, mxz, mnw, mxw,
                rsp.MP_matrix[3][0], rsp.MP_matrix[3][1], rsp.MP_matrix[3][2]);
        }
    }
#endif
}

static void gfx_sp_modify_vertex(uint16_t vtx_idx, uint8_t where, uint32_t val) {
    SUPPORT_CHECK(where == G_MWO_POINT_ST);

    int16_t s = (int16_t)(val >> 16);
    int16_t t = (int16_t)val;

    struct LoadedVertex* v = &rsp.loaded_vertices[vtx_idx];
    v->u = s;
    v->v = t;
}

/* Intro blood (M-201): set while a texture rectangle is being drawn. */
static bool s_in_texrect = false;

static inline int gfx_lod_tile_offset(const int i) {
    /* Intro blood (M-201): a texture rectangle outside 2-cycle mode samples
     * exactly the tile its command names -- the RDP only selects LOD tiles in
     * 2-cycle mode. The title leaves G_TL_LOD set from earlier draws, and the
     * D236 detail-base rule below then sent the 1-cycle gun-barrel blood
     * rectangle to a stale tile-1 CI8 declaration (static garbage instead of
     * the drip). Scoped to texrects so 3D LOD/detail paths are untouched. */
    if (s_in_texrect && (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) != G_CYC_2CYCLE)
        return i;
    if (gfx_detail_textures_enabled)
        return ((rdp.tex_lod && !rdp.tex_detail) ? 0 : i);
    // D107: GE has no true detail textures (gfx_detail_textures_enabled is
    // false), but its room GDLs still emit G_TL_LOD + G_TD_DETAIL for
    // mip-mapped textures. The old `rdp.tex_lod ? rdp.tex_detail : i` then
    // returned tex_detail (1) for every texel -> fast3d sampled GE's first
    // mip (tile 1), whose single-LOADBLOCK TMEM slot fast3d never registers
    // -> a magnified crop of the base image (the "blurry blob" ceilings /
    // wall panels in BUNKER1). GE loads the whole mip chain at TMEM 0, so
    // for an LOD texture the base render tile is the only correctly-loaded
    // level: use it.
    //
    // D172: but this must NOT collapse a genuine non-LOD two-texture combine.
    // The explosion/blood/spark particle records (assets/oddtextures.c
    // globalDL_0x078..) set G_TL_TILE (no LOD) + G_CC_INTERFERENCE
    // (TEXEL0 * TEXEL1) and bind tile 0 = IA8 smoke @ TMEM 0, tile 1 = RGBA16
    // fire @ TMEM 0x188. Returning 0 here fed TEXEL1 the smoke texture too
    // (smoke * smoke) -> the magenta/cyan particle colour. Only fold to the
    // base tile when LOD is actually active.
    //
    // D236 pass 26: "GE loads the whole mip chain at TMEM 0" holds for the
    // TEXTURETYPE_LOD / TEXTURETYPE_MIPMAP bindings (texHandleType0 /
    // texHandleType2 both do texWriteLoadToTmemAddr(tex, 0)), but NOT for
    // TEXTURETYPE_DETAIL. texHandleType1 (tex.c) loads the DETAIL texture at
    // TMEM 0 and the real base image at TMEM offset texGetSizeInBytes(tex2,0)
    // on tiles 1+. Folding to tile 0 there samples the detail texture instead
    // of the base image. On Surface 1 that is literally the D236 bug: all four
    // treeline cards (tex1198-1201, RGBA5551 64x17 cut-outs) are type-1
    // bindings whose detail texture is tex2465, an opaque 32x32 IA8 noise
    // tile -- so the treeline draws as an opaque tiled noise wall and the
    // cards themselves are never even imported (pass 25's "zero fmt=0 siz=2
    // imports in 4800 frames").
    //
    // Discriminator: tile fi+1's declared FORMAT. Tiles 1.. of a type-0/type-2
    // binding are LOD levels of tile 0's image, so they necessarily carry the
    // same fmt/siz; a type-1 pair is two unrelated textures and generally does
    // not (Surface 1's cards: tile 0 = tex2465 IA8, tile 1 = tex1198 RGBA16).
    // Deliberately asymmetric -- a mip chain can never trip this, and a detail
    // pair that happens to share a format just keeps today's behaviour -- so
    // the D107 mip case is safe by construction.
    if (rdp.tex_lod) {
        const uint32_t fi = rdp.first_tile_index;
        if (g_detail_base_tile && fi + 1 < 8 &&
            rdp.texture_tile[fi + 1].tmem != rdp.texture_tile[fi].tmem &&
            (rdp.texture_tile[fi + 1].fmt != rdp.texture_tile[fi].fmt ||
             rdp.texture_tile[fi + 1].siz != rdp.texture_tile[fi].siz)) {
            return 1;
        }
        return 0;
    }
    return i;
}

/* Xbox Fast3D state-generation hoist (MK64-X front-end architecture).
 * Geometry opcodes can share RDP depth/combiner/texture/shader configuration.
 * Prepare the material only after the triangle survives culling; do not skip
 * the original material state transitions on the first visible triangle.
 *
 * We retain the original RDP preparation verbatim inside the miss path. The
 * cache stores only the outputs that the vertex baker needs. Vertex math and
 * command ordering remain unchanged. We intentionally do not memoize vertex
 * values here: the game's 3D matrices, lighting and near-plane clipper have
 * different invalidation requirements and must be audited separately. */
struct GEPreparedTriangleState {
    unsigned generation;
    unsigned bake_serial;
    bool use_alpha, use_fog, use_grayscale, use_modulate;
    uint32_t tm;
    uint32_t tex_width[2], tex_height[2], tex_width2[2], tex_height2[2];
    bool wrap_s[2], wrap_t[2];
    float wrap_tw[2], wrap_th[2], wrap_uls[2], wrap_ult[2];
    struct ColorCombiner *comb;
    uint8_t num_inputs;
    bool used_textures[2];
    struct GfxClipParameters clip_parameters;
};
static GEPreparedTriangleState s_ge_prepared_tri;
/* A complete native room packet replaces a TRI run, but the original material
 * commands remain authoritative. Probe one TRI to run material setup only;
 * if the native backend declines, replay that TRI normally. */
static bool s_roomNativePrepareOnly=false;
#ifndef GE_XBOX_FAST3D_MATERIAL_CACHE
#define GE_XBOX_FAST3D_MATERIAL_CACHE 1
#endif
static unsigned s_ge_tri_state_generation=1;
static unsigned s_ge_tri_preparations=0, s_ge_tri_reuses=0;
/* Phase 5F: two triangles of the same RDP rectangle share one material.
 * Only gfx_draw_rectangle() enables the second-half window, after confirming
 * that the first half actually completed material preparation. Never cache
 * across different rectangles or across the temporary RDP viewport/geometry
 * restoration, and never affect ordinary 3D triangles. */
#ifndef GE_XBOX_FAST3D_RECT_PAIR_MATERIAL
#define GE_XBOX_FAST3D_RECT_PAIR_MATERIAL 1
#endif
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_FAST3D_RECT_PAIR_MATERIAL
static bool s_ge_rect_second_half=false;
#endif

#ifndef GE_XBOX_FAST3D_VERTEX_CACHE
#define GE_XBOX_FAST3D_VERTEX_CACHE 1
#endif
/* Reuse the exact packed floats already emitted for a loaded N64 vertex
 * while its RDP material generation and source vertex remain unchanged.
 * Clip-generated vertices and transient 2D rectangle vertices bypass this
 * table (their addresses and attributes are not stable). Cap the memo at
 * 32 floats, the Fast3D emitter's existing per-vertex buffer allocation.
 * Unusual larger layouts remain on the untouched slow path. */
struct GEPreparedVertexBake {
    unsigned generation, count;
    float packed[32];
};
static GEPreparedVertexBake s_ge_vertex_bake[MAX_VERTICES];
static unsigned s_ge_vertex_bakes=0,s_ge_vertex_reuses=0;
static void geTriVertexInvalidated(unsigned slot) {
    if(slot<MAX_VERTICES)s_ge_vertex_bake[slot].generation=0;
}
extern "C" void geNativeFast3DVertexBakeCounters(unsigned* prepared,unsigned* reused) {
    if(prepared)*prepared=s_ge_vertex_bakes;
    if(reused)*reused=s_ge_vertex_reuses;
}
extern "C" unsigned geNativeFast3DVertexCacheEnabled(void) {
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_FAST3D_VERTEX_CACHE
    return 1u;
#else
    return 0u;
#endif
}

static void geTriStateInvalidated() {
    ++s_ge_tri_state_generation;
    if (!s_ge_tri_state_generation) {
        s_ge_tri_state_generation=1;
        s_ge_prepared_tri.generation=0;
    }
}

extern "C" void geNativeFast3DTriSetupCounters(unsigned *prepared, unsigned *reused) {
    if (prepared) *prepared=s_ge_tri_preparations;
    if (reused) *reused=s_ge_tri_reuses;
}
extern "C" unsigned geNativeFast3DTriSetupCacheEnabled(void) {
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_FAST3D_MATERIAL_CACHE
    return 1u;
#else
    return 0u;
#endif
}

/* PH5A BEGIN: Conservative engine-wide portal/scissor early reject.
 * Frustum outcodes above catch triangles entirely outside [-w,+w].  Portal
 * scissors can be much smaller: rejecting a triangle before RDP material
 * preparation avoids texture import, shader setup, vertex packing and D3D
 * submission for pixels the Xbox can never show.  Never skip DL commands,
 * viewport transitions or geometry that intersects any clip boundary.
 *
 * Enable for Xbox builds only; 0 is a byte-for-byte behavior A/B fallback.
 */
#ifndef GE_XBOX_FAST3D_EARLY_SCISSOR_REJECT
#define GE_XBOX_FAST3D_EARLY_SCISSOR_REJECT 1
#endif
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_FAST3D_EARLY_SCISSOR_REJECT
static unsigned s_ge_early_scissor_rejects=0;
extern "C" unsigned geNativeFast3DEarlyScissorRejects(void) {
    return s_ge_early_scissor_rejects;
}
static bool geEarlyScissorReject(const LoadedVertex *a,
                                  const LoadedVertex *b,
                                  const LoadedVertex *c) {
    /* Lazy state cache. Recalculate the four homogeneous clip boundaries
     * ONLY when a viewport, portal scissor or output dimension changes,
     * not for every triangle. Rect helpers can modify the RDP structures
     * directly, so checking the values is safer than an opcode-only epoch. */
    static bool cached=false, active=false;
    static struct XYWidthHeight lastViewport,lastScissor;
    static unsigned lastWidth=0,lastHeight=0;
    static float left=0,right=0,bottom=0,top=0;
    if (fbActive)return false; /* offscreen framebuffer paths unchanged */
    if (!cached || lastWidth!=gfx_current_dimensions.width ||
        lastHeight!=gfx_current_dimensions.height ||
        memcmp(&lastViewport,&rdp.viewport,sizeof(lastViewport)) ||
        memcmp(&lastScissor,&rdp.scissor,sizeof(lastScissor))) {
        cached=true;active=false;
        lastViewport=rdp.viewport;lastScissor=rdp.scissor;
        lastWidth=gfx_current_dimensions.width;
        lastHeight=gfx_current_dimensions.height;
        /* The Xbox backend maps clip vertices into a top-left D3D viewport.
         * gfx_pc stores viewport and scissor in bottom-left pixel coords;
         * NDC x/y therefore map with the SAME + sign on both axes.
         * If driver clamping might be required, preserve the original path. */
        if (rdp.viewport.width && rdp.viewport.height &&
            rdp.scissor.width && rdp.scissor.height &&
            rdp.viewport.x >= 0 && rdp.viewport.y >= 0 &&
            (float)rdp.viewport.x+(float)rdp.viewport.width <= (float)lastWidth &&
            (float)rdp.viewport.y+(float)rdp.viewport.height <= (float)lastHeight) {
            const float vx=(float)rdp.viewport.x,vy=(float)rdp.viewport.y;
            const float vw=(float)rdp.viewport.width,vh=(float)rdp.viewport.height;
            /* Two physical pixels of guard on EVERY side: only wholly
             * invisible geometry outside the enlarged portal is rejected. */
            const float pad=2.0f;
            const float sx0=(float)rdp.scissor.x-pad;
            const float sx1=(float)rdp.scissor.x+(float)rdp.scissor.width+pad;
            const float sy0=(float)rdp.scissor.y-pad;
            const float sy1=(float)rdp.scissor.y+(float)rdp.scissor.height+pad;
            /* A full-screen scissor or a 1-pixel edge inset needs no test. */
            if (!(sx0<=vx && sx1>=vx+vw && sy0<=vy && sy1>=vy+vh)) {
                left=2.0f*(sx0-vx)/vw-1.0f;
                right=2.0f*(sx1-vx)/vw-1.0f;
                bottom=2.0f*(sy0-vy)/vh-1.0f;
                top=2.0f*(sy1-vy)/vh-1.0f;
                active=ge_gfx_isfinite(left) && ge_gfx_isfinite(right) &&
                    ge_gfx_isfinite(bottom) && ge_gfx_isfinite(top);
            }
        }
    }
    if (!active)return false;
    /* No reject across or behind the eye plane. Clip/Fog/FVF behavior
     * stays with the original near-plane and backend implementations. */
    if (!(a->w>0.0f && b->w>0.0f && c->w>0.0f))return false;
    /* Inclusive clip math in homogeneous coordinates. Only when all three
     * vertices lie beyond the SAME plane is the entire triangle invisible.
     * Test finiteness only on a prospective rejection: most triangles pass. */
    const bool outside=
        (a->x<left*a->w && b->x<left*b->w && c->x<left*c->w) ||
        (a->x>right*a->w && b->x>right*b->w && c->x>right*c->w) ||
        (a->y<bottom*a->w && b->y<bottom*b->w && c->y<bottom*c->w) ||
        (a->y>top*a->w && b->y>top*b->w && c->y>top*c->w);
    if (!outside)return false;
    return ge_gfx_isfinite(a->w) && ge_gfx_isfinite(b->w) && ge_gfx_isfinite(c->w) &&
        ge_gfx_isfinite(a->x) && ge_gfx_isfinite(b->x) && ge_gfx_isfinite(c->x) &&
        ge_gfx_isfinite(a->y) && ge_gfx_isfinite(b->y) && ge_gfx_isfinite(c->y);
}
#endif /* GE_XBOX_FAST3D_EARLY_SCISSOR_REJECT */
/* PH5A END */

/* PH5B: compare the projected triangle orientation without reciprocal-W.
 * (x1/w1-x2/w2)*(y3/w3-y2/w2)-(y1/w1-y2/w2)*(x3/w3-x2/w2)
 * has the sign of the numerator below when all W are positive. Mixed/zero W,
 * nonfinite values and near-degenerate orientations must use the original
 * divisions and exact N64-facing tests, including the negative-W sign rule.
 * Does not alter the submitted vertex coordinates, clipping or draw order. */
#ifndef GE_XBOX_FAST3D_HOMOGENEOUS_BACKFACE
#define GE_XBOX_FAST3D_HOMOGENEOUS_BACKFACE 1
#endif
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_FAST3D_HOMOGENEOUS_BACKFACE
static bool geTryHomogeneousBackface(const struct LoadedVertex *v1,
                                     const struct LoadedVertex *v2,
                                     const struct LoadedVertex *v3,
                                     float *crossOut) {
    const float w1=v1->w, w2=v2->w, w3=v3->w;
    /* Avoid catastrophic precision loss when multiplying tiny/huge W. */
    if (!(w1>0.0001f && w2>0.0001f && w3>0.0001f) ||
        !ge_gfx_isfinite(w1) || !ge_gfx_isfinite(w2) || !ge_gfx_isfinite(w3) ||
        !ge_gfx_isfinite(v1->x) || !ge_gfx_isfinite(v1->y) ||
        !ge_gfx_isfinite(v2->x) || !ge_gfx_isfinite(v2->y) ||
        !ge_gfx_isfinite(v3->x) || !ge_gfx_isfinite(v3->y))return false;
    const float abx=v1->x*w2-v2->x*w1;
    const float aby=v1->y*w2-v2->y*w1;
    const float cbx=v3->x*w2-v2->x*w3;
    const float cby=v3->y*w2-v2->y*w3;
    const float term1=abx*cby,term2=aby*cbx;
    const float numerator=term1-term2;
    const float magnitude=fabsf(term1)+fabsf(term2);
    /* Sign only. Treat cancellation as uncertain and compute the old result. */
    if (!ge_gfx_isfinite(numerator) || !ge_gfx_isfinite(magnitude) ||
        !(magnitude>0.0f) || !(fabsf(numerator)>magnitude*0.00001f))return false;
    *crossOut=numerator;
    return true;
}
#endif

static void gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx, bool is_rect) {
    GE_P0E_SCOPE(GE_P0E_TRIANGLE);
    /* D75D: only real model triangles (all indices < MAX_VERTICES); the
     * fullscreen-quad helpers use indices MAX_VERTICES+0..3 and would skew maxtri. */
    if (vtx1_idx < MAX_VERTICES && vtx2_idx < MAX_VERTICES && vtx3_idx < MAX_VERTICES) {
        uint32_t d75d_mx = vtx1_idx > vtx2_idx ? (vtx1_idx > vtx3_idx ? vtx1_idx : vtx3_idx)
                                               : (vtx2_idx > vtx3_idx ? vtx2_idx : vtx3_idx);
        if (tri_dbg()) d75d_note_tri(videoGetFrameCount(), d75d_mx);
    }
    struct LoadedVertex* v1 = &rsp.loaded_vertices[vtx1_idx];
    struct LoadedVertex* v2 = &rsp.loaded_vertices[vtx2_idx];
    struct LoadedVertex* v3 = &rsp.loaded_vertices[vtx3_idx];
    struct LoadedVertex* v_arr[3] = { v1, v2, v3 };

    if (!s_roomNativePrepareOnly && (rsp.extra_geometry_mode & G_NO_CLIPPING_EXT) == 0) {
        /* D233: the outcode bits set in gfx_sp_vertex (x<-w / x>w / y<-w /
         * y>w / z>w) are only valid half-space tests when w>0. A vertex
         * that has crossed behind the camera plane (w<0) flips the sense of
         * those comparisons, so its clip_rej bits can come out wrong. If
         * that spurious bit happens to match the other two (genuinely
         * correct) vertices' bits, the AND-reduction below fires and drops
         * the whole triangle before it ever reaches the GPU -- even though
         * OpenGL's own homogeneous clipper (which handles w<0 correctly)
         * would have rendered the visible portion of it. This hits large
         * polygons near the camera (room walls/ceilings near a doorway,
         * where the camera is close enough for a vertex to sit behind it)
         * far more than small prop models, matching D233's "room geometry
         * intermittently vanishes near doors, furniture/terminals still
         * draw" report. Fix: never trust the trivial-reject AND test when
         * any vertex has a negative w -- defer to GL's clipper instead,
         * same as the backface-cull code just below already does for the
         * same w-sign hazard. Worst case for the (rare) mixed-sign case is
         * a few extra triangles reaching the GPU; never fewer. */
        bool any_behind_camera = (v1->w < 0) || (v2->w < 0) || (v3->w < 0);
        if (!any_behind_camera && (v1->clip_rej & v2->clip_rej & v3->clip_rej)) {
            // The whole triangle lies outside the visible area
            if (tri_dbg()) d75d_note_rej(videoGetFrameCount(), 0);
            return;
        }
    }

    if (!s_roomNativePrepareOnly && (rsp.geometry_mode & G_CULL_BOTH) != 0) {
        float cross;
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_FAST3D_HOMOGENEOUS_BACKFACE
        const bool geFastBackface=geTryHomogeneousBackface(v1,v2,v3,&cross);
#else
        const bool geFastBackface=false;
#endif
        if (!geFastBackface) {
            /* Original projected-divide path for all uncertain geometries. */
            float dx1 = v1->x / (v1->w) - v2->x / (v2->w);
            float dy1 = v1->y / (v1->w) - v2->y / (v2->w);
            float dx2 = v3->x / (v3->w) - v2->x / (v2->w);
            float dy2 = v3->y / (v3->w) - v2->y / (v2->w);
            cross = dx1 * dy2 - dy1 * dx2;
            if ((v1->w < 0) ^ (v2->w < 0) ^ (v3->w < 0)) {
                // Preserve original mixed-sign W behavior.
                cross = -cross;
            }
        }

        // If inverted culling is requested, negate the cross
        // if ((rsp.extra_geometry_mode & G_EX_INVERT_CULLING) == 1) {
        //     cross = -cross;
        // }

        switch (rsp.geometry_mode & G_CULL_BOTH) {
            case G_CULL_FRONT:
                if (cross <= 0) {
                    if (tri_dbg()) d75d_note_rej(videoGetFrameCount(), 1);
                    return;
                }
                break;
            case G_CULL_BACK:
                if (cross >= 0) {
                    if (tri_dbg()) d75d_note_rej(videoGetFrameCount(), 1);
                    return;
                }
                break;
            case G_CULL_BOTH:
                // Why is this even an option?
                if (tri_dbg()) d75d_note_rej(videoGetFrameCount(), 1);
                return;
        }
    }

#if defined(GE_XBOX_NATIVE32) && GE_XBOX_FAST3D_EARLY_SCISSOR_REJECT
    /* Before material setup (texture conversion/shader lookup), preserve
     * full state execution for uncertain cases and for diagnostic probes. */
    if (!s_roomNativePrepareOnly && !is_rect && !(rsp.extra_geometry_mode & G_NO_CLIPPING_EXT) &&
        !tri_dbg() && geEarlyScissorReject(v1,v2,v3)) {
        ++s_ge_early_scissor_rejects;
        return;
    }
#endif

    /* Respect the original N64 state-update ordering. All non-geometry DL
     * opcodes advance a generation in gfx_run_dl; the frame and render targets
     * also invalidate it. Imported texture or viewport dirtiness is checked
     * explicitly because those flags may be changed by non-DL callers. */
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_FAST3D_MATERIAL_CACHE
    static int geTriProbes=-1;
    if (geTriProbes<0)
        geTriProbes=(getenv("GE_D172")!=NULL || getenv("GE_D229")!=NULL) ? 1 : 0;
    /* A rectangle temporarily overrides RDP viewport/geometry. Preserve
     * forced setup for its first triangle, but the immediately following
     * triangle can reuse that *same* prepared material if the first half
     * reached the end of material setup and no texture/viewport flag changed.
     * The prepared state's generation remains zero, so it cannot leak into
     * the next rectangle or ordinary geometry. */
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_FAST3D_RECT_PAIR_MATERIAL
    const bool rectForcesSetup = is_rect && !s_ge_rect_second_half;
    const bool generationDiffers = !is_rect &&
        s_ge_prepared_tri.generation!=s_ge_tri_state_generation;
    const bool triNeedsSetup = rectForcesSetup || geTriProbes || generationDiffers ||
#else
    const bool triNeedsSetup = is_rect || geTriProbes ||
        s_ge_prepared_tri.generation!=s_ge_tri_state_generation ||
#endif
        (s_ge_prepared_tri.comb &&
         ((s_ge_prepared_tri.comb->used_textures[0] && rdp.textures_changed[0]) ||
          (s_ge_prepared_tri.comb->used_textures[1] && rdp.textures_changed[1]))) ||
        rdp.viewport_or_scissor_changed;
#else
    /* Keep desktop PC-port behaviour unchanged for reference comparisons. */
    const bool triNeedsSetup = true;
#endif
    if (triNeedsSetup) {
    GE_P0E_SCOPE(GE_P0E_MATERIAL);
    bool depth_test = ((rsp.geometry_mode & G_ZBUFFER) == G_ZBUFFER || (rdp.other_mode_l & G_ZS_PRIM) == G_ZS_PRIM) &&
                      ((rdp.other_mode_h & G_CYC_1CYCLE) == G_CYC_1CYCLE || (rdp.other_mode_h & G_CYC_2CYCLE) == G_CYC_2CYCLE);
    bool depth_update = (rdp.other_mode_l & Z_UPD) == Z_UPD;
    bool depth_compare = (rdp.other_mode_l & Z_CMP) == Z_CMP;
    bool depth_source_prim = (rdp.other_mode_l & G_ZS_PRIM) == G_ZS_PRIM /* && gDP.primDepth.z == 1.0f */;
    uint16_t zmode = rdp.other_mode_l & ZMODE_DEC;
    uint8_t depth_mode = (depth_test ? 1 : 0) | (depth_update ? 2 : 0) | (depth_compare ? 4 : 0) | (depth_source_prim ? 8 : 0) | (zmode >> 6);

    if (depth_mode != rendering_state.depth_mode) {
        gfx_flush();
        gfx_rapi->set_depth_mode(depth_test, depth_update, depth_compare, depth_source_prim, zmode);
        rendering_state.depth_mode = depth_mode;
    }

    if (rdp.viewport_or_scissor_changed) {
        if (memcmp(&rdp.viewport, &rendering_state.viewport, sizeof(rdp.viewport)) != 0) {
            gfx_flush();
            gfx_rapi->set_viewport(rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
            rendering_state.viewport = rdp.viewport;
        }
        if (memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_flush();
            gfx_rapi->set_scissor(rdp.scissor.x, rdp.scissor.y, rdp.scissor.width, rdp.scissor.height);
            rendering_state.scissor = rdp.scissor;
        }
        rdp.viewport_or_scissor_changed = false;
    }

    uint64_t cc_options = 0;
    bool use_alpha =
        (rdp.other_mode_l & (3 << 20)) == (G_BL_CLR_MEM << 20) && (rdp.other_mode_l & (3 << 16)) == (G_BL_1MA << 16);
    const bool use_fog = ((rdp.other_mode_l >> 30) == G_BL_CLR_FOG) || ((rdp.other_mode_l >> 26) == G_BL_A_FOG);
    /* D266: G_AC_DECAL (alphacompare == 2) never discards, so it must not
     * take the texedge path at all; NONE/THRESHOLD/DITHER are distinguished
     * in gfx_opengl.cpp via the existing options. */
    const bool ac_decal = (rdp.other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == (2U << G_MDSFT_ALPHACOMPARE);
    const bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA && !ac_decal;
    const bool use_noise = (rdp.other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == G_AC_DITHER;
    const bool use_2cyc = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE;
    const bool alpha_threshold = (rdp.other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == G_AC_THRESHOLD;
    const bool invisible = (rdp.other_mode_l & (3 << 24)) == (G_BL_0 << 24) && (rdp.other_mode_l & (3 << 20)) == (G_BL_CLR_MEM << 20);
    const bool use_grayscale = rdp.grayscale;
    const bool use_modulate = use_alpha && (rsp.extra_geometry_mode & G_MODULATE_EXT) != 0;
    const bool use_blur = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) == G_TF_BLUR_EXT;

    if ((rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA) {
        use_alpha = true;
    }

    if (use_alpha) {
        cc_options |= (uint64_t)SHADER_OPT_ALPHA;
    }
    if (use_fog) {
        cc_options |= (uint64_t)SHADER_OPT_FOG;
    }
    if (texture_edge) {
        cc_options |= (uint64_t)SHADER_OPT_TEXTURE_EDGE;
    }
    if (use_noise) {
        cc_options |= (uint64_t)SHADER_OPT_NOISE;
    }
    if (use_2cyc) {
        cc_options |= (uint64_t)SHADER_OPT_2CYC;
    }
    if (alpha_threshold) {
        cc_options |= (uint64_t)SHADER_OPT_ALPHA_THRESHOLD;
    }
    if (invisible) {
        cc_options |= (uint64_t)SHADER_OPT_INVISIBLE;
    }
    if (use_grayscale) {
        cc_options |= (uint64_t)SHADER_OPT_GRAYSCALE;
    }
    if (use_blur) {
        cc_options |= (uint64_t)SHADER_OPT_BLUR;
    }

    // If we are not using alpha, clear the alpha components of the combiner as they have no effect
    if (!use_alpha) {
        cc_options &= ~((0xfff << 16) | ((uint64_t)0xfff << 44));
    }

    ColorCombinerKey key;
    key.combine_mode = rdp.combine_mode;
    key.options = cc_options;

    ColorCombiner* comb = gfx_lookup_or_create_color_combiner(key);

    uint32_t tm = 0;
    uint32_t tex_width[2], tex_height[2], tex_width2[2], tex_height2[2];

    /* D74 (Video.WrapFix): per-texunit pre-wrap window. N64 wraps a render
     * tile's UVs at the TILE period (uls/ult + lrs/lrt window) when the tile
     * is a sub-region of the uploaded image; GL wraps at the full image size.
     * Computed once per texunit here (needs the ORIGINAL cms/cmt, before the
     * CLAMP-clearing below) and applied per vertex in the loop. Opt-in --
     * this path never ran before (the old guard was `cms & G_TX_WRAP` ==
     * `& 0`), so it is new behaviour. */
    bool  wrap_s[2] = { false, false }, wrap_t[2] = { false, false };
    float wrap_tw[2] = { 0, 0 }, wrap_th[2] = { 0, 0 };
    float wrap_uls[2] = { 0, 0 }, wrap_ult[2] = { 0, 0 };

    for (int i = 0; i < 2; i++) {
        // TODO: fix this; for now just ignore smaller mips
        const uint32_t tile = rdp.first_tile_index + gfx_lod_tile_offset(i);
        if (comb->used_textures[i]) {
            if (rdp.textures_changed[i]) {
                gfx_flush();
                import_texture(i, tile, false);
                rdp.textures_changed[i] = false;
            }

            uint8_t cms = rdp.texture_tile[tile].cms;
            uint8_t cmt = rdp.texture_tile[tile].cmt;

            uint32_t tex_size_bytes = rdp.loaded_texture[rdp.texture_tile[tile].tmem].orig_size_bytes;
            uint32_t line_size = rdp.texture_tile[tile].line_size_bytes;

            if (line_size == 0) {
                line_size = 1;
            }

            tex_height[i] = tex_size_bytes / line_size;
            switch (rdp.texture_tile[tile].siz) {
                case G_IM_SIZ_4b:
                    line_size <<= 1;
                    break;
                case G_IM_SIZ_8b:
                    break;
                case G_IM_SIZ_16b:
                    line_size /= G_IM_SIZ_16b_LINE_BYTES;
                    break;
                case G_IM_SIZ_32b:
                    line_size /= G_IM_SIZ_32b_LINE_BYTES; // this is 2!
                    tex_height[i] /= 2;
                    break;
            }
            tex_width[i] = line_size;
            /* D245 (M-201): D229 water (RGBA16 tile over a CI8 load) is
             * imported as CI8 -- one byte per texel, base level only -- so the
             * UV normalisation must use that geometry: width = line bytes
             * (32, not 32/2 = 16, which doubled the S frequency) and height =
             * the mask period (32, not 1400/32 = 43 rows of mip chain). */
            {
                const auto& ltx = rdp.loaded_texture[rdp.texture_tile[tile].tmem];
                if (rdp.texture_tile[tile].fmt == G_IM_FMT_RGBA &&
                    rdp.texture_tile[tile].siz == G_IM_SIZ_16b && ltx.src_fmt == G_IM_FMT_CI) {
                    const uint8_t mkt = rdp.texture_tile[tile].maskt;
                    tex_width[i] = rdp.texture_tile[tile].line_size_bytes;
                    if (mkt > 0 && mkt < 12 && tex_height[i] > (1u << mkt)) tex_height[i] = 1u << mkt;
                }
            }

            tex_width2[i] = (rdp.texture_tile[tile].lrs - rdp.texture_tile[tile].uls + 4) / 4;
            tex_height2[i] = (rdp.texture_tile[tile].lrt - rdp.texture_tile[tile].ult + 4) / 4;

            uint32_t tex_width1 = tex_width[i] << (cms & G_TX_MIRROR);
            uint32_t tex_height1 = tex_height[i] << (cmt & G_TX_MIRROR);

            if (g_wrap_fix && !(cms & G_TX_MIRROR)) {
                /* (a) sub-tile window: wrap tile (clamp bit not set) whose
                 * uls..lrs window is smaller than the uploaded image -> pre-fmod
                 * the UVs at the window size. */
                if (!(cms & G_TX_CLAMP) && tex_width2[i] > 0 && tex_width2[i] < tex_width[i]) {
                    wrap_s[i]   = true;
                    wrap_tw[i]  = (float)tex_width2[i];
                    wrap_uls[i] = rdp.texture_tile[tile].uls / 4.0f;
                }
                if (!(cmt & G_TX_CLAMP) && tex_height2[i] > 0 && tex_height2[i] < tex_height[i]) {
                    wrap_t[i]   = true;
                    wrap_th[i]  = (float)tex_height2[i];
                    wrap_ult[i] = rdp.texture_tile[tile].ult / 4.0f;
                }
                /* (b) RC3 non-power-of-two wrap period: the N64 RDP masks the
                 * texel coordinate at 1<<mask (GE sets mask = ceil(log2(dim)),
                 * texDimensionToMask), so a non-PoT tile repeats at the NEXT
                 * power of two, not at its image size the way GL GL_REPEAT does.
                 * Fold the UV at the N64 period; the [dim, 1<<mask) overflow
                 * band (TMEM smear on console) is clamped to the last texel so
                 * it reads as an edge streak instead of a bogus early repeat. */
                {
                    uint8_t mks = rdp.texture_tile[tile].masks;
                    uint8_t mkt = rdp.texture_tile[tile].maskt;
                    if (!wrap_s[i] && !(cms & G_TX_CLAMP) && mks >= 1 && mks <= 14) {
                        float period = (float)(1u << mks);
                        if (period != (float)tex_width[i] && tex_width[i] > 0) {
                            wrap_s[i]   = true;
                            wrap_tw[i]  = period;
                            wrap_uls[i] = 0.0f;
                        }
                    }
                    if (!wrap_t[i] && !(cmt & G_TX_CLAMP) && mkt >= 1 && mkt <= 14) {
                        float period = (float)(1u << mkt);
                        if (period != (float)tex_height[i] && tex_height[i] > 0) {
                            wrap_t[i]   = true;
                            wrap_th[i]  = period;
                            wrap_ult[i] = 0.0f;
                        }
                    }
                }
            }

            if ((cms & G_TX_CLAMP) && ((cms & G_TX_MIRROR) || tex_width1 != tex_width2[i])) {
                tm |= 1 << 2 * i;
                cms &= ~G_TX_CLAMP;
            }
            if ((cmt & G_TX_CLAMP) && ((cmt & G_TX_MIRROR) || tex_height1 != tex_height2[i])) {
                tm |= 1 << (2 * i + 1);
                cmt &= ~G_TX_CLAMP;
            }

            if (rendering_state.textures[i]) {
                bool linear_filter = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
                if (linear_filter != rendering_state.textures[i]->second.linear_filter ||
                    cms != rendering_state.textures[i]->second.cms || cmt != rendering_state.textures[i]->second.cmt) {
                    gfx_flush();
                    gfx_rapi->set_sampler_parameters(i, linear_filter, cms, cmt, rdp.tex_lod);
                    rendering_state.textures[i]->second.linear_filter = linear_filter;
                    rendering_state.textures[i]->second.cms = cms;
                    rendering_state.textures[i]->second.cmt = cmt;
                }
            }

#ifdef PORT
            /* D229 probe (env-gated, inert): green/pulsating IsWater water.
             * The water quad is a 2-cycle LERP(TEXEL1, TEXEL0) draw binding
             * BOTH tiles to the same TMEM with tile 1 offset by uls/ult --
             * the D172 probe's `tmem differs` filter never fires for it. Dump
             * each texunit's full decoded tile state + wrap decision + GL
             * texture identity so one Frigate capture pins whether TEXEL1
             * resolves to the same image/region as TEXEL0 or drifts into
             * other TMEM content (the green). */
            static int ge_d229_b = -1;
            if (ge_d229_b < 0) ge_d229_b = getenv("GE_D229") != NULL;
            if (ge_d229_b && use_2cyc && comb->used_textures[0] && comb->used_textures[1]) {
                static int d229x = 0;
                static int d229_total = 0;
                d229_total++;
                if (d229x < 12) {
                    d229x++;
                    const uint32_t tmem = rdp.texture_tile[tile].tmem;
                    sysLogPrintf(LOG_NOTE,
                        "D229: texunit%d first=%u lodoff=%u tile=%u tmem=%u fmt=%u siz=%u | "
                        "uls=%u ult=%u lrs=%u lrt=%u masks=%u maskt=%u shifts=%d shiftt=%d cms=%u cmt=%u | "
                        "texw=%u texh=%u texw2=%u texh2=%u | "
                        "wrapS=%d(tw=%.1f,uls=%.1f) wrapT=%d(th=%.1f,ult=%.1f) | "
                        "tmembytes=%u GLaddr=%p texid=%u glbytes=%u same_tmem_as_other=%d",
                        i, rdp.first_tile_index, gfx_lod_tile_offset(i), tile, tmem,
                        rdp.texture_tile[tile].fmt, rdp.texture_tile[tile].siz,
                        rdp.texture_tile[tile].uls, rdp.texture_tile[tile].ult,
                        rdp.texture_tile[tile].lrs, rdp.texture_tile[tile].lrt,
                        rdp.texture_tile[tile].masks, rdp.texture_tile[tile].maskt,
                        (int)rdp.texture_tile[tile].shifts, (int)rdp.texture_tile[tile].shiftt,
                        (unsigned)cms, (unsigned)cmt,
                        tex_width[i], tex_height[i], tex_width2[i], tex_height2[i],
                        (int)wrap_s[i], wrap_tw[i], wrap_uls[i],
                        (int)wrap_t[i], wrap_th[i], wrap_ult[i],
                        rdp.loaded_texture[tmem].orig_size_bytes,
                        rendering_state.textures[i] ? (const void*)rendering_state.textures[i]->first.texture_addr : nullptr,
                        rendering_state.textures[i] ? rendering_state.textures[i]->second.texture_id : 0u,
                        rendering_state.textures[i] ? rendering_state.textures[i]->first.size_bytes : 0u,
                        (int)(rdp.texture_tile[rdp.first_tile_index + gfx_lod_tile_offset(1 - i)].tmem == tmem));
                } else if (d229_total % 400 == 0) {
                    sysLogPrintf(LOG_NOTE, "D229: (summary) %d dual-texunit 2-cycle tris so far", d229_total);
                }
            }
#endif
        }
    }

#ifdef PORT
    /* D172 probe (env-gated, inert): the magenta/cyan blood/spark bug. The
     * particle records (assets/oddtextures.c globalDL_0x078..) draw a 2-cycle
     * G_CC_INTERFERENCE combine (TEXEL0*TEXEL1) binding two tiles of two
     * formats - tile 0 IA8 smoke, tile 1 RGBA16 fire @ tmem 0x188. Dump both
     * texunits' tile state whenever a 2-cycle tri actually consumes TEXEL1, so
     * one Silo capture pins whether TEXEL1 resolves to the right tmem/format. */
    static int ge_d172_a = -1;
    if (ge_d172_a < 0) ge_d172_a = getenv("GE_D172") != NULL;
    if (ge_d172_a && use_2cyc && comb->used_textures[1] && !rdp.tex_lod) {
        const uint32_t fi = rdp.first_tile_index;
        /* the tile fast3d will actually SAMPLE for each texunit */
        const uint32_t s0 = fi + gfx_lod_tile_offset(0);
        const uint32_t s1 = fi + gfx_lod_tile_offset(1);
        /* the tile the DL actually CONFIGURED for texunit 1 */
        const uint32_t c1 = fi + 1;
        /* only interesting when the DL set up a genuinely distinct 2nd tile
         * (different tmem) -- filters out mip/LOD-bilerp false positives */
        if (rdp.texture_tile[c1].tmem != rdp.texture_tile[fi].tmem) {
            static int d172x = 0;
            static int d172_total = 0;
            /* D219: the original 40-hit cap (sized for M-90's short scripted
             * repro) silently went dark for the rest of a real play session
             * after ~40 ordinary bullet-impact particles, well before any
             * actual explosion -- and the silence itself was indistinguishable
             * from "no more particle draws happened at all". Never suppress
             * the interesting (lit) case, and keep an always-on counter with
             * periodic summaries so a long session still proves whether this
             * code path is being hit throughout, not just early on. */
            const bool lit = (rsp.geometry_mode & G_LIGHTING) != 0;
            d172_total++;
            if (!lit && d172x >= 40) {
                if (d172_total % 200 == 0) {
                    sysLogPrintf(LOG_NOTE, "D172: (summary) %d multitex tris seen so far, still LIGHTING=off", d172_total);
                }
            } else {
                d172x++;
                sysLogPrintf(LOG_NOTE,
                    "D172: multitex tri combine=%llx tex_lod=%d first=%u | "
                    "cfg1[tile=%u tmem=%u fmt=%u siz=%u] | "
                    "SAMPLED0=tile%u(tmem=%u) SAMPLED1=tile%u(tmem=%u fmt=%u)%s | "
                    /* D219 (docs/dev/findings.md): explosion.c never clears
                     * G_LIGHTING before drawing these particle billboards -
                     * it relies on whatever last set it. If lighting is ON
                     * here, cn[]'s authored RGBA tint is misread as a normal
                     * vector (gfx_pc.cpp:1301-1346) and SHADE becomes a
                     * scene-light color instead of red/orange, which would
                     * explain a state-dependent purple/blue that a synthetic
                     * scripted repro (M-90) might not reproduce. */
                    "geometry_mode=%08x LIGHTING=%s | "
                    /* D219 round 2: G_LIGHTING is ruled out (5000/5000
                     * samples off, live-confirmed still purple/blue). Next
                     * suspect: a D217-style texture-cache collision -- the
                     * cache key is {texture_addr, fmt, siz, size_bytes,
                     * palette_hash} (gfx_pc.h) with no content hash for
                     * non-CI textures, so if two of the 15 FIRE_N images
                     * ever resolve to the same address (Globalimagetable
                     * fixup aliasing) or the cache evicts/reuses a texture_id
                     * without a real re-upload, this tile's GL texture could
                     * be serving stale/wrong-image content while every
                     * bookkeeping field above still looks correct. */
                    "TEXEL1_addr=%p TEXEL1_texid=%u TEXEL1_bytes=%u",
                    (unsigned long long)rdp.combine_mode, (int)rdp.tex_lod, fi,
                    c1, rdp.texture_tile[c1].tmem, rdp.texture_tile[c1].fmt, rdp.texture_tile[c1].siz,
                    s0, rdp.texture_tile[s0].tmem,
                    s1, rdp.texture_tile[s1].tmem, rdp.texture_tile[s1].fmt,
                    (s1 == s0) ? "  <-- TEXEL1 == TEXEL0 (BUG)" : "",
                    rsp.geometry_mode,
                    (rsp.geometry_mode & G_LIGHTING) ? "ON <-- D219 SUSPECT" : "off",
                    rendering_state.textures[1] ? (const void*)rendering_state.textures[1]->first.texture_addr : nullptr,
                    rendering_state.textures[1] ? rendering_state.textures[1]->second.texture_id : 0u,
                    rendering_state.textures[1] ? rendering_state.textures[1]->first.size_bytes : 0u);
            }
        }
    }
#endif

    struct ShaderProgram* prg = comb->prg[tm];
    if (prg == NULL) {
        comb->prg[tm] = prg =
            gfx_lookup_or_create_shader_program(comb->shader_id0, comb->shader_id1 | (tm * SHADER_OPT_TEXEL0_CLAMP_S));
    }
    if (prg != rendering_state.shader_program) {
        gfx_flush();
        gfx_rapi->unload_shader(rendering_state.shader_program);
        gfx_rapi->load_shader(prg);
        rendering_state.shader_program = prg;
    }
    if (use_alpha != rendering_state.alpha_blend || use_modulate != rendering_state.modulate) {
        gfx_flush();
        gfx_rapi->set_use_alpha(use_alpha, use_modulate);
        rendering_state.alpha_blend = use_alpha;
        rendering_state.modulate = use_modulate;
    }
    uint8_t num_inputs;
    bool used_textures[2];

    gfx_rapi->shader_get_info(prg, &num_inputs, used_textures);

    struct GfxClipParameters clip_parameters = gfx_rapi->get_clip_parameters();

        GEPreparedTriangleState &s=s_ge_prepared_tri;
        s.use_alpha=use_alpha;
        s.use_fog=use_fog;
        s.use_grayscale=use_grayscale;
        s.use_modulate=use_modulate;
        s.tm=tm;
        memcpy(s.tex_width,tex_width,sizeof(tex_width));
        memcpy(s.tex_height,tex_height,sizeof(tex_height));
        memcpy(s.tex_width2,tex_width2,sizeof(tex_width2));
        memcpy(s.tex_height2,tex_height2,sizeof(tex_height2));
        memcpy(s.wrap_s,wrap_s,sizeof(wrap_s));
        memcpy(s.wrap_t,wrap_t,sizeof(wrap_t));
        memcpy(s.wrap_tw,wrap_tw,sizeof(wrap_tw));
        memcpy(s.wrap_th,wrap_th,sizeof(wrap_th));
        memcpy(s.wrap_uls,wrap_uls,sizeof(wrap_uls));
        memcpy(s.wrap_ult,wrap_ult,sizeof(wrap_ult));
        s.comb=comb;
        s.num_inputs=num_inputs;
        memcpy(s.used_textures,used_textures,sizeof(used_textures));
        s.clip_parameters=clip_parameters;
        /* Separate bake serial: even if an out-of-band texture/viewport dirty
         * flag forces a rebuild without a DL generation change, no packed
         * vertex from the previous material may be reused. */
        if(++s.bake_serial==0) {
            memset(s_ge_vertex_bake,0,sizeof(s_ge_vertex_bake));
            s.bake_serial=1;
        }
        /* Rectangle helpers temporarily mutate RDP state within one opcode.
         * Never carry that temporary material into the following 3D draw. */
        s.generation=is_rect?0:s_ge_tri_state_generation;
        ++s_ge_tri_preparations;
    } else {
        ++s_ge_tri_reuses;
    }
    /* Native packet contains this triangle; probing prepares RDP state only.
     * Backend draw must succeed before any command in the run is skipped. */
    if(s_roomNativePrepareOnly)return;
    const GEPreparedTriangleState &s=s_ge_prepared_tri;
    const bool use_alpha=s.use_alpha, use_fog=s.use_fog;
    const bool use_grayscale=s.use_grayscale, use_modulate=s.use_modulate;
    const uint32_t tm=s.tm;
    const uint32_t *tex_width=s.tex_width,*tex_height=s.tex_height;
    const uint32_t *tex_width2=s.tex_width2,*tex_height2=s.tex_height2;
    const bool *wrap_s=s.wrap_s,*wrap_t=s.wrap_t;
    const float *wrap_tw=s.wrap_tw,*wrap_th=s.wrap_th;
    const float *wrap_uls=s.wrap_uls,*wrap_ult=s.wrap_ult;
    struct ColorCombiner *comb=s.comb;
    const uint8_t num_inputs=s.num_inputs;
    const bool *used_textures=s.used_textures;
    const struct GfxClipParameters &clip_parameters=s.clip_parameters;

    if (tri_dbg()) {   /* D474: one cached gate for the debug hooks */
        d75d_note_emit(videoGetFrameCount(), v_arr); // survived all rejection gates -> reaches GL
        d303_note_emit(videoGetFrameCount(), v_arr); // TEMP D303
        zf_note_emit(videoGetFrameCount(), v_arr);   // TEMP D306/D308
        d526_note_emit(videoGetFrameCount(), v_arr, use_alpha ? 1 : 0, use_modulate ? 1 : 0); // D526 census
        d75d_note_emit_z(videoGetFrameCount(), v_arr, (used_textures[0] || used_textures[1]) ? 1 : 0, comb->used_textures[0] ? 1 : 0);
    }
    /* D543: RSP-style near-plane clipping on the CPU. The GPU clips the
     * triangle itself against the near plane, but it interpolates the
     * noperspective shade colour and the fog value in screen space (D548
     * bands, D540 light/dark cycling, D543 bright lit area at Bond's feet),
     * so a big ground triangle that crosses the camera plane extrapolates
     * them. The RSP instead clips in clip space and lerps every vertex
     * attribute linearly there (t = dA / (dA - dB)); do the same, so GL
     * receives only fully-in-front triangles and never clips. Skipped for
     * rects, G_NO_CLIPPING_EXT, and GE_NEARCLIP=0 (A/B). Fog on a vertex
     * the clip creates is re-derived from its own z/w (as gfx_sp_vertex
     * does), like the RSP, whose clipper runs new vertices through the same
     * vertex-finish code: at the near plane that is a constant, so the haze
     * at the player's feet stays static (D553; maintainer compared against
     * LLE mupen64plus, cxd4 + angrylion). GE_NEARCLIPFOG=lerp restores the
     * old interpolation from the parents (whose behind-the-eye corner flips
     * between 0 and 255 fog as w crosses 0 -> the dark/bright popping) for
     * A/B. The new vertices live on the stack: rsp.loaded_vertices is
     * game-visible. */
    static int nearclip_on = -1, nearclip_fog_recompute = -1;
    if (nearclip_on < 0) {
        const char* e = getenv("GE_NEARCLIP");
        nearclip_on = !(e && e[0] == '0');
        const char* f = getenv("GE_NEARCLIPFOG");
        nearclip_fog_recompute = (f && strcmp(f, "lerp") == 0) ? 0 : 1;   /* D553: recompute by default */
    }
    struct LoadedVertex* emit_v[6] = { v1, v2, v3, NULL, NULL, NULL };
    int n_out_tris = 1;
    struct LoadedVertex nc_vtx[2];
    {
    GE_P0E_SCOPE(GE_P0E_CLIP);
    if (nearclip_on && !is_rect && (rsp.extra_geometry_mode & G_NO_CLIPPING_EXT) == 0) {
        const float NEAR_EPS = 1e-4f;
        float d[3];
        int n_in = 0;
        for (int i = 0; i < 3; i++) {
            d[i] = v_arr[i]->z + v_arr[i]->w - NEAR_EPS;
            if (d[i] > 0.0f) n_in++;
        }
        if (n_in == 0) {
            return; // entirely behind the near plane
        }
        if (n_in < 3) {
            struct LoadedVertex* poly[4];
            int n_poly = 0, n_new = 0;
            for (int i = 0; i < 3; i++) {
                int j = (i + 1) % 3;
                struct LoadedVertex* a = v_arr[i];
                struct LoadedVertex* b = v_arr[j];
                if (d[i] > 0.0f) poly[n_poly++] = a;
                if ((d[i] > 0.0f) != (d[j] > 0.0f)) {
                    float t = d[i] / (d[i] - d[j]);
                    struct LoadedVertex* o = &nc_vtx[n_new++];
                    o->x = a->x + (b->x - a->x) * t;
                    o->y = a->y + (b->y - a->y) * t;
                    o->z = a->z + (b->z - a->z) * t;
                    o->w = a->w + (b->w - a->w) * t;
                    o->u = a->u + (b->u - a->u) * t;
                    o->v = a->v + (b->v - a->v) * t;
                    o->color.r = (uint8_t)clampf(floorf(a->color.r + (b->color.r - a->color.r) * t + 0.5f), 0.f, 255.f);
                    o->color.g = (uint8_t)clampf(floorf(a->color.g + (b->color.g - a->color.g) * t + 0.5f), 0.f, 255.f);
                    o->color.b = (uint8_t)clampf(floorf(a->color.b + (b->color.b - a->color.b) * t + 0.5f), 0.f, 255.f);
                    o->color.a = (uint8_t)clampf(floorf(a->color.a + (b->color.a - a->color.a) * t + 0.5f), 0.f, 255.f);
                    o->clip_rej = 0;
                    o->fog_n = a->fog_n + (b->fog_n - a->fog_n) * t;
                    if (nearclip_fog_recompute) {
                        if (rsp.geometry_mode & G_FOG) {
                            float fw = o->w;
                            if (fabsf(fw) < 0.001f) fw = 0.001f;
                            float winv = 1.0f / fw;
                            if (!gfx_fog_vertex && winv < 0.0f) winv = 32767;
                            o->fog = clampf(o->z * winv * rsp.fog_mul + rsp.fog_offset, 0.f, 255.f);
                        } else {
                            o->fog = a->fog;
                        }
                    } else {
                        o->fog = (uint8_t)clampf(floorf(a->fog + ((float)b->fog - (float)a->fog) * t + 0.5f), 0.f, 255.f);
                    }
                    if (gfx_fog_vertex) o->fog_n = o->fog; // as gfx_sp_vertex
                    poly[n_poly++] = o;
                }
            }
            // n_poly is 3 or 4 here (one or two vertices inside)
            emit_v[0] = poly[0]; emit_v[1] = poly[1]; emit_v[2] = poly[2];
            if (n_poly == 4) {
                emit_v[3] = poly[0]; emit_v[4] = poly[2]; emit_v[5] = poly[3];
                n_out_tris = 2;
            }
        }
    }
    } /* PH0E clip scope: no changes to emitted geometry. */
    const int n_emit_verts = n_out_tris * 3;
    if (buf_vbo_num_tris + n_out_tris > MAX_BUFFERED) {
        gfx_flush(); // buf_vbo holds MAX_BUFFERED triangles; make room for 1-2
    }
    {
    GE_P0E_SCOPE(GE_P0E_PACK);
    for (int i = 0; i < n_emit_verts; i++) {
        struct LoadedVertex* vi = emit_v[i];
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_FAST3D_VERTEX_CACHE
        /* Pointer arithmetic is performed on integers until we have proved
         * that the vertex belongs to the stable RSP loaded-vertex array. */
        const uintptr_t vertexAddress=(uintptr_t)vi;
        const uintptr_t loadedStart=(uintptr_t)&rsp.loaded_vertices[0];
        const uintptr_t loadedEnd=(uintptr_t)&rsp.loaded_vertices[MAX_VERTICES];
        const bool stableVertex = !is_rect && !tri_dbg() &&
            vertexAddress>=loadedStart && vertexAddress<loadedEnd &&
            ((vertexAddress-loadedStart)%sizeof(LoadedVertex))==0;
        unsigned vertexSlot=0;
        if(stableVertex) {
            vertexSlot=(unsigned)((vertexAddress-loadedStart)/sizeof(LoadedVertex));
            const GEPreparedVertexBake &b=s_ge_vertex_bake[vertexSlot];
            if(b.generation==s_ge_prepared_tri.bake_serial && b.count<=32u && b.count>0u) {
                memcpy(&buf_vbo[buf_vbo_len],b.packed,b.count*sizeof(float));
                buf_vbo_len+=b.count;
                ++s_ge_vertex_reuses;
                continue;
            }
        }
        const size_t vertexBakeStart=buf_vbo_len;
#endif
        float z = vi->z, w = vi->w;
        if (clip_parameters.z_is_from_0_to_1) {
            z = (z + w) / 2.0f;
        }

        buf_vbo[buf_vbo_len++] = vi->x;
        buf_vbo[buf_vbo_len++] = clip_parameters.invert_y ? -vi->y : vi->y;
        buf_vbo[buf_vbo_len++] = z;
        buf_vbo[buf_vbo_len++] = w;

        for (int t = 0; t < 2; t++) {
            if (!used_textures[t]) {
                continue;
            }

            // TODO: fix this; for now just ignore smaller mips
            const uint32_t tile = gfx_lod_tile_offset(t);

            float u = vi->u / 32.0f;
            float v = vi->v / 32.0f;

            int shifts = rdp.texture_tile[rdp.first_tile_index + tile].shifts;
            int shiftt = rdp.texture_tile[rdp.first_tile_index + tile].shiftt;
            if (shifts != 0) {
                if (shifts <= 10) {
                    u /= 1 << shifts;
                } else {
                    u *= 1 << (16 - shifts);
                }
            }
            if (shiftt != 0) {
                if (shiftt <= 10) {
                    v /= 1 << shiftt;
                } else {
                    v *= 1 << (16 - shiftt);
                }
            }

            u -= rdp.texture_tile[rdp.first_tile_index + tile].uls / 4.0f;
            v -= rdp.texture_tile[rdp.first_tile_index + tile].ult / 4.0f;

            // D74 (Video.WrapFix, opt-in): pre-wrap UVs at the tile-window
            // period when the render tile is a sub-region of the uploaded
            // image. Flags/sizes are precomputed per texunit above (indexed by
            // `t`, not the vertex `i`). The old in-place version was inert
            // (guard `cms & G_TX_WRAP` == `& 0`) and OOB (`tex_width2[i]` with
            // `i` = vertex). See SMALL-FIXES B1.
            if (wrap_s[t]) {
                u = fmodf(u, wrap_tw[t]);
                if (u < 0.0f) u += wrap_tw[t];
                u += wrap_uls[t];
                /* RC3: when the N64 wrap period (wrap_tw) exceeds the uploaded
                 * image width, the [width, period) band has no real texels --
                 * clamp it to the edge rather than let GL_REPEAT restart the
                 * image early. Never triggers for the sub-tile-window case
                 * (wrap_tw + wrap_uls stay within tex_width). */
                if (tex_width[t] > 0 && u > (float)tex_width[t]) u = (float)tex_width[t] - 0.5f;
            }
            if (wrap_t[t]) {
                v = fmodf(v, wrap_th[t]);
                if (v < 0.0f) v += wrap_th[t];
                v += wrap_ult[t];
                if (tex_height[t] > 0 && v > (float)tex_height[t]) v = (float)tex_height[t] - 0.5f;
            }

            if (!is_rect) {
                if (!(rdp.other_mode_h & G_TP_PERSP)) {
                    u *= 0.5f;
                    v *= 0.5f;
                }

                if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) {
                    // Linear filter adds 0.5f to the coordinates
                    u += 0.5f;
                    v += 0.5f;
                }
            }

            buf_vbo[buf_vbo_len++] = u / tex_width[t];
            buf_vbo[buf_vbo_len++] = v / tex_height[t];

            bool clampS = tm & (1 << 2 * t);
            bool clampT = tm & (1 << (2 * t + 1));

            if (clampS) {
                buf_vbo[buf_vbo_len++] = (tex_width2[t] - 0.5f) / tex_width[t];
            }
            if (clampT) {
                buf_vbo[buf_vbo_len++] = (tex_height2[t] - 0.5f) / tex_height[t];
            }
        }

        if (use_fog) {
            buf_vbo[buf_vbo_len++] = rdp.fog_color.r / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.fog_color.g / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.fog_color.b / 255.0f;
            buf_vbo[buf_vbo_len++] = vi->fog_n / 255.0f; // D540: see LoadedVertex.fog_n
        }

        if (use_grayscale) {
            buf_vbo[buf_vbo_len++] = rdp.grayscale_color.r / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.grayscale_color.g / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.grayscale_color.b / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.grayscale_color.a / 255.0f; // lerp interpolation factor (not alpha)
        }

        for (int j = 0; j < num_inputs; j++) {
            struct RGBA* color = 0;
            struct RGBA tmp = { 0 };
            for (int k = 0; k < 1 + (use_alpha ? 1 : 0); k++) {
                switch (comb->shader_input_mapping[k][j]) {
                        // Note: CCMUX constants and ACMUX constants used here have same value, which is why this works
                        // (except LOD fraction).
                    case G_CCMUX_PRIMITIVE:
                        color = &rdp.prim_color;
                        break;
                    case G_CCMUX_SHADE:
                        color = &vi->color;
                        break;
                    case G_CCMUX_SHADE_ALPHA:
                        tmp.r = tmp.g = tmp.b = vi->color.a;
                        color = &tmp;
                        break;
                    case G_CCMUX_ENVIRONMENT:
                        color = &rdp.env_color;
                        break;
                    case G_CCMUX_PRIMITIVE_ALPHA: {
                        tmp.r = tmp.g = tmp.b = rdp.prim_color.a;
                        color = &tmp;
                        break;
                    }
                    case G_CCMUX_ENV_ALPHA: {
                        tmp.r = tmp.g = tmp.b = rdp.env_color.a;
                        color = &tmp;
                        break;
                    }
                    case G_CCMUX_PRIM_LOD_FRAC: {
                        tmp.r = tmp.g = tmp.b = rdp.prim_lod_fraction;
                        color = &tmp;
                        break;
                    }
                    case G_CCMUX_LOD_FRACTION: {
                        if (rdp.other_mode_h & G_TL_LOD) {
                            // HACK: very roughly eyeballed based on the carpets in Defection
                            // this is actually supposed to be calculated per pixel
                            const float distance_frac = clampf(w/1024.f,0.f,1.f);
                            tmp.r = tmp.g = tmp.b = tmp.a = (0.7f + distance_frac * 0.3f) * 255.f;
                        } else {
                            tmp.r = tmp.g = tmp.b = tmp.a = 255;
                        }
                        color = &tmp;
                        break;
                    }
                    case G_ACMUX_PRIM_LOD_FRAC:
                        tmp.a = rdp.prim_lod_fraction;
                        color = &tmp;
                        break;
                    default:
                        memset(&tmp, 0, sizeof(tmp));
                        color = &tmp;
                        break;
                }
                if (k == 0) {
                    buf_vbo[buf_vbo_len++] = color->r / 255.0f;
                    buf_vbo[buf_vbo_len++] = color->g / 255.0f;
                    buf_vbo[buf_vbo_len++] = color->b / 255.0f;
                } else {
                    buf_vbo[buf_vbo_len++] = color->a / 255.0f;
                }
            }
        }
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_FAST3D_VERTEX_CACHE
        if(stableVertex) {
            const size_t n=buf_vbo_len-vertexBakeStart;
            if(n>0 && n<=32u) {
                GEPreparedVertexBake &b=s_ge_vertex_bake[vertexSlot];
                memcpy(b.packed,&buf_vbo[vertexBakeStart],n*sizeof(float));
                b.count=(unsigned)n;
                b.generation=s_ge_prepared_tri.bake_serial;
                ++s_ge_vertex_bakes;
            }
        }
#endif
    }

    } /* PH0E packed vertex output. */
    buf_vbo_num_tris += n_out_tris;
    if (buf_vbo_num_tris >= MAX_BUFFERED) {
        gfx_flush();
    }
}

static inline void gfx_sp_tri4(Gfx *cmd) {
    // the game issues gSPTri2 for quads, which uses G_TRI4 with 2 empty triangles
    uint8_t x = C1(0, 4);
    uint8_t y = C1(4, 4);
    uint8_t z = C0(0, 4);

    if(x || y || z) {
        gfx_sp_tri1(x, y, z, false);
    }

    x = C1(8, 4);
    y = C1(12, 4);
    z = C0(4, 4);

    if (x || y || z) {
        gfx_sp_tri1(x, y, z, false);
    }

    x = C1(16, 4);
    y = C1(20, 4);
    z = C0(8, 4);

    if (x || y || z) {
        gfx_sp_tri1(x, y, z, false);
    }

    x = C1(24, 4);
    y = C1(28, 4);
    z = C0(12, 4);

    if (x || y || z) {
        gfx_sp_tri1(x, y, z, false);
    }
}

static void gfx_sp_geometry_mode(uint32_t clear, uint32_t set) {
    rsp.geometry_mode &= ~clear;
    rsp.geometry_mode |= set;
}


#if defined(GE_XBOX_NATIVE32) && GE_XBOX_NATIVE_ROOM_BATCHES
/* Batch-level eligibility is checked ONCE for a sequence of triangles, not
 * for each triangle like Phase 5E. The packet is stored by the graphics
 * thread, and every rendering command between packets runs unchanged. */
/* Geometry-run admission threshold: older 6B required >=6 opcodes AND
 * >=16 triangles, excluding many normal short TRI4 runs. Eight triangles
 * across at least two commands still amortize a resident draw, while single
 * triangles/short runs continue through software. Disable the 8-triangle
 * policy to restore the original 6B admission filter. */
#ifndef GE_XBOX_NATIVE_ROOM_SHORT_RUNS
#define GE_XBOX_NATIVE_ROOM_SHORT_RUNS 0
#endif
#ifndef GE_XBOX_ROOM_COLD_PREFLIGHT
#define GE_XBOX_ROOM_COLD_PREFLIGHT 1
#endif

static GfxNativeRoomVertex s_roomBuildVerts[1024u];
/* Frontend cache stores ONLY metadata for a resident packet; warm frames do
 * not walk or repack the triangle run. GPU resources live in the backend. */
struct NativeRoomMeta {
    const Gfx* source;
    unsigned room,revision,signature,commands,vertices;
    unsigned cullMode,textureUnits;
    float min_bound[3],max_bound[3];
    bool valid;
};
enum { GE_ROOM_FRONT_META_SLOTS = GE_XBOX_ROOM_PACKET_RECORDS };
static NativeRoomMeta s_roomFrontMeta[GE_ROOM_FRONT_META_SLOTS];
static unsigned s_roomFrontMetaClock=0;
static unsigned s_roomFrontBuildsFrame=0;
/* Lightweight cumulative diagnostics at RUN boundaries, not per triangle.
 * Indexes: begin, runs, short, material, vertex, near, uploaded, backend.
 * Read by the Back-button overlay once per 120-frame window; never logged. */
enum { ROOM_STAT_BEGIN, ROOM_STAT_RUN, ROOM_STAT_SHORT,
       ROOM_STAT_MATERIAL, ROOM_STAT_VERTEX, ROOM_STAT_NEAR,
       ROOM_STAT_UPLOADED, ROOM_STAT_BACKEND, ROOM_STAT_COUNT };
static unsigned s_roomHWStats[ROOM_STAT_COUNT]={0};
/* Ineligible static material packets stay on the original Fast3D path.
 * Direct-mapped failure entries avoid probing unchanged room DLs repeatedly. */
struct NativeRoomRejected {
    const Gfx* source;
    unsigned room,revision,commands,material;
};
static NativeRoomRejected s_roomRejected[128];
static unsigned geRoomRejectStamp() {
    uint32_t h=geRoomMix32(2166136261u,(uint32_t)rdp.combine_mode);
    h=geRoomMix32(h,(uint32_t)(rdp.combine_mode>>32));
    h=geRoomMix32(h,(uint32_t)rdp.other_mode_h);
    h=geRoomMix32(h,(uint32_t)rdp.other_mode_l);
    h=geRoomMix32(h,(uint32_t)rsp.geometry_mode);
    return geRoomMix32(h,rdp.first_tile_index);
}
static NativeRoomRejected* geRoomRejectSlot(const Gfx* begin) {
    return &s_roomRejected[((uintptr_t)begin>>4u)&127u];
}
static unsigned geRoomIndices(const Gfx *cmd,unsigned index[12]) {
    const uint32_t opcode=cmd->words.w0>>24;
    if(opcode==(uint8_t)G_TRI1) {
        index[0]=(cmd->words.w1>>16&255u)/10u;
        index[1]=(cmd->words.w1>>8&255u)/10u;
        index[2]=(cmd->words.w1&255u)/10u;
        return 3u;
    }
    if(opcode==(uint8_t)G_TRI4) {
        unsigned total=0u;
        for(unsigned k=0;k<4u;++k) {
            unsigned x=(cmd->words.w1>>(8u*k))&15u;
            unsigned y=(cmd->words.w1>>(8u*k+4u))&15u;
            unsigned z=(cmd->words.w0>>(4u*k))&15u;
            if(x||y||z) {index[total++]=x;index[total++]=y;index[total++]=z;}
        }
        return total;
    }
    return 0u;
}
static bool geRoomNativeMaterialOK() {
    if(s_roomScope<0 || s_roomHasColorOverride || fbActive || tri_dbg() ||
       !gfx_rapi->native_room_draw || !gfx_rapi->native_room_prepare ||
       (rsp.geometry_mode & (G_LIGHTING|G_TEXTURE_GEN|G_TEXTURE_GEN_LINEAR)) ||
       (rsp.extra_geometry_mode & G_NO_CLIPPING_EXT))return false;
    const GEPreparedTriangleState &m=s_ge_prepared_tri;
    if(m.generation!=s_ge_tri_state_generation || !m.comb ||
       m.use_alpha || m.use_fog || m.use_grayscale || m.use_modulate ||
       m.tm || m.num_inputs>1 || m.used_textures[1] ||
       m.wrap_s[0] || m.wrap_t[0] ||
       m.clip_parameters.invert_y || m.clip_parameters.z_is_from_0_to_1)return false;
    if(m.num_inputs==1 && m.comb->shader_input_mapping[0][0]!=G_CCMUX_SHADE)return false;
    if(m.used_textures[0] && (!m.tex_width[0] || !m.tex_height[0]))return false;
    return true;
}
static unsigned geRoomPacketSignature(unsigned commands) {
    const GEPreparedTriangleState &m=s_ge_prepared_tri;
    /* One packet remains reusable under camera motion, but never when UV
     * derivation or the effective RDP material changes. */
    uint32_t signature=geRoomMix32(2166136261u,commands);
    signature=geRoomMix32(signature,m.tex_width[0]);
    signature=geRoomMix32(signature,m.tex_height[0]);
    signature=geRoomMix32(signature,(uint32_t)rdp.other_mode_h);
    signature=geRoomMix32(signature,(uint32_t)rsp.texture_scaling_factor.s);
    signature=geRoomMix32(signature,(uint32_t)rsp.texture_scaling_factor.t);
    signature=geRoomMix32(signature,(uint32_t)rdp.combine_mode);
    signature=geRoomMix32(signature,(uint32_t)(rdp.combine_mode>>32));
    if(m.used_textures[0]) {
        const unsigned tile=rdp.first_tile_index+gfx_lod_tile_offset(0);
        signature=geRoomMix32(signature,rdp.texture_tile[tile].uls);
        signature=geRoomMix32(signature,rdp.texture_tile[tile].ult);
        signature=geRoomMix32(signature,(unsigned)rdp.texture_tile[tile].shifts);
        signature=geRoomMix32(signature,(unsigned)rdp.texture_tile[tile].shiftt);
    }
    return signature;
}
static bool geRoomBuildPacket(const Gfx *begin,unsigned commands,
                             GfxNativeRoomDraw *packet) {
    if(!packet || !commands || commands>256u || !geRoomNativeMaterialOK())return false;
    const unsigned stamp=geRoomMatrixStamp();
    unsigned total=0;
    float minX=0,minY=0,minZ=0,maxX=0,maxY=0,maxZ=0;
    const GEPreparedTriangleState &m=s_ge_prepared_tri;
    for(unsigned c=0;c<commands;++c) {
        unsigned indices[12];const unsigned count=geRoomIndices(&begin[c],indices);
        if(!count || total+count>1024u)return false;
        for(unsigned n=0;n<count;++n) {
            unsigned idx=indices[n];
            if(idx>=MAX_VERTICES)return false;
            const NativeRoomRawSlot &raw=s_roomRawSlot[idx];
            if(!raw.valid || raw.room!=(unsigned)s_roomScope ||
               raw.revision!=s_roomRevision || raw.matrixStamp!=stamp)return false;
            GfxNativeRoomVertex &v=s_roomBuildVerts[total++];
            v.x=raw.x;v.y=raw.y;v.z=raw.z;
            v.color=raw.materialColor;
            v.u0=v.v0=0.0f;
            if(m.used_textures[0]) {
                const LoadedVertex &lv=rsp.loaded_vertices[idx];
                float u=lv.u/32.0f,vv=lv.v/32.0f;
                const unsigned tile=rdp.first_tile_index+gfx_lod_tile_offset(0);
                const int sh=rdp.texture_tile[tile].shifts,sv=rdp.texture_tile[tile].shiftt;
                if(sh) {if(sh<=10)u/=(float)(1<<sh);else u*=(float)(1<<(16-sh));}
                if(sv) {if(sv<=10)vv/=(float)(1<<sv);else vv*=(float)(1<<(16-sv));}
                u-=rdp.texture_tile[tile].uls/4.0f;
                vv-=rdp.texture_tile[tile].ult/4.0f;
                if(!(rdp.other_mode_h&G_TP_PERSP)){u*=0.5f;vv*=0.5f;}
                if((rdp.other_mode_h&(3u<<G_MDSFT_TEXTFILT))!=G_TF_POINT){u+=0.5f;vv+=0.5f;}
                v.u0=u/(float)m.tex_width[0];v.v0=vv/(float)m.tex_height[0];
            }
            if(total==1){minX=maxX=v.x;minY=maxY=v.y;minZ=maxZ=v.z;}
            else {
                if(v.x<minX)minX=v.x;if(v.x>maxX)maxX=v.x;
                if(v.y<minY)minY=v.y;if(v.y>maxY)maxY=v.y;
                if(v.z<minZ)minZ=v.z;if(v.z>maxZ)maxZ=v.z;
            }
        }
    }
#if GE_XBOX_NATIVE_ROOM_SHORT_RUNS
    if(total<24u)return false; /* >=8 triangles, room-only persistent packets */
#else
    if(total<48u)return false; /* original 6B minimum 16 triangles */
#endif
    packet->room=(unsigned)s_roomScope;
    packet->revision=s_roomRevision;
    packet->signature=geRoomPacketSignature(commands);
    packet->command_address=begin;
    packet->vertices=s_roomBuildVerts;
    packet->vertex_count=total;
    memcpy(packet->matrix,rsp.MP_matrix,sizeof(packet->matrix));
    packet->aspect_offset=rsp.aspect_ofs;
    packet->aspect_scale=rsp.aspect_scale/gfx_current_dimensions.aspect_ratio;
    switch(rsp.geometry_mode&G_CULL_BOTH) {
        case G_CULL_FRONT:packet->cull_mode=1u;break;
        case G_CULL_BACK:packet->cull_mode=2u;break;
        case G_CULL_BOTH:return false;
        default:packet->cull_mode=0u;break;
    }
    packet->texture_units=m.used_textures[0]?1u:0u;
    for(unsigned i=0;i<3;++i){packet->min_bound[i]=i==0?minX:i==1?minY:minZ;
        packet->max_bound[i]=i==0?maxX:i==1?maxY:maxZ;}
    return true;
}
static NativeRoomMeta* geRoomFindMeta(const Gfx* begin,unsigned commands,
                                      unsigned signature) {
    for(unsigned i=0;i<GE_ROOM_FRONT_META_SLOTS;++i) {
        NativeRoomMeta &e=s_roomFrontMeta[i];
        if(e.valid && e.source==begin && e.commands==commands &&
           e.room==(unsigned)s_roomScope && e.revision==s_roomRevision &&
           e.signature==signature)return &e;
    }
    return 0;
}
#if GE_XBOX_ROOM_COLD_PREFLIGHT
static NativeRoomMeta* geRoomFindMetaCandidate(const Gfx* begin,unsigned commands) {
    /* Material signature is not prepared yet. Any possibly warm packet must
     * proceed through the original material validation and exact lookup. */
    for(unsigned i=0;i<GE_ROOM_FRONT_META_SLOTS;++i) {
        NativeRoomMeta &e=s_roomFrontMeta[i];
        if(e.valid && e.source==begin && e.commands==commands &&
           e.room==(unsigned)s_roomScope && e.revision==s_roomRevision)
            return &e;
    }
    return 0;
}
#endif
static void geRoomCopyMeta(GfxNativeRoomDraw &packet,const NativeRoomMeta &e) {
    packet.room=e.room;packet.revision=e.revision;packet.signature=e.signature;
    packet.command_address=e.source;packet.vertices=0;
    packet.vertex_count=e.vertices;packet.cull_mode=e.cullMode;
    packet.texture_units=e.textureUnits;
    memcpy(packet.min_bound,e.min_bound,sizeof(e.min_bound));
    memcpy(packet.max_bound,e.max_bound,sizeof(e.max_bound));
    memcpy(packet.matrix,rsp.MP_matrix,sizeof(packet.matrix));
    packet.aspect_offset=rsp.aspect_ofs;
    packet.aspect_scale=rsp.aspect_scale/gfx_current_dimensions.aspect_ratio;
}
static void geRoomRememberMeta(const GfxNativeRoomDraw &packet,unsigned commands) {
    NativeRoomMeta *e=0;
    for(unsigned i=0;i<GE_ROOM_FRONT_META_SLOTS;++i)if(!s_roomFrontMeta[i].valid) {
        e=&s_roomFrontMeta[i];break;
    }
    if(!e) {e=&s_roomFrontMeta[(s_roomFrontMetaClock++)%GE_ROOM_FRONT_META_SLOTS];}
    e->source=(const Gfx*)packet.command_address;e->commands=commands;
    e->room=packet.room;e->revision=packet.revision;
    e->signature=packet.signature;
    e->vertices=packet.vertex_count;e->cullMode=packet.cull_mode;
    e->textureUnits=packet.texture_units;
    memcpy(e->min_bound,packet.min_bound,sizeof(e->min_bound));
    memcpy(e->max_bound,packet.max_bound,sizeof(e->max_bound));
    e->valid=true;
}
static bool geRoomFrontProjectionSafe(const GfxNativeRoomDraw &packet) {
#if GE_XBOX_ROOM_GPU_CLIP
    /* Frontend still validates material and exact vertex provenance. The
     * fixed-function Xbox clipper receives original homogeneous W through
     * the world/view/projection matrix. Avoid room-AABB false negatives: the
     * player is commonly *inside* this box. */
    (void)packet;
    return true;
#else
    for(unsigned k=0;k<8u;++k) {
        const float x=(k&1u)?packet.max_bound[0]:packet.min_bound[0];
        const float y=(k&2u)?packet.max_bound[1]:packet.min_bound[1];
        const float z=(k&4u)?packet.max_bound[2]:packet.min_bound[2];
        const float w=x*packet.matrix[3]+y*packet.matrix[7]+z*packet.matrix[11]+packet.matrix[15];
        const float pz=x*packet.matrix[2]+y*packet.matrix[6]+z*packet.matrix[10]+packet.matrix[14];
        if(!(w>0.005f && pz > -w+0.005f && pz < w-0.005f))return false;
    }
    return true;
#endif
}
static bool geRoomNativeRun(Gfx *begin,unsigned count) {
    ++s_roomHWStats[ROOM_STAT_RUN];
    if(s_roomScope<0 || !s_roomRevision ||
       (rsp.geometry_mode&(G_LIGHTING|G_TEXTURE_GEN)) ||
       s_roomHasColorOverride || !gfx_rapi->native_room_draw ||
       !gfx_rapi->native_room_prepare) {
        ++s_roomHWStats[ROOM_STAT_MATERIAL];return false;
    }
#if GE_XBOX_NATIVE_ROOM_SHORT_RUNS
    if(count<2u){++s_roomHWStats[ROOM_STAT_SHORT];return false;}
#else
    if(count<6u){++s_roomHWStats[ROOM_STAT_SHORT];return false;}
#endif
    unsigned triCount=0;
    for(unsigned c=0;c<count;++c) {
        unsigned idx[12];
        triCount+=geRoomIndices(begin+c,idx)/3u;
    }
#if GE_XBOX_NATIVE_ROOM_SHORT_RUNS
    if(triCount<8u){++s_roomHWStats[ROOM_STAT_SHORT];return false;}
#else
    if(triCount<16u){++s_roomHWStats[ROOM_STAT_SHORT];return false;}
#endif
    const unsigned rejectStamp=geRoomRejectStamp();
    NativeRoomRejected *reject=geRoomRejectSlot(begin);
    if(reject->source==begin && reject->room==(unsigned)s_roomScope &&
       reject->revision==s_roomRevision && reject->commands==count &&
       reject->material==rejectStamp){++s_roomHWStats[ROOM_STAT_MATERIAL];return false;}
#if GE_XBOX_ROOM_COLD_PREFLIGHT
    /* Phase 6E: first ask the backend whether ANY cold packet can be built
     * on this graphics task. In 6D, only one task out of eight may upload,
     * and the fixed 1 MiB/128-record cache may become permanently full.
     * No need to material-probe or rebuild a failed packet in those cases.
     * Potentially warm packets still run the full exact-identity validation. */
    GfxNativeRoomDraw earlyCapacity;
    memset(&earlyCapacity,0,sizeof(earlyCapacity));
    earlyCapacity.vertex_count=triCount*3u;
    NativeRoomMeta *potentialWarm=0;
    if(!gfx_rapi->native_room_prepare(&earlyCapacity)) {
        potentialWarm=geRoomFindMetaCandidate(begin,count);
        if(!potentialWarm) {
            ++s_roomHWStats[ROOM_STAT_BACKEND];return false;
        }
    }
#endif
    /* Probe a single original triangle for material setup only. Every RDP
     * command surrounding the run is still executed by the interpreter. */
    unsigned first[12];const unsigned n=geRoomIndices(begin,first);
    if(n<3u || first[0]>=MAX_VERTICES || first[1]>=MAX_VERTICES ||
       first[2]>=MAX_VERTICES){++s_roomHWStats[ROOM_STAT_VERTEX];return false;}
    s_roomNativePrepareOnly=true;
    gfx_sp_tri1((uint8_t)first[0],(uint8_t)first[1],(uint8_t)first[2],false);
    s_roomNativePrepareOnly=false;
    if(!geRoomNativeMaterialOK()) {
        ++s_roomHWStats[ROOM_STAT_MATERIAL];
        reject->source=begin;reject->room=(unsigned)s_roomScope;
        reject->revision=s_roomRevision;reject->commands=count;
        reject->material=rejectStamp;
        return false;
    }
    const unsigned signature=geRoomPacketSignature(count);
    const unsigned stamp=geRoomMatrixStamp();
    NativeRoomMeta *warm=0;
#if GE_XBOX_ROOM_COLD_PREFLIGHT
    if(potentialWarm && potentialWarm->signature==signature)warm=potentialWarm;
    else
#endif
        warm=geRoomFindMeta(begin,count,signature);
    if(warm) {
        const NativeRoomRawSlot &firstRaw=s_roomRawSlot[first[0]];
        if(firstRaw.valid && firstRaw.room==(unsigned)s_roomScope &&
           firstRaw.revision==s_roomRevision && firstRaw.matrixStamp==stamp) {
            GfxNativeRoomDraw packet;
            geRoomCopyMeta(packet,*warm);
            /* No flush or backend state work for camera-near packets. The
             * original software clipping path remains authoritative. */
            if(!geRoomFrontProjectionSafe(packet)) {
                ++s_roomHWStats[ROOM_STAT_NEAR];return false;
            }
            /* Query resident VBs before making a draw barrier. */
            if(gfx_rapi->native_room_prepare(&packet)) {
                gfx_flush();
                if(gfx_rapi->native_room_draw(&packet))return true;
                ++s_roomHWStats[ROOM_STAT_BACKEND];
                return false;
            }
        }
        /* Only an actual backend eviction or stale vertex-slot stamp
         * invalidates the frontend metadata. */
        warm->valid=false;
    }
    if(s_roomFrontBuildsFrame>=GE_XBOX_ROOM_UPLOADS_PER_FRAME){++s_roomHWStats[ROOM_STAT_BACKEND];return false;}
    unsigned total=0u;
    for(unsigned c=0;c<count;++c) {
        unsigned indices[12];const unsigned nVerts=geRoomIndices(begin+c,indices);
        if(!nVerts){++s_roomHWStats[ROOM_STAT_VERTEX];return false;}
        total+=nVerts;
        if(total>1024u){++s_roomHWStats[ROOM_STAT_VERTEX];return false;}
        for(unsigned i=0;i<nVerts;++i) {
            if(indices[i]>=MAX_VERTICES){++s_roomHWStats[ROOM_STAT_VERTEX];return false;}
            const NativeRoomRawSlot &raw=s_roomRawSlot[indices[i]];
            if(!raw.valid || raw.room!=(unsigned)s_roomScope ||
               raw.revision!=s_roomRevision || raw.matrixStamp!=stamp) {
                ++s_roomHWStats[ROOM_STAT_VERTEX];return false;
            }
        }
    }
#if GE_XBOX_NATIVE_ROOM_SHORT_RUNS
    if(total<24u){++s_roomHWStats[ROOM_STAT_SHORT];return false;}
#else
    if(total<48u){++s_roomHWStats[ROOM_STAT_SHORT];return false;}
#endif
    GfxNativeRoomDraw packet;
    if(!geRoomBuildPacket(begin,count,&packet)) {
        ++s_roomHWStats[ROOM_STAT_VERTEX];return false;
    }
    if(!geRoomFrontProjectionSafe(packet)) {
        ++s_roomHWStats[ROOM_STAT_NEAR];return false;
    }
    ++s_roomFrontBuildsFrame;
    if(gfx_rapi->native_room_prepare(&packet)) {
        ++s_roomHWStats[ROOM_STAT_UPLOADED];
        geRoomRememberMeta(packet,count);
    } else ++s_roomHWStats[ROOM_STAT_BACKEND];
    /* First frame always executes the original TRI run. */
    return false;
}

#endif

#if defined(GE_XBOX_NATIVE32)
/* A fixed-layout C boundary. Always defined even in a rollback build. */
extern "C" void geNativeRoomPipelineCounters(unsigned out[8]) {
    if(!out)return;
#if GE_XBOX_NATIVE_ROOM_BATCHES
    for(unsigned i=0;i<ROOM_STAT_COUNT;++i)out[i]=s_roomHWStats[i];
#else
    for(unsigned i=0;i<8u;++i)out[i]=0u;
#endif
}
#endif

static inline void gfx_update_aspect_mode(void) {
    const uint32_t side = rsp.aspect_mode & G_ASPECT_CENTER_EXT;

    rsp.aspect_scale = rsp.aspect_mode ? gfx_current_native_aspect : gfx_current_dimensions.aspect_ratio; /* D447: == window aspect unless the output rect is active */

    if (side == G_ASPECT_LEFT_EXT) {
        rsp.aspect_ofs = 1.f - gfx_current_dimensions.aspect_ratio / gfx_current_native_aspect;
    } else if (side == G_ASPECT_RIGHT_EXT) {
        rsp.aspect_ofs = gfx_current_dimensions.aspect_ratio / gfx_current_native_aspect - 1.f;
    } else {
        rsp.aspect_ofs = 0.f;
    }

    if (side && (rsp.aspect_mode & G_ASPECT_WIDE_EXT)) {
        constexpr float c = 16.f / 9.f;
        if (gfx_current_dimensions.aspect_ratio > c) {
            rsp.aspect_ofs *= c / gfx_current_dimensions.aspect_ratio;
        }
    }
}

static void gfx_sp_extra_geometry_mode(uint32_t clear, uint32_t set) {
    rsp.extra_geometry_mode &= ~clear;
    rsp.extra_geometry_mode |= set;
    rsp.aspect_mode = (rsp.extra_geometry_mode & G_ASPECT_MODE_EXT);
    gfx_update_aspect_mode();
}

static void gfx_adjust_viewport_or_scissor(XYWidthHeight* area, bool preserve_aspect = false, bool window_space = false) {
    window_space = window_space || g_overlay_window_space;   /* D510 */
    // HACK: assume all target framebuffers have the same aspect
    // Use floor/ceil to ensure scissor fully contains the logical region
    // and prevents sub-pixel gaps at viewport edges
    // g_gpSafeTop already plays the exact role SCREEN_HEIGHT plays below
    // (both are the bottom-up Y value of the mapped region's TOP edge) --
    // this reduces to the untouched original formula when crop is off.
    const bool crop = g_safe_area_crop_enabled && !g_split_screen && !window_space && g_gpSafeHeight > 0.0f;
    const float safeTop = crop ? g_gpSafeTop : (float)GE_LOGICAL_HEIGHT;
    const float safeHeight = crop ? g_gpSafeHeight : (float)GE_LOGICAL_HEIGHT;
    const float ratioY = gfx_current_dimensions.height / safeHeight;

    // D246 (findings.md): a consistent, exactly-1-logical-unit gap at the
    // left AND right screen edges (measured empirically: 2px/3px/4px at
    // RATIO_X 2/3/4 -- always exactly 1 unit of the 320-wide logical space,
    // on both edges, symmetric, same underlying cause on every level tested)
    // reveals whatever's drawn behind the foreground scene there (varies by
    // level, e.g. sky/ambient colour) instead of scene content. Root cause
    // not isolated to a specific game-code or fast3d call site despite a
    // deep pass (viewport/scissor math here is provably exact at integer
    // RATIO_X, ruling out a floor/ceil rounding bug) -- treat this as the
    // same class of issue as the vertical safe-area crop above (D247/the
    // TV-overscan margin) and fold it into the same toggle: trim the same
    // fixed 1-unit margin from both edges rather than trying to force
    // content to reach a boundary it may never actually be drawn to.
    const float safeLeft = (g_safe_area_crop_enabled && !g_split_screen && !window_space) ? 1.0f : 0.0f;
    const float safeWidth = (g_safe_area_crop_enabled && !g_split_screen && !window_space) ? (float)GE_LOGICAL_WIDTH - 2.0f : (float)GE_LOGICAL_WIDTH;
    const float ratioX = gfx_current_dimensions.width / safeWidth;

    float x1 = (area->x - safeLeft) * ratioX;
    float y1 = (safeTop - area->y) * ratioY;
    float x2 = (area->x + area->width - safeLeft) * ratioX;
    float y2 = (safeTop - area->y + area->height) * ratioY;
    
    if (preserve_aspect) {
        /* Match gfx_adjust_x_for_aspect_ratio: the anchor offset is part of
         * the coordinate being scaled. Applying an unscaled offset moved
         * edge-anchored HUD scissors away from their actual geometry. */
        const float ratio = gfx_current_native_aspect / gfx_current_dimensions.aspect_ratio;
        const float midx = gfx_current_dimensions.width * 0.5f;
        const float offset = rsp.aspect_ofs * midx;
        x1 = midx + (x1 - midx + offset) * ratio;
        x2 = midx + (x2 - midx + offset) * ratio;
    }

    area->x = floor(x1);
    area->y = floor(y1);
    area->width = ceil(x2) - area->x;
    area->height = ceil(y2) - area->y;

    if (!game_renders_to_framebuffer ||
        (gfx_msaa_level > 1 && gfx_current_dimensions.width == gfx_current_game_window_viewport.width &&
            gfx_current_dimensions.height == gfx_current_game_window_viewport.height)) {
        area->x += gfx_current_game_window_viewport.x;
        area->y += gfx_current_window_dimensions.height -
                    (gfx_current_game_window_viewport.y + gfx_current_game_window_viewport.height);
    }
}

/* D316: the on-window pixel rect the full VI canvas (0,0)-(SCREEN_WIDTH,
 * SCREEN_HEIGHT) currently maps to, i.e. exactly the rect gfx_draw_rectangle's
 * default_viewport (below) resolves to once the safe-area crop above is
 * applied. Reuses gfx_adjust_viewport_or_scissor itself (not a hand-derived
 * inverse) so this can never drift from the real forward transform: port-side
 * mouse-to-logical-2D-space mapping (port/src/optionsoverlay.c) needs this to
 * invert clicks correctly when the crop shrinks that mapped rect below the
 * full window (D316 -- the overlay's own click math previously assumed the
 * logical canvas always fills the whole window, which is false whenever the
 * last-set gameplay viewport was inset, e.g. NTSC "Full" removes ~8% top and
 * bottom). Top-left origin, window pixel units -- matches SDL mouse coords
 * and gfx_current_game_window_viewport's documented convention. */
extern "C" void gfx_get_ui_screen_rect(int32_t *outX, int32_t *outY, int32_t *outW, int32_t *outH) {
    struct XYWidthHeight area = { 0, (int16_t)GE_LOGICAL_HEIGHT, (uint32_t)GE_LOGICAL_WIDTH, (uint32_t)GE_LOGICAL_HEIGHT };
    gfx_adjust_viewport_or_scissor(&area, false, true);   /* D510: the overlay is window-space */
    *outX = area.x;
    *outY = area.y;
    *outW = (int32_t)area.width;
    *outH = (int32_t)area.height;
}

static void gfx_calc_and_set_viewport(const Vp_t* viewport) {
    // 2 bits fraction
    float width = 2.0f * viewport->vscale[0] / 4.0f;
    float height = 2.0f * viewport->vscale[1] / 4.0f;
    float x = (viewport->vtrans[0] / 4.0f) - width / 2.0f;
    float y = ((viewport->vtrans[1] / 4.0f) + height / 2.0f);

    rdp.viewport.x = x;
    rdp.viewport.y = y;
    rdp.viewport.width = width;
    rdp.viewport.height = height;

    /* Cache the raw (pre window-scale) viewport bounds for the safe-area
     * crop above -- guard against a degenerate/zero-height viewport so a
     * later divide can't ever see one. */
    if (height >= 0.9f * (float)GE_LOGICAL_HEIGHT) {
        g_gpSafeTop = y;
        g_gpSafeHeight = height;
    } else if (height > 1.0f && g_gpSafeHeight <= 0.0f) {
        /* D447: Wide/Cinema (and split-screen) viewports are the player's own
         * letterbox, never the overscan margin: don't let them define the crop
         * band, or the crop would stretch them to fill the window. If no full
         * viewport has been seen yet, seed the standard centred 220-line band
         * (VIEWPORT_HEIGHT_DEFAULT_NTSC) around this viewport's centre. */
        const float centre = y - height * 0.5f;
        const float bandH = 220.0f * (float)GE_LOGICAL_HEIGHT / 240.0f;
        g_gpSafeTop = centre + bandH * 0.5f;
        g_gpSafeHeight = bandH;
    }

    gfx_adjust_viewport_or_scissor(&rdp.viewport);

    rdp.viewport_or_scissor_changed = true;
}

static void gfx_sp_movemem(uint8_t index, uint8_t offset, const void* data) {
    if (!fast3d_ptr_ok(data)) {
        return; /* D146: corrupt DL -> wild pointer */
    }
    switch (index) {
        case G_MV_VIEWPORT:
            gfx_calc_and_set_viewport((const Vp_t*)data);
            break;
        case G_MV_LOOKATY:
        case G_MV_LOOKATX:
            // I think this is only really used for guLookAtReflect
            index = !((index - G_MV_LOOKATY) / 2);
            rsp.lookat[index] = ((const Light *)data)->l;
            rsp.lookat_enabled = (index == 0) || (rsp.lookat[1].dir[0] || rsp.lookat[1].dir[1]);
            rsp.lights_changed = true;
            break;
        case G_MV_L0:
        case G_MV_L1:
        case G_MV_L2:
            // NOTE: reads out of bounds if it is an ambient light
            memcpy(rsp.current_lights + (index - G_MV_L0) / 2, data, sizeof(Light_t));
            break;
    }
}

static void gfx_sp_moveword(uint8_t index, uint16_t offset, uintptr_t data) {
    switch (index) {
        case G_MW_NUMLIGHT:
            // Ambient light is included
            // The 31th bit is a flag that lights should be recalculated
            rsp.current_num_lights = (data - 0x80000000U) / 32;
            rsp.lights_changed = 1;
            break;
        case G_MW_FOG:
            rsp.fog_mul = (int16_t)(data >> 16);
            rsp.fog_offset = (int16_t)data;
            break;
        case G_MW_SEGMENT:
            // GE registers segment bases as OS_K0_TO_PHYSICAL(ptr); store the
            // live host pointer so seg_addr() resolves seg+offset correctly.
            if ((offset>>2)>=16u) sysFatalError("Invalid Fast3D segment index");
            segmentPointers[offset>>2]=(uintptr_t)geNativeAddressToPointer(data);
            break;
    }
}

static void gfx_sp_texture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) {
    rsp.texture_scaling_factor.s = sc;
    rsp.texture_scaling_factor.t = tc;
    rdp.tex_max_lod = level;
    if (rdp.first_tile_index != tile) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
        rdp.first_tile_index = tile;
    }
}

static void gfx_dp_set_scissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    float x = ulx / 4.0f;
    float y = lry / 4.0f;
    float width = (lrx - ulx) / 4.0f;
    float height = (lry - uly) / 4.0f;

    // D493b: with the overscan crop off (the Original N64 preset turns it
    // off) D246's 1-unit trim is off too, and its gap at the left/right
    // edges showed live scene/background pixels. Keep the geometry unscaled
    // and scissor those two logical columns out instead, so they show the
    // frame clear (black) like the rest of the overscan border.
    if (!g_safe_area_crop_enabled && !g_split_screen && !g_overlay_window_space) {
        const float edge = 1.0f;
        if (x < edge) {
            width -= edge - x;
            x = edge;
        }
        if (x + width > (float)GE_LOGICAL_WIDTH - edge) {
            width = (float)GE_LOGICAL_WIDTH - edge - x;
        }
        if (width < 0.0f) {
            width = 0.0f;
        }
    }

    rdp.scissor.x = x;
    rdp.scissor.y = y;
    rdp.scissor.width = width;
    rdp.scissor.height = height;

    gfx_adjust_viewport_or_scissor(&rdp.scissor, rsp.aspect_mode != 0);

    rdp.viewport_or_scissor_changed = true;
}

static void gfx_dp_set_texture_image(uint32_t format, uint32_t size, uint32_t width, uint32_t tex_flags, const void* addr) {
    rdp.texture_to_load.addr = (const uint8_t*)addr;
    rdp.texture_to_load.fmt = (uint8_t)format; /* D229 */
    rdp.texture_to_load.siz = size;
    rdp.texture_to_load.width = width;
    rdp.texture_to_load.tex_flags = tex_flags;
}

static bool gfx_dp_set_tile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, uint32_t palette,
                            uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks,
                            uint32_t shifts) {
    // OTRTODO:
    // SUPPORT_CHECK(tmem == 0 || tmem == 256);
    static uint32_t max_tmem = 0;
    if (cms == G_TX_WRAP && masks == G_TX_NOMASK) {
        cms = G_TX_CLAMP;
    }
    if (cmt == G_TX_WRAP && maskt == G_TX_NOMASK) {
        cmt = G_TX_CLAMP;
    }

    if (fmt == G_IM_FMT_RGBA && siz < G_IM_SIZ_16b) {
        // HACK: sometimes the game will submit G_IM_FMT_RGBA, G_IM_SIZ_8b/4b, intending it to read as CI8/CI4 with RGBA16 palette
        fmt = G_IM_FMT_CI;
    } else if (fmt == G_IM_FMT_IA && siz == G_IM_SIZ_32b) {
        // HACK: ... and sometimes it submits this, apparently intending it to be I8
        fmt = G_IM_FMT_I;
        siz = G_IM_SIZ_8b;
    }

    /* D474 (low-end perf): a set_tile that changes nothing must not mark the
     * textures dirty -- that forces a texture-cache lookup AND a gfx_flush()
     * (batch break / extra draw call) on the next triangle. Loads, TLUTs and
     * G_TEXTURE still dirty the textures through their own paths. */
    auto& tt = rdp.texture_tile[tile];
    const bool same = tt.palette == palette && tt.fmt == fmt && tt.siz == siz &&
                      tt.cms == cms && tt.cmt == cmt && tt.masks == masks && tt.maskt == maskt &&
                      tt.shifts == shifts && tt.shiftt == shiftt &&
                      tt.line_size_bytes == line * 8 && tt.tmem == tmem;
    tt.palette = palette; // palette should set upper 4 bits of color index in 4b mode
    tt.fmt = fmt;
    tt.siz = siz;
    tt.cms = cms;
    tt.cmt = cmt;
    tt.masks = masks; /* RC3 */
    tt.maskt = maskt; /* RC3 */
    tt.shifts = shifts;
    tt.shiftt = shiftt;
    tt.line_size_bytes = line * 8;
    tt.tmem = tmem;

    if (!same) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    return !same; /* PH5C: preserve original writes, report actual change. */
}

static bool gfx_dp_set_tile_size(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
    auto& tt = rdp.texture_tile[tile];
    if (tt.uls == uls && tt.ult == ult && tt.lrs == lrs && tt.lrt == lrt) {
        return false;   /* D474: unchanged tile size -- no re-import, no batch break */
    }
    tt.uls = uls;
    tt.ult = ult;
    tt.lrs = lrs;
    tt.lrt = lrt;
    tt.width = (lrs - uls + 4) / 4;
    tt.height = (lrt - ult + 4) / 4;
    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
    return true;
}

static void gfx_dp_load_tlut(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    // SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(rdp.texture_to_load.siz == G_IM_SIZ_16b);
    SUPPORT_CHECK(rdp.texture_tile[tile].tmem >= 256);

    rdp.texture_tile[tile].uls = uls;
    rdp.texture_tile[tile].ult = ult;
    rdp.texture_tile[tile].lrs = lrs;
    rdp.texture_tile[tile].lrt = lrt;

    const uint32_t width = (lrs - uls + 1);
    const uint32_t height = (lrt - ult + 1);
    const uint32_t pitch = rdp.texture_to_load.width + 1;
    const uint32_t count =  width * height;
    const uint16_t *base = (const uint16_t *)rdp.texture_to_load.addr + pitch * ult + uls;

    if (rdp.texture_tile[tile].tmem == 256) {
        rdp.palette_addrs[0] = (const uint8_t *)base;
        if (count >= 256) {
            rdp.palette_addrs[1] = (const uint8_t *)(base + 128);
        }
    } else {
        rdp.palette_addrs[1] = (const uint8_t *)base;
    }

    const uint32_t palofs = rdp.texture_tile[tile].tmem - 256;
    SUPPORT_CHECK(palofs + count <= 256);

    const uint16_t *src = base;
    uint16_t *dst = rdp.palette + palofs;
    for (uint32_t i = 0; i < count; ++i) {
        *dst++ = PD_BE16(*src++);
    }

    /* A reused TLUT source may contain new bytes (D217). Invalidate every
     * bank touched by this copy, retaining D476's independence from unrelated
     * CI4 banks. A later CI8 import still hashes the entire current palette. */
    rdp.palette_hash_dirty=true;
    const unsigned bank_end=(palofs+count+15u)/16u;
    for(unsigned bank=palofs/16u;bank<bank_end;++bank)
        rdp.palette_bank_hash_valid&=(uint16_t)~(1u<<bank);

    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
}

static void gfx_dp_load_block(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    // SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(uls == 0);
    SUPPORT_CHECK(ult == 0);

    // The lrs field rather seems to be number of pixels to load
    uint32_t orig_size_bytes = (lrs + 1) << rdp.texture_to_load.siz >> 1;
    uint32_t size_bytes = orig_size_bytes;
    if (rdp.texture_to_load.raw_tex_metadata.h_byte_scale != 1 ||
        rdp.texture_to_load.raw_tex_metadata.v_pixel_scale != 1) {
        size_bytes *= rdp.texture_to_load.raw_tex_metadata.h_byte_scale;
        size_bytes *= rdp.texture_to_load.raw_tex_metadata.v_pixel_scale;
    }

    LoadedTexture& loaded_texture = rdp.loaded_texture[rdp.texture_tile[tile].tmem];
    loaded_texture.orig_size_bytes = orig_size_bytes;
    loaded_texture.size_bytes = size_bytes;
    loaded_texture.full_size_bytes = size_bytes;
    loaded_texture.line_size_bytes = size_bytes;
    loaded_texture.full_image_line_size_bytes = size_bytes;
    loaded_texture.tex_flags = rdp.texture_to_load.tex_flags;
    loaded_texture.raw_tex_metadata = rdp.texture_to_load.raw_tex_metadata;
    loaded_texture.addr = rdp.texture_to_load.addr;
    loaded_texture.src_fmt = rdp.texture_to_load.fmt; /* D229 */
    loaded_texture.dxt0 = (dxt == 0);                 /* D75 */

    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
}

static void gfx_dp_load_tile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    SUPPORT_CHECK(tile == G_TX_LOADTILE);

    uint32_t offset_x = uls >> G_TEXTURE_IMAGE_FRAC;
    uint32_t offset_y = ult >> G_TEXTURE_IMAGE_FRAC;
    uint32_t tile_width = ((lrs - uls) >> G_TEXTURE_IMAGE_FRAC) + 1;
    uint32_t tile_height = ((lrt - ult) >> G_TEXTURE_IMAGE_FRAC) + 1;
    uint32_t full_image_width = rdp.texture_to_load.width + 1;

    uint32_t offset_x_in_bytes = offset_x << rdp.texture_to_load.siz >> 1;
    uint32_t tile_line_size_bytes = tile_width << rdp.texture_to_load.siz >> 1;
    uint32_t full_image_line_size_bytes = full_image_width << rdp.texture_to_load.siz >> 1;

    uint32_t orig_size_bytes = tile_line_size_bytes * tile_height;
    uint32_t size_bytes = orig_size_bytes;
    uint32_t start_offset_bytes = full_image_line_size_bytes * offset_y + offset_x_in_bytes;

    float h_byte_scale = rdp.texture_to_load.raw_tex_metadata.h_byte_scale;
    float v_pixel_scale = rdp.texture_to_load.raw_tex_metadata.v_pixel_scale;

    if (h_byte_scale != 1 || v_pixel_scale != 1) {
        start_offset_bytes = h_byte_scale * (v_pixel_scale * offset_y * full_image_line_size_bytes + offset_x_in_bytes);
        size_bytes *= h_byte_scale * v_pixel_scale;
        full_image_line_size_bytes *= h_byte_scale;
        tile_line_size_bytes *= h_byte_scale;
    }

    LoadedTexture& loaded_texture = rdp.loaded_texture[rdp.texture_tile[tile].tmem];
    loaded_texture.orig_size_bytes = orig_size_bytes;
    loaded_texture.size_bytes = size_bytes;
    loaded_texture.full_size_bytes = full_image_line_size_bytes * tile_height;
    loaded_texture.full_image_line_size_bytes = full_image_line_size_bytes;
    loaded_texture.line_size_bytes = tile_line_size_bytes;
    loaded_texture.tex_flags = rdp.texture_to_load.tex_flags;
    loaded_texture.raw_tex_metadata = rdp.texture_to_load.raw_tex_metadata;
    loaded_texture.addr = rdp.texture_to_load.addr + start_offset_bytes;
    loaded_texture.src_fmt = rdp.texture_to_load.fmt; /* D229 */
    loaded_texture.dxt0 = false;                      /* D75 */

    rdp.texture_tile[tile].uls = uls;
    rdp.texture_tile[tile].ult = ult;
    rdp.texture_tile[tile].lrs = lrs;
    rdp.texture_tile[tile].lrt = lrt;
    rdp.texture_tile[tile].width = ((lrs - uls) >> G_TEXTURE_IMAGE_FRAC) + 1;
    rdp.texture_tile[tile].height = ((lrt - ult) >> G_TEXTURE_IMAGE_FRAC) + 1;

    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
}

static void gfx_dp_set_combine_mode(uint32_t rgb, uint32_t alpha, uint32_t rgb_cyc2, uint32_t alpha_cyc2) {
    rdp.combine_mode = rgb | (alpha << 16) | ((uint64_t)rgb_cyc2 << 28) | ((uint64_t)alpha_cyc2 << 44);
}

static inline uint32_t color_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a & 0xf) | ((b & 0xf) << 4) | ((c & 0x1f) << 8) | ((d & 7) << 13);
}

static inline uint32_t alpha_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a & 7) | ((b & 7) << 3) | ((c & 7) << 6) | ((d & 7) << 9);
}

static void gfx_dp_set_grayscale_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.grayscale_color.r = r;
    rdp.grayscale_color.g = g;
    rdp.grayscale_color.b = b;
    rdp.grayscale_color.a = a;
}

static void gfx_dp_set_env_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.env_color.r = r;
    rdp.env_color.g = g;
    rdp.env_color.b = b;
    rdp.env_color.a = a;
}

static void gfx_dp_set_prim_color(uint8_t m, uint8_t l, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.prim_lod_fraction = l;
    rdp.prim_color.r = r;
    rdp.prim_color.g = g;
    rdp.prim_color.b = b;
    rdp.prim_color.a = a;
    rdp.fill_color.r = r;
    rdp.fill_color.g = g;
    rdp.fill_color.b = b;
    rdp.fill_color.a = a;
    rdp.tex_min_lod = m;

}

static void gfx_dp_set_fog_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.fog_color.r = r;
    rdp.fog_color.g = g;
    rdp.fog_color.b = b;
    rdp.fog_color.a = a;
}

static void gfx_dp_set_fill_color(uint32_t packed_color) {
    uint16_t col16 = (uint16_t)packed_color;
    uint32_t r = col16 >> 11;
    uint32_t g = (col16 >> 6) & 0x1f;
    uint32_t b = (col16 >> 1) & 0x1f;
    uint32_t a = col16 & 1;
    rdp.fill_color.r = SCALE_5_8(r);
    rdp.fill_color.g = SCALE_5_8(g);
    rdp.fill_color.b = SCALE_5_8(b);
    rdp.fill_color.a = a * 255;
}

static void gfx_dp_set_subpixel_offset(int16_t x, int16_t y) {
    rdp.subpixel_ofs_x = x;
    rdp.subpixel_ofs_y = y;
}

static void gfx_draw_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    uint32_t saved_other_mode_h = rdp.other_mode_h;
    uint32_t cycle_type = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));

    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = (rdp.other_mode_h & ~(3U << G_MDSFT_TEXTFILT)) | G_TF_POINT;
    }

    ulx += rdp.subpixel_ofs_x;
    lrx += rdp.subpixel_ofs_x;
    uly += rdp.subpixel_ofs_y;
    lry += rdp.subpixel_ofs_y;

    // D397: the N64 RDP floors the U10.2 top/bottom edges of a texture rect to
    // whole scanlines before its per-scanline coverage test, so a rect covers
    // scanlines [floor(uly/4), floor((lry-1)/4]]. A rect whose height is not a
    // whole number of native pixels therefore rasterizes to a whole number of
    // scanlines on hardware at every window scale. The gun-barrel / file-select
    // gradient is built from 0.75px-tall row strips (title2.c: uly=(i+12)<<2,
    // lry=((i+13)<<2)-1 -> 0.75px tall at 1px pitch); on the RDP each strip is
    // exactly one scanline. The continuous-NDC conversion below instead passes
    // the fractional bottom edge straight to the GPU, whose coverage test
    // aliases the 0.75px pitch into a visible "comb" (H2: period fixed in
    // native rows, ~3 = the 0.75px-at-1px-pitch beat, window period = 3*scale).
    // Snap the top/bottom edges to the RDP scanline range so a sub-pixel strip
    // rasterizes to exactly the scanline it targets. No-op for whole-pixel
    // rects (edges already multiples of 4) and for the copy-mode +1<<2 edge
    // (already a whole scanline), so normal fills are unchanged.
    if (lry > uly) {
        const int32_t top_scan = uly >> 2;
        const int32_t bot_scan = (lry - 1) >> 2;
        uly = top_scan << 2;
        lry = (bot_scan + 1) << 2;
    }

    // U10.2 coordinates
    float ulxf = ulx;
    float ulyf = uly;
    float lrxf = lrx;
    float lryf = lry;

    ulxf = ulxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    ulyf = -(ulyf / (4.0f * HALF_SCREEN_HEIGHT)) + 1.0f;
    lrxf = lrxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    lryf = -(lryf / (4.0f * HALF_SCREEN_HEIGHT)) + 1.0f;

    ulxf = gfx_adjust_x_for_aspect_ratio(ulxf);
    lrxf = gfx_adjust_x_for_aspect_ratio(lrxf);

    struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];

    ul->x = ulxf;
    ul->y = ulyf;
    ul->z = -1.0f;
    ul->w = 1.0f;

    ll->x = ulxf;
    ll->y = lryf;
    ll->z = -1.0f;
    ll->w = 1.0f;

    lr->x = lrxf;
    lr->y = lryf;
    lr->z = -1.0f;
    lr->w = 1.0f;

    ur->x = lrxf;
    ur->y = ulyf;
    ur->z = -1.0f;
    ur->w = 1.0f;

    // The coordinates for texture rectangle shall bypass the viewport setting
    struct XYWidthHeight default_viewport = { 0, (int16_t)GE_LOGICAL_HEIGHT, (uint32_t)GE_LOGICAL_WIDTH, (uint32_t)GE_LOGICAL_HEIGHT };
    struct XYWidthHeight viewport_saved = rdp.viewport;
    uint32_t geometry_mode_saved = rsp.geometry_mode;

    gfx_adjust_viewport_or_scissor(&default_viewport);

    rdp.viewport = default_viewport;
    rdp.viewport_or_scissor_changed = true;
    rsp.geometry_mode = 0;

#if defined(GE_XBOX_NATIVE32) && GE_XBOX_FAST3D_RECT_PAIR_MATERIAL
    const unsigned prepBeforeFirstHalf=s_ge_tri_preparations;
    s_ge_rect_second_half=false;
#endif
    gfx_sp_tri1(MAX_VERTICES + 0, MAX_VERTICES + 1, MAX_VERTICES + 3, true);
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_FAST3D_RECT_PAIR_MATERIAL
    /* If the first half was rejected before setup, the second must prepare
     * normally. No state commands execute between these two triangle calls.
     * Dirty texture/viewport flags and diagnostic probes still force setup. */
    s_ge_rect_second_half=(s_ge_tri_preparations!=prepBeforeFirstHalf) &&
        !GENativePlatform_HasFatalError();
#endif
    gfx_sp_tri1(MAX_VERTICES + 1, MAX_VERTICES + 2, MAX_VERTICES + 3, true);
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_FAST3D_RECT_PAIR_MATERIAL
    s_ge_rect_second_half=false;
#endif

    rsp.geometry_mode = geometry_mode_saved;
    rdp.viewport = viewport_saved;
    rdp.viewport_or_scissor_changed = true;

    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = saved_other_mode_h;
    }
}

/* D226: HUD span scale (G_HUDSCALE_EXT, emitted by port/include/hudaspect.h
 * PORT_HUD_SCALE around specific HUD draws). While active, every rectangle
 * (text glyphs, message boxes, ammo icons are all rects) is scaled about the
 * anchor in logical 10.2 screen space, and texrect steps are divided by the
 * scale so the same texels cover the larger/smaller rect. 1.0 = inactive. */
static float s_hud_scale = 1.0f;
static int32_t s_hud_ax = 0, s_hud_ay = 0;   /* anchor, 10.2 fixed (px*4) */

/* D510: uniform scale about the origin for the port overlay DL, which lays
 * itself out on a canonical 320-wide canvas whatever the game's current VI
 * canvas is (440x330 on the front end). Applied after the HUD span scale. */
static float s_overlay_scale = 1.0f;
extern "C" void gfx_set_overlay_scale(float s) {
    s_overlay_scale = (s > 0.0f) ? s : 1.0f;
}

static inline void gfx_hud_scale_rect(int32_t& ulx, int32_t& uly, int32_t& lrx, int32_t& lry) {
    if (s_overlay_scale != 1.0f && g_overlay_window_space) {
        const bool hud = (s_hud_scale != 1.0f);
        if (hud) {
            ulx = s_hud_ax + (int32_t)lroundf((float)(ulx - s_hud_ax) * s_hud_scale);
            lrx = s_hud_ax + (int32_t)lroundf((float)(lrx - s_hud_ax) * s_hud_scale);
            uly = s_hud_ay + (int32_t)lroundf((float)(uly - s_hud_ay) * s_hud_scale);
            lry = s_hud_ay + (int32_t)lroundf((float)(lry - s_hud_ay) * s_hud_scale);
        }
        ulx = (int32_t)lroundf((float)ulx * s_overlay_scale);
        lrx = (int32_t)lroundf((float)lrx * s_overlay_scale);
        uly = (int32_t)lroundf((float)uly * s_overlay_scale);
        lry = (int32_t)lroundf((float)lry * s_overlay_scale);
        if (ulx < 0) ulx = 0;
        if (uly < 0) uly = 0;
        return;
    }
    if (s_hud_scale == 1.0f) {
        return;
    }
    ulx = s_hud_ax + (int32_t)lroundf((float)(ulx - s_hud_ax) * s_hud_scale);
    lrx = s_hud_ax + (int32_t)lroundf((float)(lrx - s_hud_ax) * s_hud_scale);
    uly = s_hud_ay + (int32_t)lroundf((float)(uly - s_hud_ay) * s_hud_scale);
    lry = s_hud_ay + (int32_t)lroundf((float)(lry - s_hud_ay) * s_hud_scale);
    /* A full-width box (e.g. the dialogue backdrop, 0..W) scaled about a
     * centre anchor goes negative; negative rect coords are not drawn, so
     * clip at the canvas origin (the right/bottom overhang is harmless). */
    if (ulx < 0) ulx = 0;
    if (uly < 0) uly = 0;
}

static void gfx_dp_texture_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls,
                                     int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
    const float ov = (g_overlay_window_space && s_overlay_scale != 1.0f) ? s_overlay_scale : 1.0f;   /* D510 */
    if (s_hud_scale != 1.0f || ov != 1.0f) {   /* D226 */
        gfx_hud_scale_rect(ulx, uly, lrx, lry);
        dsdx = (int16_t)lroundf((float)dsdx / (s_hud_scale * ov));
        dtdy = (int16_t)lroundf((float)dtdy / (s_hud_scale * ov));
    }
    uint64_t saved_combine_mode = rdp.combine_mode;
    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY) {
        // Per RDP Command Summary Set Tile's shift s and this dsdx should be set to 4 texels
        // Divide by 4 to get 1 instead
        dsdx >>= 2;

        // Color combiner is turned off in copy mode
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_TEXEL0), alpha_comb(0, 0, 0, G_ACMUX_TEXEL0), 0, 0);

        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }

    // uls and ult are S10.5
    // dsdx and dtdy are S5.10
    // lrx, lry, ulx, uly are U10.2
    // lrs, lrt are S10.5

    const int16_t width = flip ? lry - uly : lrx - ulx;
    const int16_t height = flip ? lrx - ulx : lry - uly;
    const float lrs = ((uls << 7) + dsdx * width) >> 7;
    const float lrt = ((ult << 7) + dtdy * height) >> 7;

    struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];
    ul->u = uls;
    ul->v = ult;
    lr->u = lrs;
    lr->v = lrt;
    if (!flip) {
        ll->u = uls;
        ll->v = lrt;
        ur->u = lrs;
        ur->v = ult;
    } else {
        ll->u = lrs;
        ll->v = ult;
        ur->u = uls;
        ur->v = lrt;
    }

    uint8_t saved_tile = rdp.first_tile_index;
    if (saved_tile != tile) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    rdp.first_tile_index = tile;

    s_in_texrect = true;
    gfx_draw_rectangle(ulx, uly, lrx, lry);
    s_in_texrect = false;
    if (saved_tile != tile) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    rdp.first_tile_index = saved_tile;
    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_image_rectangle(int32_t tile, int32_t w, int32_t h,
                                   int32_t ulx, int32_t uly, int16_t uls, int16_t ult,
                                   int32_t lrx, int32_t lry, int16_t lrs, int16_t lrt) {
    uint64_t saved_combine_mode = rdp.combine_mode;

    struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];
    ul->u = uls * 32;
    ul->v = ult * 32;
    lr->u = lrs * 32;
    lr->v = lrt * 32;
    ll->u = uls * 32;
    ll->v = lrt * 32;
    ur->u = lrs * 32;
    ur->v = ult * 32;

    // ensure we have the correct texture size
    rdp.texture_tile[tile].line_size_bytes = w << rdp.texture_tile[tile].siz >> 1;
    rdp.texture_tile[tile].width = w;
    rdp.texture_tile[tile].height = h;
    rdp.texture_tile[tile].cms = 0;
    rdp.texture_tile[tile].cmt = 0;
    rdp.texture_tile[tile].shifts = 0;
    rdp.texture_tile[tile].shiftt = 0;
    auto& loadtex = rdp.loaded_texture[rdp.texture_tile[tile].tmem];
    loadtex.full_image_line_size_bytes = loadtex.line_size_bytes = rdp.texture_tile[tile].line_size_bytes;
    loadtex.size_bytes = loadtex.orig_size_bytes = loadtex.full_size_bytes = loadtex.line_size_bytes * h;

    uint8_t saved_tile = rdp.first_tile_index;
    if (saved_tile != tile) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    rdp.first_tile_index = tile;

    s_in_texrect = true;
    gfx_draw_rectangle(ulx, uly, lrx, lry);
    s_in_texrect = false;
    if (saved_tile != tile) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    rdp.first_tile_index = saved_tile;

    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_fill_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    gfx_hud_scale_rect(ulx, uly, lrx, lry);   /* D226 */
    if (rdp.color_image_address == rdp.z_buf_address) {
        // Don't clear Z buffer here since we already did it with glClear
        return;
    }
    uint32_t mode = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));

    // OTRTODO: This is a bit of a hack for widescreen screen fades, but it'll work for now...
    if (ulx == 0 && uly == 0 && lrx == 319 * 4 && lry == 239 * 4) {
        ulx = -1024;
        uly = -1024;
        lrx = 2048;
        lry = 2048;
    }

    if (mode == G_CYC_COPY || mode == G_CYC_FILL) {
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }

    for (int i = MAX_VERTICES; i < MAX_VERTICES + 4; i++) {
        struct LoadedVertex* v = &rsp.loaded_vertices[i];
        v->color = rdp.fill_color;
    }

    uint64_t saved_combine_mode = rdp.combine_mode;

    if (mode == G_CYC_FILL) {
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_SHADE), alpha_comb(0, 0, 0, G_ACMUX_SHADE), 0, 0);
    }

    gfx_draw_rectangle(ulx, uly, lrx, lry);
    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_set_z_image(void* z_buf_address) {
    rdp.z_buf_address = z_buf_address;
}

static void gfx_dp_set_color_image(uint32_t format, uint32_t size, uint32_t width, void* address) {
    rdp.color_image_address = address;
}

static void gfx_sp_set_other_mode(uint32_t shift, uint32_t num_bits, uint64_t mode) {
    uint64_t mask = (((uint64_t)1 << num_bits) - 1) << shift;
    uint64_t om = rdp.other_mode_l | ((uint64_t)rdp.other_mode_h << 32);
    om = (om & ~mask) | mode;
    rdp.other_mode_l = (uint32_t)om;
    rdp.other_mode_h = (uint32_t)(om >> 32);
    rdp.palette_fmt = rdp.other_mode_h & (3U << G_MDSFT_TEXTLUT);
    rdp.tex_lod = (rdp.other_mode_h & G_TL_LOD) != 0;
    rdp.tex_detail = (rdp.other_mode_h & (2U << G_MDSFT_TEXTDETAIL)) == G_TD_DETAIL;
}

static void gfx_sp_set_vertex_colors(uint32_t count, const struct NormalColor *vcn) {
    // common sense dictates that we should copy the colors as the command is supposed to do,
    // but it actually doesn't seem to matter
    // SUPPORT_CHECK(count <= sizeof(rsp.vertex_colors) / sizeof(rsp.vertex_colors[0]));
    // for (uint32_t i = 0; i < count; ++i) {
    //     rsp.vertex_colors[i] = vcn[i];
    // }
    if (fast3d_ptr_ok(vcn)) { /* D146: ignore a wild pointer from a corrupt DL */
        rsp.vertex_colors = vcn;
    }
}

static void gfx_dp_set_other_mode(uint32_t h, uint32_t l) {
    rdp.other_mode_h = h;
    rdp.other_mode_l = l;
}

/* D245 (M-201): G_FLOATVTX_EXT loader -- gfx_sp_vertex for pre-transformed
 * clip-space float vertices with float S/T (see port/include/floatvtx.h).
 * Only the unlit, unfogged path the sky/water fans use is supported. */
static void gfx_sp_vertex_float(size_t n_vertices, size_t dest_index, const PortFloatVtx* vertices) {
    if (!fast3d_ptr_ok(vertices) || dest_index + n_vertices > MAX_VERTICES) {
        return;
    }
    for (size_t i = 0; i < n_vertices; i++, dest_index++) {
        const PortFloatVtx* v = &vertices[i];
        struct LoadedVertex* d = &rsp.loaded_vertices[dest_index];
        geTriVertexInvalidated((unsigned)dest_index);
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_NATIVE_ROOM_BATCHES
        s_roomRawSlot[dest_index].valid=false;
#endif
        float x = gfx_adjust_x_for_aspect_ratio(v->x, v->w);
        float y = v->y, z = v->z, w = v->w;
        d->u = v->s * (float)rsp.texture_scaling_factor.s / 65536.0f;
        d->v = v->t * (float)rsp.texture_scaling_factor.t / 65536.0f;
        d->color.r = v->r;
        d->color.g = v->g;
        d->color.b = v->b;
        d->color.a = v->a;
        d->clip_rej = 0;
        if (x < -w) d->clip_rej |= 1;
        if (x > w) d->clip_rej |= 2;
        if (y < -w) d->clip_rej |= 4;
        if (y > w) d->clip_rej |= 8;
        if (z > w) d->clip_rej |= 32;
        d->x = x;
        d->y = y;
        d->z = z;
        d->w = w;
        d->fog = rdp.fog_color.a;
        d->fog_n = gfx_fog_vertex ? d->fog : d->fog * w;
    }
}

static inline void* seg_addr(uintptr_t address) {
    /* Tagged DRAM addresses and live arena pointers are unambiguous on Xbox;
     * untagged small numbers are not presumed to be physical offsets. */
    const uintptr_t top=address&0xff800000u;
    if(top==0x70000000u || top==0x80000000u || top==0xa0000000u || top==0xf0000000u)
        return geNativeAddressToPointer(address);
    if(geNativeAddressIsRam(address) || geNativeAddressIsNativeImage(address)) return (void*)address;
    const unsigned seg=(unsigned)((address>>24)&15u);
    if(address<0x10000000u && seg && segmentPointers[seg]) {
        /* Native resource conversion preserves the original N64 byte offset;
         * it never adds the desktop sidecar's low-bit pointer marker. */
        const uintptr_t offset=address&0x00ffffffu;
        return (void*)(segmentPointers[seg]+offset);
    }
    return geNativeAddressToPointer(address);
}

uintptr_t clearMtx;

static unsigned s_nativeCommandBudget=0;
static unsigned s_nativeDLDepth=0;
static unsigned s_nativeMaxDLDepth=0;
static int s_nativeGfxFailed=0;
static Gfx *s_nativeTaskBegin=0,*s_nativeTaskEnd=0;
enum { NATIVE_DL_MAX_DEPTH=128 };
struct NativeDLReturn {
    Gfx *command,*start;
    bool bounded;
};
/* A recursive interpreter frame includes diagnostic and command scratch.
 * 128 such frames exceed the native worker's 64 KiB stack. Keep only list
 * return metadata here and execute every list in one native call frame. */
static NativeDLReturn s_nativeDLReturns[NATIVE_DL_MAX_DEPTH-1];
struct NativeDLGuard { NativeDLGuard(){s_nativeDLDepth=1;} ~NativeDLGuard(){s_nativeDLDepth=0;} };
static bool nativeDLInTask(const Gfx *command) {
    const uintptr_t p=(uintptr_t)command;
    return p>=(uintptr_t)s_nativeTaskBegin && p<(uintptr_t)s_nativeTaskEnd;
}
static bool nativeDLPop(Gfx *&command,Gfx *&start,bool &bounded) {
    if(--s_nativeDLDepth==0)return false;
    const NativeDLReturn& parent=s_nativeDLReturns[s_nativeDLDepth-1u];
    command=parent.command;start=parent.start;bounded=parent.bounded;
    return true;
}
/* Phase 0D: the N64 command stream is classified before the interpreter's
 * main opcode switch. Use one 256-bit lookup instead of a second sparse
 * switch for whether a command dirties the prepared triangle material.
 * Exact same clean opcodes, including TRI4, VTX, matrix, calls and no-ops.
 * No command skipping or change to display-list order. A/B: set to 0. */
#ifndef GE_XBOX_FAST3D_OPCODE_STATE_MASK
#define GE_XBOX_FAST3D_OPCODE_STATE_MASK 1
#endif
/* PH5C: do not invalidate cached triangle material for idempotent texture
 * tile or combine commands. All commands still execute in original order. */
#ifndef GE_XBOX_FAST3D_EQUIVALENT_STATE_SKIP
#define GE_XBOX_FAST3D_EQUIVALENT_STATE_SKIP 1
#endif
#if GE_XBOX_PHASE0_PROFILE && GE_XBOX_PHASE0E_PROFILE
/* One mutually-exclusive opcode class per command. Child scopes move time
 * from that opcode into material/texture/clip/packing/backend submission. */
static int geP0EOpcodeCategory(uint32_t opcode) {
    switch(opcode) {
        case (uint8_t)G_TRI1: case (uint8_t)G_TRI4:
            return GE_P0E_TRIANGLE;
        case G_VTX: case G_FLOATVTX_EXT:
            return GE_P0E_VERTEX;
        case G_MTX: case (uint8_t)G_POPMTX:
            return GE_P0E_MATRIX;
        case G_SETTIMG: case G_SETTIMG_FB_EXT: case G_SETTILE:
        case G_SETTILESIZE: case G_LOADBLOCK: case G_LOADTILE:
        case G_LOADTLUT: case G_INVALTEXCACHE_EXT:
            return GE_P0E_TEXTURE_STATE;
        case G_TEXRECT: case G_TEXRECTFLIP:
        case G_TEXRECT_WIDE_EXT: case G_IMAGERECT_EXT:
        case G_FILLRECT: case G_FILLRECT_WIDE_EXT:
            return GE_P0E_RECT;
        case G_DL: case (uint8_t)G_ENDDL:
        case G_SPNOOP: case G_NOOP:
        case G_XBOX_MP_VI_CANVAS_EXT: /* viewport/scissor metadata */
        case G_RDPPIPESYNC: case G_RDPFULLSYNC:
        case G_RDPLOADSYNC: case G_RDPTILESYNC:
            return GE_P0E_CONTROL;
        default: return GE_P0E_STATE;
    }
}
#endif
static void gfx_run_dl(Gfx* cmd) {
    NativeDLGuard depthGuard;
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_NATIVE_ROOM_BATCHES
    s_roomScope=-1;s_roomRevision=0;s_roomHasColorOverride=false;
    s_roomPrimaryStart=s_roomPrimaryEnd=0u;
    s_roomAttemptedStart=s_roomAttemptedUntil=0;
    s_roomFrontBuildsFrame=0;
    memset(s_roomRawSlot,0,sizeof(s_roomRawSlot));
#endif
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_FAST3D_OPCODE_STATE_MASK
    static uint32_t geCleanDLMask[8] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    static bool geCleanDLMaskReady = false;
    if (!geCleanDLMaskReady) {
        static const uint8_t geCleanDLCommands[] = {
            (uint8_t)G_TRI1, (uint8_t)G_TRI4, (uint8_t)G_VTX,
            (uint8_t)G_FLOATVTX_EXT, (uint8_t)G_MTX, (uint8_t)G_POPMTX,
            (uint8_t)G_DL, (uint8_t)G_ENDDL, (uint8_t)G_SPNOOP,
            (uint8_t)G_NOOP, (uint8_t)G_COL,
            (uint8_t)G_XBOX_MP_VI_CANVAS_EXT, 0x49u, 0x4au, 0x4bu
        };
        for (unsigned i = 0; i < sizeof(geCleanDLCommands); ++i) {
            const unsigned op = geCleanDLCommands[i];
            geCleanDLMask[op >> 5] |= (uint32_t)1u << (op & 31u);
        }
        geCleanDLMaskReady = true;
    }
#endif
    bool boundedTask=nativeDLInTask(cmd);
    if(!cmd) {
        GENativePlatform_SetError("Invalid or recursively cyclic display list");
        s_nativeGfxFailed=1;return;
    }
    Gfx* dListStart = cmd;
    if(s_nativeMaxDLDepth<1u)s_nativeMaxDLDepth=1u;

    for (;;) {
        /* DP-only task buffers are length-delimited; nested Fast3D lists
           retain their native END_DL/branch semantics. */
        if(boundedTask && cmd==s_nativeTaskEnd) {
            if(!nativeDLPop(cmd,dListStart,boundedTask))return;
            continue;
        }
        if(s_nativeGfxFailed || !s_nativeCommandBudget--) {
            if(!s_nativeGfxFailed) GENativePlatform_SetError("Display-list command budget exceeded");
            s_nativeGfxFailed=1;return;
        }
        if(boundedTask) {
            /* Check the first slot before reading its opcode. */
            if((uintptr_t)cmd>(uintptr_t)s_nativeTaskEnd ||
               (uintptr_t)s_nativeTaskEnd-(uintptr_t)cmd<sizeof(Gfx)) {
                GENativePlatform_SetError("Truncated native graphics command payload");
                s_nativeGfxFailed=1;return;
            }
        }
        uint32_t opcode = cmd->words.w0 >> 24;
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_NATIVE_ROOM_BATCHES
        if(s_roomScope>=0 && !boundedTask &&
           (!s_roomAttemptedStart ||
            (uintptr_t)cmd<(uintptr_t)s_roomAttemptedStart ||
            (uintptr_t)cmd>=(uintptr_t)s_roomAttemptedUntil) &&
           s_roomPrimaryStart && s_roomPrimaryEnd>s_roomPrimaryStart &&
           (uintptr_t)cmd>=s_roomPrimaryStart &&
           (uintptr_t)cmd<s_roomPrimaryEnd &&
           (opcode==(uint8_t)G_TRI1 || opcode==(uint8_t)G_TRI4)) {
            unsigned count=0;
            while(count<32u &&
                (uintptr_t)&cmd[count]<s_roomPrimaryEnd) {
                const unsigned next=(unsigned)(cmd[count].words.w0>>24);
                if(next!=(uint8_t)G_TRI1 && next!=(uint8_t)G_TRI4)break;
                ++count;
            }
            const bool residentDrawn=geRoomNativeRun(cmd,count);
            if(residentDrawn) {
                if(s_nativeCommandBudget<count-1u) {
                    GENativePlatform_SetError("Native room packet exceeded DL command budget");
                    s_nativeGfxFailed=1;return;
                }
                s_nativeCommandBudget-=count-1u;
                cmd+=count;continue;
            }
            s_roomAttemptedStart=cmd;
            s_roomAttemptedUntil=cmd+count;
        }
#endif
#if defined(GE_XBOX_NATIVE32)
#if GE_XBOX_FAST3D_EQUIVALENT_STATE_SKIP
        const bool geEquivalentStateCommand = (opcode == G_SETTILE ||
            opcode == G_SETTILESIZE || opcode == G_SETCOMBINE);
#else
        const bool geEquivalentStateCommand = false;
#endif
#if GE_XBOX_FAST3D_OPCODE_STATE_MASK
        if ((geCleanDLMask[opcode >> 5] & ((uint32_t)1u << (opcode & 31u))) == 0u &&
            !geEquivalentStateCommand)
            geTriStateInvalidated();
#else
        switch (opcode) {
            case (uint8_t)G_TRI1:
            case (uint8_t)G_TRI4:
            case G_VTX:
            case G_FLOATVTX_EXT:
            case G_MTX:
            case (uint8_t)G_POPMTX:
            case G_DL:
            case (uint8_t)G_ENDDL:
            case G_SPNOOP:
            case G_NOOP:
            case G_COL:
                break;
            default:
                if (!geEquivalentStateCommand)geTriStateInvalidated();
                break;
        }
#endif /* GE_XBOX_FAST3D_OPCODE_STATE_MASK */
#endif /* GE_XBOX_NATIVE32 */
        if(boundedTask) {
            const unsigned slots=(opcode==0x48 || opcode==G_FILLRECT_WIDE_EXT)?2u:
                ((opcode==G_TEXRECT || opcode==G_TEXRECTFLIP ||
                  opcode==G_TEXRECT_WIDE_EXT || opcode==G_IMAGERECT_EXT)?3u:1u);
            if((uintptr_t)cmd>(uintptr_t)s_nativeTaskEnd ||
               (uintptr_t)s_nativeTaskEnd-(uintptr_t)cmd<slots*sizeof(Gfx)) {
                GENativePlatform_SetError("Truncated native graphics command payload");
                s_nativeGfxFailed=1;return;
            }
        }
        // gfx_print_cmd(cmd);
#if GE_XBOX_PHASE0_PROFILE && GE_XBOX_PHASE0E_PROFILE
        const int p0eOpCategory=geP0EOpcodeCategory(opcode);
        geP0ECommandNote(p0eOpCategory);
#endif
        { /* PH0E: scope exits correctly on G_DL/ENDDL continue or error return. */
        GE_P0E_SCOPE(p0eOpCategory);
        switch (opcode) {
#if defined(GE_XBOX_NATIVE32)
            case G_XBOX_MP_VI_CANVAS_EXT: { /* N64 logical VI canvas, submitted by viSetupCurrentPlayerView. */
#if GE_XBOX_MP_VI_CANVAS_FIX
                const uint32_t w = ((uint32_t)cmd->words.w1 >> 16) & 0xffffu;
                const uint32_t h = (uint32_t)cmd->words.w1 & 0xffffu;
                /* Fail conservatively on corrupt custom commands. */
                if ((w == 320u && h == 240u) || (w == 440u && h == 330u)) {
                    const bool canvasChanged = (w != s_xboxLogicalWidth || h != s_xboxLogicalHeight);
                    if (canvasChanged) gfx_flush();
                    s_xboxLogicalWidth = w;
                    s_xboxLogicalHeight = h;
                    if (canvasChanged) rdp.viewport_or_scissor_changed = true;
#if GE_XBOX_MP_PORTRAIT_FULL_VIEWPORT
                    /* The title/menu DL does not run viSetupCurrentPlayerView().
                     * Changing the logical canvas alone left rdp.viewport as
                     * the previous split-screen gameplay viewport (or the
                     * previous 320x240 viewport). 2D rectangles override
                     * that viewport temporarily, but the 3D portrait quads
                     * don't. Reset both rects at the start of EACH 440x330
                     * menu DL, even when its canvas dimensions are unchanged.
                     * Subsequent panel scissors still take effect normally. */
                    if (w == 440u && h == 330u) {
                        struct XYWidthHeight full = {0, (int16_t)h, w, h};
                        gfx_adjust_viewport_or_scissor(&full);
                        struct XYWidthHeight fullScissor = {0, (int16_t)h, w, h};
                        gfx_adjust_viewport_or_scissor(&fullScissor, rsp.aspect_mode != 0);
                        if (memcmp(&rdp.viewport, &full, sizeof(full)) != 0 ||
                            memcmp(&rdp.scissor, &fullScissor, sizeof(fullScissor)) != 0) {
                            if (!canvasChanged) gfx_flush();
                            rdp.viewport = full;
                            rdp.scissor = fullScissor;
                            rdp.viewport_or_scissor_changed = true;
                        }
                    }
#endif
                }
#endif
                break;
            }
#endif
            case 0x48: { /* User-authorized Nintendo-slot logo, game-owned timing. */
                float radians,scale;
                const uint32_t r=(uint32_t)cmd[0].words.w1;
                const uint32_t z=(uint32_t)cmd[1].words.w0;
                memcpy(&radians,&r,4);memcpy(&scale,&z,4);
                gfx_flush();
                geNativeDrawLogo(radians,scale,(float)(cmd[0].words.w0&255u)/255.0f);
                ++cmd;
                break;
            }
                // RSP commands:
#if defined(GE_XBOX_NATIVE32)
            case 0x49: /* Primary room begin, emitted by bg.c only. */
#if GE_XBOX_NATIVE_ROOM_BATCHES
                ++s_roomHWStats[ROOM_STAT_BEGIN];
                s_roomScope=(int)(cmd->words.w0&0xffffu);
                s_roomRevision=0;
                s_roomHasColorOverride=false;
                s_roomPrimaryStart=(uintptr_t)seg_addr(cmd->words.w1);
                s_roomPrimaryEnd=0u;
#endif
                break;
            case 0x4b: /* Current source generation for GPU cache invalidation. */
#if GE_XBOX_NATIVE_ROOM_BATCHES
                s_roomRevision=(unsigned)(cmd->words.w0&0xffffffu);
                if(s_roomPrimaryStart && cmd->words.w1 &&
                   cmd->words.w1<4u*1024u*1024u)
                    s_roomPrimaryEnd=s_roomPrimaryStart+(uintptr_t)cmd->words.w1;
#endif
                break;
            case 0x4a: /* End primary-room scope. */
#if GE_XBOX_NATIVE_ROOM_BATCHES
                s_roomScope=-1;
                s_roomRevision=0;
                s_roomHasColorOverride=false;
                s_roomPrimaryStart=s_roomPrimaryEnd=0u;
#endif
                break;
#endif
            case G_SPNOOP: /* Original Fast3D SP no-op (0x00), one Gfx slot. */
                break;
            case G_NOOP: /* 0xc0. GE's gbi_extension.h also names this slot
                             G_SETTEX (gsSPUseTexture); the game never emits it
                             (finding B1), so treating it as a no-op is safe. */
                break;
            case G_MTX: {
                gfx_sp_matrix(C0(16, 8), (const int32_t*)seg_addr(cmd->words.w1));
                break;
            }
            case (uint8_t)G_POPMTX:
                gfx_sp_pop_matrix(1);
                break;
            case G_MOVEMEM:
                gfx_sp_movemem(C0(16, 8), 0, seg_addr(cmd->words.w1));
                break;
            case (uint8_t)G_MOVEWORD:
                gfx_sp_moveword(C0(0, 8), C0(8, 16), cmd->words.w1);
                break;
            case (uint8_t)G_TEXTURE:
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(0, 8));
                break;
            case G_VTX:
                gfx_sp_vertex(C0(0, 16) / sizeof(Vtx), C0(16, 4), (const Vtx*)seg_addr(cmd->words.w1));
                if (tri_dbg()) d75d_note_vtx(videoGetFrameCount(), C0(0, 16) / sizeof(Vtx), C0(16, 4));
                break;
            case G_DL: {
                if (C0(16, 1) == 0) {
                    // Push return address
                    Gfx* subGFX = (Gfx*)seg_addr(cmd->words.w1);

                    if (subGFX != nullptr) {
                        if(s_nativeDLDepth>=NATIVE_DL_MAX_DEPTH) {
                            GENativePlatform_SetError("Invalid or recursively cyclic display list");
                            s_nativeGfxFailed=1;return;
                        }
                        NativeDLReturn& parent=s_nativeDLReturns[s_nativeDLDepth-1u];
                        parent.command=cmd+1;parent.start=dListStart;parent.bounded=boundedTask;
                        ++s_nativeDLDepth;
                        if(s_nativeDLDepth>s_nativeMaxDLDepth)s_nativeMaxDLDepth=s_nativeDLDepth;
                        cmd=subGFX;dListStart=subGFX;boundedTask=nativeDLInTask(cmd);
                        continue;
                    }
                } else {
                    cmd = (Gfx*)seg_addr(cmd->words.w1);
                    if(!cmd) {
                        GENativePlatform_SetError("Invalid or recursively cyclic display list");
                        s_nativeGfxFailed=1;return;
                    }
                    /* A tail branch keeps the caller's return address, but
                     * its destination may be outside the root task buffer. */
                    boundedTask=nativeDLInTask(cmd);
                    continue;
                }
                break;
            }
            case (uint8_t)G_ENDDL:
                if(!nativeDLPop(cmd,dListStart,boundedTask))return;
                continue;
            case (uint8_t)G_SETGEOMETRYMODE:
                gfx_sp_geometry_mode(0, cmd->words.w1);
                break;
            case (uint8_t)G_CLEARGEOMETRYMODE:
                gfx_sp_geometry_mode(cmd->words.w1, 0);
                break;
            case G_FLOATVTX_EXT: /* D245 */
                gfx_sp_vertex_float(C0(0, 16), C0(16, 8), (const PortFloatVtx*)seg_addr(cmd->words.w1));
                break;
            case 0x46: /* G_HUDSCALE_EXT (D226): w0 low16 = scale*256 (0/256 = off), w1 = ax4<<16 | ay4 */
            {
                const uint32_t sc = C0(0, 16);
                s_hud_scale = (sc == 0 || sc == 256) ? 1.0f : (float)sc / 256.0f;
                s_hud_ax = (int32_t)C1(16, 16);
                s_hud_ay = (int32_t)C1(0, 16);
                break;
            }
            case G_EXTRAGEOMETRYMODE_EXT:
                gfx_sp_extra_geometry_mode(~C0(0, 24), cmd->words.w1);
                break;
            case (uint8_t)G_TRI1:
                gfx_sp_tri1(C1(16, 8) / 10, C1(8, 8) / 10, C1(0, 8) / 10, false);
                break;
            case (uint8_t)G_TRI4:
                gfx_sp_tri4(cmd);
                break;
            case (uint8_t)G_SETOTHERMODE_L:
                gfx_sp_set_other_mode(C0(8, 8), C0(0, 8), cmd->words.w1);
                break;
            case (uint8_t)G_SETOTHERMODE_H:
                gfx_sp_set_other_mode(C0(8, 8) + 32, C0(0, 8), (uint64_t)cmd->words.w1 << 32);
                break;
            case G_COL:
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_NATIVE_ROOM_BATCHES && !GE_XBOX_ROOM_ALLOW_GCOL
                s_roomHasColorOverride=true;
#endif
                gfx_sp_set_vertex_colors(C0(0, 16) / 4, (NormalColor *)seg_addr(cmd->words.w1));
                break;

            // RDP Commands:
            case G_SETTIMG: {
                gfx_dp_set_texture_image(C0(21, 3), C0(19, 2), C0(0, 10), 0, seg_addr(cmd->words.w1));
                break;
            }
            case G_SETTIMG_FB_EXT:
                gfx_flush();
                gfx_rapi->select_texture_fb(cmd->words.w1);
                rdp.textures_changed[0] = false;
                rdp.textures_changed[1] = false;
                break;
            case G_SETGRAYSCALE_EXT:
                rdp.grayscale = cmd->words.w1;
                break;
            case G_LOADBLOCK:
                gfx_dp_load_block(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTILE:
                gfx_dp_load_tile(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETTILE: {
                const bool changed=gfx_dp_set_tile(C0(21, 3), C0(19, 2), C0(9, 9), C0(0, 9), C1(24, 3), C1(20, 4), C1(18, 2), C1(14, 4),
                                                   C1(10, 4), C1(8, 2), C1(4, 4), C1(0, 4));
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_FAST3D_EQUIVALENT_STATE_SKIP
                if (changed)geTriStateInvalidated();
#else
                (void)changed;
#endif
                break;
            }
            case G_SETTILESIZE: {
                const bool changed=gfx_dp_set_tile_size(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_FAST3D_EQUIVALENT_STATE_SKIP
                if (changed)geTriStateInvalidated();
#else
                (void)changed;
#endif
                break;
            }
            case G_LOADTLUT:
                gfx_dp_load_tlut(C1(24, 3), C0(14, 10), C0(2, 10), C1(14, 10), C1(2, 10));
                break;
            case G_SETENVCOLOR:
                gfx_dp_set_env_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETPRIMCOLOR:
                gfx_dp_set_prim_color(C0(8, 8), C0(0, 8), C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFOGCOLOR:
                gfx_dp_set_fog_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFILLCOLOR:
                gfx_dp_set_fill_color(cmd->words.w1);
                break;
            case G_SETINTENSITY_EXT:
                gfx_dp_set_grayscale_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETCOMBINE: {
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_FAST3D_EQUIVALENT_STATE_SKIP
                const uint64_t previousCombine=rdp.combine_mode;
#endif
#ifdef PORT
                /* D172 probe (env-gated, inert): log every SETCOMBINE with the
                 * cycle-type active at that moment. Particle records
                 * (explosion.c g_ExplosionDisplayLists[]) set a 2-cycle
                 * combine but never set cycletype - this tells us what
                 * cycletype fast3d has when they replay. */
                static int ge_d172_b = -1;
                if (ge_d172_b < 0) ge_d172_b = getenv("GE_D172") != NULL;
                if (ge_d172_b) {
                    static uint64_t d172seen[64];
                    static int d172cnt = 0;
                    uint64_t key = ((uint64_t)(uint32_t)cmd->words.w0 << 32) | (uint32_t)cmd->words.w1;
                    bool dup = false;
                    for (int k = 0; k < d172cnt; k++) if (d172seen[k] == key) { dup = true; break; }
                    if (!dup && d172cnt < 64) {
                        d172seen[d172cnt++] = key;
                        uint32_t ct = (rdp.other_mode_h >> G_MDSFT_CYCLETYPE) & 3;
                        sysLogPrintf(LOG_NOTE,
                            "D172: SETCOMBINE w0=%08x w1=%08x  cycletype=%u (%s)",
                            (uint32_t)cmd->words.w0, (uint32_t)cmd->words.w1,
                            ct, ct == 0 ? "1CYC" : ct == 1 ? "2CYC" : ct == 2 ? "COPY" : "FILL");
                    }
                }
#endif
                gfx_dp_set_combine_mode(color_comb(C0(20, 4), C1(28, 4), C0(15, 5), C1(15, 3)),
                                        alpha_comb(C0(12, 3), C1(12, 3), C0(9, 3), C1(9, 3)),
                                        color_comb(C0(5, 4), C1(24, 4), C0(0, 5), C1(6, 3)),
                                        alpha_comb(C1(21, 3), C1(3, 3), C1(18, 3), C1(0, 3)));
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_FAST3D_EQUIVALENT_STATE_SKIP
                if (rdp.combine_mode!=previousCombine)geTriStateInvalidated();
#endif
                break;
            }
            // G_SETPRIMCOLOR, G_CCMUX_PRIMITIVE, G_ACMUX_PRIMITIVE, is used by Goddard
            // G_CCMUX_TEXEL1, LOD_FRACTION is used in Bowser room 1
            case G_SETSUBPIXELOFFSET_EXT: {
                gfx_dp_set_subpixel_offset(C0(0, 16), C1(0, 16));
                break;
            }
            case G_TEXRECT:
            case G_TEXRECTFLIP: {
                int32_t lrx, lry, tile, ulx, uly;
                uint32_t uls, ult, dsdx, dtdy;
                lrx = C0(12, 12);
                lry = C0(0, 12);
                tile = C1(24, 3);
                ulx = C1(12, 12);
                uly = C1(0, 12);
                ++cmd;
                uls = C1(16, 16);
                ult = C1(0, 16);
                ++cmd;
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
                gfx_dp_texture_rectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, opcode == G_TEXRECTFLIP);
                break;
            }
            case G_FILLRECT:
                gfx_dp_fill_rectangle(C1(12, 12), C1(0, 12), C0(12, 12), C0(0, 12));
                break;
            case G_FILLRECT_WIDE_EXT: {
                int32_t lrx, lry, ulx, uly;
                lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                lry = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                uly = (int32_t)(C1(0, 24) << 8) >> 8;
                gfx_dp_fill_rectangle(ulx, uly, lrx, lry);
                break;
            }
            case G_TEXRECT_WIDE_EXT: {
                int32_t lrx, lry, tile, ulx, uly;
                uint32_t uls, ult, dsdx, dtdy;
                bool flip;
                lrx = (int32_t)((C0(0, 24) << 8)) >> 8;
                lry = (int32_t)((C1(0, 24) << 8)) >> 8;
                tile = C1(24, 3);
                flip = C1(27, 1);
                ++cmd;
                ulx = (int32_t)((C0(0, 24) << 8)) >> 8;
                uly = (int32_t)((C1(0, 24) << 8)) >> 8;
                ++cmd;
                uls = C0(16, 16);
                ult = C0(0, 16);
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
                gfx_dp_texture_rectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, flip);
                break;
            }
            case G_IMAGERECT_EXT: {
                int16_t tile, iw, ih;
                int16_t x0, y0, s0, t0;
                int16_t x1, y1, s1, t1;
                tile = C0(0, 3);
                iw = C1(16, 16);
                ih = C1(0, 16);
                ++cmd;
                x0 = C0(16, 16);
                y0 = C0(0, 16);
                s0 = C1(16, 16);
                t0 = C1(0, 16);
                ++cmd;
                x1 = C0(16, 16);
                y1 = C0(0, 16);
                s1 = C1(16, 16);
                t1 = C1(0, 16);
                gfx_dp_image_rectangle(tile, iw, ih, x0, y0, s0, t0, x1, y1, s1, t1);
                break;
            }
            case G_SETSCISSOR:
                gfx_dp_set_scissor(C1(24, 2), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETZIMG:
                gfx_dp_set_z_image(seg_addr(cmd->words.w1));
                break;
            case G_SETCIMG:
                gfx_dp_set_color_image(C0(21, 3), C0(19, 2), C0(0, 11), seg_addr(cmd->words.w1));
                break;
            case G_SETFB_EXT:
                gfx_flush();
                if (cmd->words.w1) {
                    // don't care about noise here
                    gfx_set_framebuffer(cmd->words.w1, 1.f);
                    fbActive = true;
                } else {
                    gfx_reset_framebuffer();
                    fbActive = false;
                }
                break;
            case G_COPYFB_EXT:
                gfx_copy_framebuffer(C0(11, 11), C0(0, 11), (int16_t)C1(16, 16), (int16_t)C1(0, 16), C0(22, 1));
                break;
            case G_RDPSETOTHERMODE:
                gfx_dp_set_other_mode(C0(0, 24), cmd->words.w1);
                break;
            case G_INVALTEXCACHE_EXT:
                if (cmd->words.w1) {
                    gfx_texture_cache_delete((const uint8_t *)seg_addr(cmd->words.w1));
                } else {
                    gfx_texture_cache_clear();
                }
                break;
            case (uint8_t)G_RDPHALF_1:
            case (uint8_t)G_RDPHALF_2:
            case (uint8_t)G_RDPHALF_CONT:
                // on N64 skyRender uses these to render some types of skies and skybox water
                // by issuing low-level ucode commands G_TRI_FILL and G_TRI_SHADE_TXTR
                // the port renders the sky in a different manner
                break;
            case G_RDPFLUSH_EXT:
                gfx_flush();
                break;
            case G_CLEAR_DEPTH_EXT:
                gfx_flush();
                gfx_rapi->clear_framebuffer(false, true);
                break;
            case G_RDPPIPESYNC:
            case G_RDPFULLSYNC:
            case G_RDPLOADSYNC:
            case G_RDPTILESYNC:
                break;
            default: {
                /* Keep the rejected command on the Xbox error screen as
                 * well as in the log. The task must unwind before the fatal
                 * hook draws, so retain the detail rather than stopping here. */
                char message[192];
                snprintf(message, sizeof(message),
                    "Unsupported GBI opcode %02X at %08X\n"
                    "W0=%08X W1=%08X\nDL=%08X ROOT=%08X",
                    (unsigned)opcode, (unsigned)(uintptr_t)cmd,
                    (unsigned)cmd->words.w0, (unsigned)cmd->words.w1,
                    (unsigned)(uintptr_t)dListStart,
                    (unsigned)(uintptr_t)s_nativeTaskBegin);
                sysLogPrintf(LOG_ERROR, "%s", message);
                GENativePlatform_SetError(message);
                s_nativeGfxFailed=1;
                return;
            }
        }
        } /* PH0E: end opcode scope. */
        ++cmd;
    }
}

static void gfx_sp_reset() {
    rsp.modelview_matrix_stack_size = 1;
    rsp.current_num_lights = 2;
    rsp.lights_changed = true;
}

extern "C" void gfx_get_dimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) {
    gfx_wapi->get_dimensions(width, height, posX, posY);
}

extern "C" void gfx_init(const GfxInitSettings *settings) {
    gfx_wapi = settings->wapi;
    gfx_rapi = settings->rapi;
    gfx_wapi->init(&settings->window_settings);
    gfx_rapi->init();
    gfx_rapi->update_framebuffer_parameters(0, settings->window_settings.width, settings->window_settings.height, 1, false, true, true, true);
    gfx_current_dimensions.internal_mul = 1;
    gfx_current_game_window_viewport.width = gfx_current_dimensions.width = settings->window_settings.width;
    gfx_current_game_window_viewport.height = gfx_current_dimensions.height = settings->window_settings.height;
    game_framebuffer = gfx_rapi->create_framebuffer();
    game_framebuffer_msaa_resolved = gfx_rapi->create_framebuffer();

    if (gfx_msaa_level > 1 && !gfx_framebuffers_enabled) {
        sysLogPrintf(LOG_WARNING, "F3D: MSAA set to %d, but framebuffers are not available; disabling", gfx_msaa_level);
        gfx_msaa_level = 1;
    }

    for (int i = 0; i < 16; i++) {
        segmentPointers[i] = 0;
    }

    if (tex_upload_buffer == nullptr) {
        // We cap texture max to 8k, because why would you need more?
        int max_tex_size = gfx_rapi->get_max_texture_size();
        if(max_tex_size<1 || max_tex_size>1024) max_tex_size=1024;
        tex_upload_buffer = (uint8_t*)malloc((size_t)max_tex_size * max_tex_size * 4);
        if(!tex_upload_buffer) sysFatalError("Cannot allocate Fast3D conversion scratch");
    }

    /* D72: N64 boots with RSP memory zeroed — no lookat until gSPLookAt. */
    rsp.lookat_enabled = false;
}

extern "C" void gfx_destroy(void) {
    // TODO: should also destroy rapi and wapi, and any other resources acquired in fast3d

    // Texture cache and loaded textures store references to Resources which need to be unreferenced.
    gfx_texture_cache_release();
    for(unsigned i=0;i<s_c_array_tex_count;++i) free(s_c_array_tex_norms[i].bytes);
    memset(s_c_array_tex_norms,0,sizeof(s_c_array_tex_norms));
    s_c_array_tex_count=0;
    free(tex_upload_buffer);tex_upload_buffer=0;
    color_combiner_count=0;prev_combiner=0;
    memset(color_combiner_buckets,0,sizeof(color_combiner_buckets));
    memset(color_combiner_next,0,sizeof(color_combiner_next));
    memset(framebuffers,0,sizeof(framebuffers));
}

extern "C" struct GfxRenderingAPI* gfx_get_current_rendering_api(void) {
    return gfx_rapi;
}

extern "C" void gfx_start_frame(void) {
    gfx_wapi->handle_events();
    gfx_wapi->get_dimensions(&gfx_current_window_dimensions.width, &gfx_current_window_dimensions.height,
                             &gfx_current_window_position_x, &gfx_current_window_position_y);

    if (gfx_current_window_dimensions.height == 0) {
        // Avoid division by zero
        gfx_current_window_dimensions.height = 1;
    }

    gfx_current_window_dimensions.aspect_ratio = (float)gfx_current_window_dimensions.width / gfx_current_window_dimensions.height;
    if (s_nativeDisplayAspect > 0.0f)
        gfx_current_window_dimensions.aspect_ratio = s_nativeDisplayAspect;

    gfx_current_dimensions = gfx_current_window_dimensions;

    gfx_current_game_window_viewport.x = 0;
    gfx_current_game_window_viewport.y = 0;
    gfx_current_game_window_viewport.width = gfx_current_dimensions.width;
    gfx_current_game_window_viewport.height = gfx_current_dimensions.height;
    g_output_rect = { 0, 0, gfx_current_window_dimensions.width, gfx_current_window_dimensions.height };

    if (g_output_aspect > 0.0f) {
        /* D447: pillar/letterbox to the exact target aspect, centred, even
         * sizes. dims == game viewport size, so different_size stays false and
         * gfx_adjust_viewport_or_scissor's existing offset path places every
         * canvas rect inside the rect. */
        const uint32_t ww = gfx_current_window_dimensions.width;
        const uint32_t wh = gfx_current_window_dimensions.height;
        uint32_t rw = ww, rh = wh;
        const float displayAspect = gfx_current_window_dimensions.aspect_ratio;
        if (displayAspect > g_output_aspect) {
            rw = (uint32_t)ge_gfx_round((float)ww * g_output_aspect / displayAspect);
        } else {
            rh = (uint32_t)ge_gfx_round((float)wh * displayAspect / g_output_aspect);
        }
        rw &= ~1u;
        rh &= ~1u;
        if (rw < 2) rw = 2;
        if (rh < 2) rh = 2;
        if (rw > ww) rw = ww;
        if (rh > wh) rh = wh;
        g_output_rect = { (int16_t)((ww - rw) / 2), (int16_t)((wh - rh) / 2), rw, rh };
        gfx_current_dimensions.width = rw;
        gfx_current_dimensions.height = rh;
        gfx_current_dimensions.aspect_ratio = g_output_aspect;
        gfx_current_game_window_viewport = g_output_rect;
    }

    if (gfx_current_dimensions.height != gfx_prev_dimensions.height) {
        for (unsigned fi=0;fi<8;++fi) {
            NativeFramebufferEntry& fb=framebuffers[fi];
            if(!fb.valid) continue;
            uint32_t width, height, msaa;
            if (fb.second.autoresize) {
                if (fb.second.upscale) {
                    width = fb.second.orig_width;
                    height = fb.second.orig_height;
                    gfx_adjust_width_height_for_scale(width, height);
                } else {
                    // assume this is a fullscreen fb
                    width = gfx_current_window_dimensions.width;
                    height = gfx_current_window_dimensions.height;
                }
                if (width != fb.second.applied_width || height != fb.second.applied_height) {
                    gfx_rapi->update_framebuffer_parameters(fb.first, width, height, 1, true, true, true, true);
                    fb.second.applied_width = width;
                    fb.second.applied_height = height;
                }
            }
        }
    }
    gfx_prev_dimensions = gfx_current_dimensions;

    bool different_size = gfx_current_dimensions.width != gfx_current_game_window_viewport.width ||
                          gfx_current_dimensions.height != gfx_current_game_window_viewport.height;
    if (gfx_framebuffers_enabled && (different_size || gfx_msaa_level > 1)) {
        game_renders_to_framebuffer = true;
        if (different_size) {
            gfx_rapi->update_framebuffer_parameters(game_framebuffer, gfx_current_dimensions.width,
                                                    gfx_current_dimensions.height, gfx_msaa_level, true, true, true,
                                                    true);
        } else {
            // MSAA framebuffer needs to be resolved to an equally sized target when complete, which must therefore
            // match the window size
            gfx_rapi->update_framebuffer_parameters(game_framebuffer, gfx_current_window_dimensions.width,
                                                    gfx_current_window_dimensions.height, gfx_msaa_level, false, true,
                                                    true, true);
        }
        if (gfx_msaa_level > 1 && different_size) {
            gfx_rapi->update_framebuffer_parameters(game_framebuffer_msaa_resolved, gfx_current_dimensions.width,
                                                    gfx_current_dimensions.height, 1, false, false, false, false);
        }
    } else {
        game_renders_to_framebuffer = false;
    }

    fbActive = 0;

    // update aspect scale and offset
    gfx_update_aspect_mode();
}

uint32_t num_dls = 0;

/* F10 port-layer options overlay (port/src/optionsoverlay.c). Returns a
 * self-contained 2D display list to draw on top of the game's frame, or NULL
 * when the overlay is closed -- in which case nothing is appended and the
 * frame is byte-identical to before (golden dumps unaffected). */
/* Desktop options overlay is not part of the Xbox game renderer. */
static Gfx* optionsOverlayEmit(void) { return 0; }

static uint64_t gfx_perf_now_ns(void) {
    return sysGetMicroseconds()*1000u;
}
static int gfx_perfstat_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("GE_PERFSTAT") != NULL;
    return on;
}
static double s_perf_dl = 0, s_perf_run = 0, s_perf_present = 0, s_perf_interval = 0;
static double s_perf_pre = 0, s_perf_post = 0, s_perf_swap = 0;
static uint64_t s_perf_frames = 0, s_perf_last_start = 0;
static double s_perf_maxiv = 0, s_perf_maxiv_dl = 0, s_perf_maxiv_run = 0;
static uint64_t s_perf_maxiv_frame = 0, s_perf_over20 = 0, s_perf_over33 = 0;

extern "C" void gfx_run(Gfx* commands) {
    s_hud_scale = 1.0f;   /* D226: never carry a HUD scale across frames */
    const uint64_t perf_t0 = gfx_perfstat_on() ? gfx_perf_now_ns() : 0;
    ++num_dls;
    gfx_sp_reset();
    geTriStateInvalidated();

    // puts("New frame");

    if (!gfx_wapi->start_frame()) {
        dropped_frame = true;
        return;
    }
    if (g_aspect_settle > 0) {   /* D509: hold the old image, see gfx_set_output_aspect */
        --g_aspect_settle;
        dropped_frame = true;
        return;
    }
    dropped_frame = false;

    if (s_shader_warm_pending) {
        gfx_shader_prewarm();   /* D480 */
    }

    gfx_rapi->update_framebuffer_parameters(0, gfx_current_window_dimensions.width,
                                            gfx_current_window_dimensions.height, 1, false, true, true,
                                            !game_renders_to_framebuffer);
    gfx_rapi->start_frame();
    gfx_rapi->start_draw_to_framebuffer(game_renders_to_framebuffer ? game_framebuffer : 0,
                                        (float)gfx_current_dimensions.height / GE_LOGICAL_HEIGHT);
    gfx_rapi->clear_framebuffer(true, false);
    rdp.viewport_or_scissor_changed = true;
    rendering_state.viewport = {};
    rendering_state.scissor = {};
    const uint64_t perf_tpre = perf_t0 ? gfx_perf_now_ns() : 0;
    const unsigned long long p0DLStart=geP0BStart();
    { /* Root covers exactly the PH0B DL region; nested exclusive buckets. */
    GE_P0E_SCOPE(GE_P0E_DISPATCH);
    gfx_run_dl(commands);
    {
        Gfx* overlay = optionsOverlayEmit();
        if (overlay != nullptr) {
            g_overlay_window_space = true;   /* D510 */
            gfx_run_dl(overlay);
            g_overlay_window_space = false;
            s_overlay_scale = 1.0f;
        }
    }
    gfx_flush();
    } /* PH0E root */
    geP0BEnd(GE_P0B_DL,p0DLStart);
    const uint64_t perf_t1 = perf_t0 ? gfx_perf_now_ns() : 0;
    gfxFramebuffer = 0;

    if (game_renders_to_framebuffer) {
        gfx_rapi->start_draw_to_framebuffer(0, 1);
        gfx_rapi->clear_framebuffer(true, true);

        if (gfx_msaa_level > 1) {
            bool different_size = gfx_current_dimensions.width != gfx_current_game_window_viewport.width ||
                                  gfx_current_dimensions.height != gfx_current_game_window_viewport.height;

            if (different_size) {
                gfx_rapi->resolve_msaa_color_buffer(game_framebuffer_msaa_resolved, game_framebuffer);
                gfxFramebuffer = (uintptr_t)gfx_rapi->get_framebuffer_texture_id(game_framebuffer_msaa_resolved);
            } else {
                gfx_rapi->resolve_msaa_color_buffer(0, game_framebuffer);
            }
        } else {
            gfxFramebuffer = (uintptr_t)gfx_rapi->get_framebuffer_texture_id(game_framebuffer);
        }
    }

    gfx_rapi->end_frame();
    const uint64_t perf_tpost = perf_t0 ? gfx_perf_now_ns() : 0;
    gfx_wapi->swap_buffers_begin();
    if (perf_t0) {
        s_perf_pre += (double)(perf_tpre - perf_t0) / 1.0e6;
        s_perf_post += (double)(perf_tpost - perf_t1) / 1.0e6;
        s_perf_swap += (double)(gfx_perf_now_ns() - perf_tpost) / 1.0e6;
        const double f = 1.0e6;
        const uint64_t t2 = gfx_perf_now_ns();
        s_perf_dl += (double)(perf_t1 - perf_tpre) / f;
        s_perf_run += (double)(t2 - perf_t0) / f;
        if (s_perf_last_start) {
            const double iv = (double)(perf_t0 - s_perf_last_start) / f;
            s_perf_interval += iv;
            /* Spike stats (hitches hide in 300-frame averages): the worst
             * frame's interval and its own dl/run, plus counts over 20/33 ms. */
            if (iv > s_perf_maxiv) {
                s_perf_maxiv = iv;
                s_perf_maxiv_dl = (double)(perf_t1 - perf_tpre) / f;
                s_perf_maxiv_run = (double)(t2 - perf_t0) / f;
                s_perf_maxiv_frame = s_perf_frames + 1;
            }
            if (iv > 20.0) s_perf_over20++;
            if (iv > 33.4) s_perf_over33++;
        }
        s_perf_last_start = perf_t0;
    }
}

extern "C" void gfx_end_frame(void) {
    const uint64_t perf_t0 = gfx_perfstat_on() ? gfx_perf_now_ns() : 0;
    if (!dropped_frame) {
        gfx_rapi->finish_render();
        gfx_wapi->swap_buffers_end();
    }
    if (perf_t0) {
        const double f = 1.0e6;
        s_perf_present += (double)(gfx_perf_now_ns() - perf_t0) / f;
        if (++s_perf_frames % 300 == 0) {
            const double n = 300.0;
            fprintf(stderr, "PERFSTAT pre=%.2f post=%.2f swap/pace=%.2f | ", s_perf_pre / n, s_perf_post / n, s_perf_swap / n);
            s_perf_pre = s_perf_post = s_perf_swap = 0;
            fprintf(stderr, "frames=%llu dl=%.2fms run=%.2fms present=%.2fms interval=%.2fms (%.1f fps) tris=%.0f batches=%.0f\n",
                    (unsigned long long)s_perf_frames, s_perf_dl / n, s_perf_run / n, s_perf_present / n,
                    s_perf_interval / n, s_perf_interval > 0 ? 1000.0 * n / s_perf_interval : 0.0,
                    (double)s_perf_tris / n, (double)s_perf_batches / n);
            fprintf(stderr, "PERFSTAT spikes: max=%.2fms at frame %llu (dl=%.2f run=%.2f) >20ms=%llu >33ms=%llu\n",
                    s_perf_maxiv, (unsigned long long)s_perf_maxiv_frame, s_perf_maxiv_dl, s_perf_maxiv_run,
                    (unsigned long long)s_perf_over20, (unsigned long long)s_perf_over33);
            s_perf_maxiv = s_perf_maxiv_dl = s_perf_maxiv_run = 0;
            s_perf_maxiv_frame = s_perf_over20 = s_perf_over33 = 0;
            s_perf_dl = s_perf_run = s_perf_present = s_perf_interval = 0;
            s_perf_tris = s_perf_batches = 0;
        }
    }
}

extern "C" void gfx_set_target_fps(int fps) {
    gfx_wapi->set_target_fps(fps);
}

extern "C" void reset_texture_state() {
    gfx_texture_cache_clear();
    if (rendering_state.shader_program) {
        gfx_rapi->unload_shader(rendering_state.shader_program);
        rendering_state.shader_program = nullptr;
    }
    gfx_rapi->clear_shaders();
    s_shader_warm_pending = true;   /* D480: re-warm on the next frame */
    color_combiner_count=0;
    prev_combiner=0;
    memset(color_combiner_buckets,0,sizeof(color_combiner_buckets));
    memset(color_combiner_next,0,sizeof(color_combiner_next));
}

extern "C" void gfx_set_texture_filter(enum FilteringMode mode) {
    reset_texture_state();
    gfx_rapi->set_texture_filter(mode);
}

extern "C" void gfx_set_mipmap_filter(enum MipmapFilteringMode mode) {
    reset_texture_state();
    gfx_rapi->set_mipmap_filter(mode);
}

extern "C" void gfx_set_fix_mip_textures(int on) { g_fix_mip_textures = !!on; }

extern "C" void gfx_set_detail_base_tile(int on) { g_detail_base_tile = !!on; }

/* D212: expose the (already-implemented) rendering-API anisotropy hook to the
 * port layer. Clamp to [1, GL max] so a stale ini value can't feed an invalid
 * GL_TEXTURE_MAX_ANISOTROPY. 1 = isotropic (driver default). */
extern "C" void gfx_set_anisotropy_level(int level) {
    reset_texture_state();
    int max = gfx_rapi->get_max_anisotropy_level ? gfx_rapi->get_max_anisotropy_level() : 1;
    if (max < 1) max = 1;
    if (level < 1) level = 1;
    if (level > max) level = max;
    if (gfx_rapi->set_anisotropy_level) {
        gfx_rapi->set_anisotropy_level(level);
    }
}
extern "C" void gfx_set_wrap_fix(int on) {
    const char* e = getenv("GE_WRAPFIX"); /* RC3 test override */
    g_wrap_fix = e ? (atoi(e) != 0) : !!on;
}

extern "C" int gfx_create_framebuffer(uint32_t width, uint32_t height, int upscale, int autoresize) {
    int fb = gfx_rapi->create_framebuffer();
    gfx_resize_framebuffer(fb, width, height, upscale, autoresize);
    return fb;
}

extern "C" void gfx_resize_framebuffer(int fb, uint32_t width, uint32_t height, int upscale, int autoresize) {
    uint32_t orig_width, orig_height;

    if (width && height) {
        // user-specified size
        orig_width = width;
        orig_height = height;
        if (upscale) {
            gfx_adjust_width_height_for_scale(width, height);
        }
        gfx_rapi->update_framebuffer_parameters(fb, width, height, 1, true, true, true, true);
    } else {
        // same size as main fb
        orig_width = width = gfx_current_dimensions.width;
        orig_height = height = gfx_current_dimensions.height;
        upscale = false;
        autoresize = true;
        gfx_rapi->update_framebuffer_parameters(fb, width, height, 1, true, true, true, true);
    }

    if(fb<0 || fb>=8) sysFatalError("Xbox framebuffer index out of range");
    framebuffers[fb].first=fb;
    framebuffers[fb].second = { orig_width, orig_height, width, height, (bool)upscale, (bool)autoresize };
    framebuffers[fb].valid=true;
}

extern "C" void gfx_set_framebuffer(int fb, float noise_scale) {
    gfx_rapi->start_draw_to_framebuffer(fb, noise_scale);
    gfx_rapi->clear_framebuffer(true, true);

}

extern "C" void gfx_copy_framebuffer(int fb_dst, int fb_src, int left, int top, int use_back) {
    const bool is_main_fb = (fb_src == 0);

    if (is_main_fb) {
        if (left > 0 && top > 0) {
            // upscale the position
            left = left * gfx_current_dimensions.width / gfx_current_native_viewport.width;
            top = top * gfx_current_dimensions.height / gfx_current_native_viewport.height;
            // D447: offset into the output rect (0,0 when the rect fills the window)
            left += gfx_current_game_window_viewport.x;
            top += gfx_current_game_window_viewport.y;
            // flip Y
            top = gfx_current_window_dimensions.height - top - 1;
        }
        if (use_back && gfx_msaa_level > 1) {
            // read from the framebuffer we've been rendering to
            fb_src = game_framebuffer;
        }
    }

    gfx_rapi->copy_framebuffer(fb_dst, fb_src, left, top, is_main_fb, (bool)use_back);
}

extern "C" void gfx_reset_framebuffer(void) {
    gfx_rapi->start_draw_to_framebuffer(0, (float)gfx_current_dimensions.height / GE_LOGICAL_HEIGHT);

}


/* Narrow C boundary keeps PR/gbi types out of Xbox/XTL translation units. */
extern "C" int geNativeGfxInit(GfxWindowManagerAPI *window,GfxRenderingAPI *renderer,
                               unsigned width,unsigned height) {
    if(!window || !renderer || !width || !height)return 0;
    GfxInitSettings settings;
    memset(&settings,0,sizeof(settings));
    settings.wapi=window;settings.rapi=renderer;
    settings.window_settings.title="007xb";
    settings.window_settings.width=width;settings.window_settings.height=height;
    settings.window_settings.fullscreen=true;
    gfx_framebuffers_enabled=false;
    gfx_detail_textures_enabled=false;
    gfx_msaa_level=1;
    gfx_rdp_affine=1;
    gfx_fog_vertex=1;
    geNativeGfxResolution(640,480);
    gfx_init(&settings);
    /* Retain the game's logical canvas; actual display aspect is supplied
     * separately so 720p and anamorphic SD use the same HUD/camera rules. */
    gfx_set_safe_area_crop(0);
    gfx_set_output_aspect(0.0f);
    return tex_upload_buffer!=0;
}
#if defined(GE_XBOX_NATIVE32)
extern "C" int getPlayerCount(void);
#ifndef GE_XBOX_MP_SPLIT_LAYOUT_FIX
#define GE_XBOX_MP_SPLIT_LAYOUT_FIX 1
#endif
#endif
extern "C" int geNativeGfxFrame(void *commands,unsigned bytes) {
    s_nativeMaxDLDepth=0;
    if(!commands || !bytes || (bytes&7u) || bytes>8u*1024u*1024u ||
       (uintptr_t)commands>UINTPTR_MAX-bytes) {
        GENativePlatform_SetError("Invalid native task display-list extent");return 0;
    }
    s_nativeCommandBudget=1000000u;s_nativeDLDepth=0;s_nativeGfxFailed=0;
    s_nativeTaskBegin=(Gfx *)commands;
    s_nativeTaskEnd=(Gfx *)((uint8_t *)commands+bytes);
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_MP_VI_CANVAS_FIX
    s_xboxLogicalWidth = SCREEN_WIDTH;
    s_xboxLogicalHeight = SCREEN_HEIGHT;
#endif
#if defined(GE_XBOX_NATIVE32) && GE_XBOX_MP_SPLIT_LAYOUT_FIX
    /* Port's split-screen flag existed but was never wired in the Xbox
     * renderer. Do not apply the one-player edge/crop rule to MP scissors. */
    gfx_set_split_screen(getPlayerCount() >= 2);
#endif
    gfx_start_frame();
    gfx_run((Gfx *)commands);
    gfx_end_frame();
    return !s_nativeGfxFailed && !dropped_frame && !GENativePlatform_HasFatalError();
}
extern "C" void geNativeGfxResolution(int w,int h) {
    if(w<1 || h<1)return;
    gfx_current_native_viewport.x=gfx_current_native_viewport.y=0;
    gfx_current_native_viewport.width=(unsigned)w;
    gfx_current_native_viewport.height=(unsigned)h;
    gfx_current_native_aspect=(float)w/(float)h;
}
extern "C" void geNativeGfxDisplayAspect(float aspect) {
    s_nativeDisplayAspect=aspect>=0.5f && aspect<=4.0f?aspect:0.0f;
}
extern "C" void geNativeGfxDestroy(void) { gfx_destroy(); }
extern "C" void geNativeGfxTextureReset(void) { gfx_texture_cache_release(); }
extern "C" unsigned geNativeGfxMaxDLDepth(void) { return s_nativeMaxDLDepth; }
