/*
 * 007xb/src engine platform adapter -> original Xbox Direct3D8 backend.
 *
 * This is a backend for the ORIGINAL port/fast3d/gfx_pc.cpp interpreter,
 * NOT a second model interpreter or an intro animation reconstruction.
 * The engine selects gfx_xbox_d3d8_api instead of gfx_opengl_api.
 *
 * Compiler: C++98 (classic RXDK); no STL, exceptions, RTTI or UCRT calls.
 * Assumes the RXDK host has already created its Xbox D3D8 device.
 *
 * The original one/two-cycle colour equation is compiled to NV2A register
 * combiners. Unsupported desktop extensions stop with a precise diagnostic.
 * Hardware validation remains required for RDP precision/overflow behaviour.
 */
#include <xtl.h>
#include "xb_native_mesh.h"
/* Team Resurgent MK64-X uses the native Xbox swizzler rather than scattering
 * pixels through a scalar Morton loop.  Old-RXDK A/B switch below. */
#ifndef GE_NV2A_FAST_SWIZZLE
#define GE_NV2A_FAST_SWIZZLE 1
#endif
#if GE_NV2A_FAST_SWIZZLE
#include <xgraphics.h>
#endif
/* Retail/playtest builds do not sample a high-resolution clock on each
 * Fast3D draw, texture upload, and GPU batch flush. The phase counters remain
 * available for explicit profile builds (GE_NV2A_PHASE_PROFILE=1). */
#ifndef GE_NV2A_PHASE_PROFILE
#define GE_NV2A_PHASE_PROFILE 0
#endif
#include <float.h>
#include <string.h>
#include <stdlib.h>
#include "xb_ge_fast3d.h"
#include "gfx_cc.h"
#include "ge_native_platform.h"
#include "ge_phase0_profile.h"
#include "ge_nv2a_combiner.h"

/* NV2A scissor removes redundant CPU polygon clipping on screen/viewport
 * boundaries. Disable for an old-RXDK A/B test or if a title uses unusual
 * scissor semantics. Padded tile addressing retains its software clipper. */
#ifndef GE_NV2A_HW_SCISSOR
#define GE_NV2A_HW_SCISSOR 1
#endif
/* Fast3D alternates many materials. Keep four recent native combiner handles
 * per parent program instead of repeatedly scanning 256 device shaders. */
#ifndef GE_NV2A_MATERIAL_SHADER_CACHE
#define GE_NV2A_MATERIAL_SHADER_CACHE 1
#endif
/* A stable input map is unchanged for every triangle in an api_draw() call.
 * Look up its NV2A shader once per call instead of once per triangle. The
 * native shader cache cannot evict this entry mid-call because no other
 * shader map is requested along this path. Disable for A/B with 0. */
#ifndef GE_NV2A_STABLE_DRAW_SHADER_REUSE
#define GE_NV2A_STABLE_DRAW_SHADER_REUSE 1
#endif
/* Fast3D often uses only V0/V1 interpolators, with no C0/C1 values
 * derived from a triangle. The compiler already records exactly which
 * register-combiner constant slots read source-vertex data. Classify each
 * newly created native shader once; zero-input materials reuse a literal
 * zero register block instead of packing it for every triangle.
 * No shader, vertex, draw order, or driver-state change. A/B: set 0. */
#ifndef GE_NV2A_SKIP_EMPTY_TRI_CONSTANTS
#define GE_NV2A_SKIP_EMPTY_TRI_CONSTANTS 1
#endif
/* For non-fog combiners the source decoder leaves every fog component
 * at zero. The 0xFF000000 result of packColor(0,0,0,1) is invariant,
 * so skip repeated float clamps/rounding on ordinary opaque materials.
 * Fog-enabled materials retain their original per-triangle RGB check. */
#ifndef GE_NV2A_STATIC_NOFOG_COLOR
#define GE_NV2A_STATIC_NOFOG_COLOR 1
#endif
/* Some original N64 combiners reference more independent per-vertex alpha
 * values than Xbox's two color alpha channels can interpolate (fog uses
 * one).  Previously this was fatal. On only those otherwise unrenderable
 * triangles, approximate the excess varying alpha inputs with the first
 * vertex's value via the already existing per-triangle C0/C1 constants.
 * Regular materials retain bit-for-bit identical interpolation and commands.
 * Define 0 for the old fail-fast behavior when auditing fidelity. */
#ifndef GE_NV2A_ALPHA_VARYING_FALLBACK
#define GE_NV2A_ALPHA_VARYING_FALLBACK 1
#endif
/* Conservative offscreen rejection moves the EXISTING four-plane test
 * ahead of pixel shader / per-triangle constant work. The test still runs
 * after source vertex validation and decoding; intersecting triangles and
 * all software/hardware clipping paths remain unchanged. Set 0 to A/B
 * against the real-hardware validated original ordering. */
#ifndef GE_NV2A_EARLY_OFFSCREEN_REJECT
#define GE_NV2A_EARLY_OFFSCREEN_REJECT 1
#endif
/* Game-wide lean Fast3D processing: the RSP already packed the relevant
 * shader inputs, so do not clear or interpolate seven unused RGBA channels
 * for every emitted N64 triangle. Keep old path for A/B verification. */
#ifndef GE_NV2A_GAMEPLAY_LEAN_VERTEX
#define GE_NV2A_GAMEPLAY_LEAN_VERTEX 1
#endif
/* When a draw has no varying tile-clamp bounds, screen projection alone
 * can prove a triangle invisible. Project xyz/w first and reject it before
 * decoding texture coordinates, fog, and combiner colors. Keep the original
 * late check for clamped materials to preserve every data validation path.
 * Visible triangles reuse the exact projected x/y/rhw values; the GPU stream
 * and source-data interpretation are unchanged. 0 restores legacy ordering. */
#ifndef GE_NV2A_POSITION_FIRST_CULL
#define GE_NV2A_POSITION_FIRST_CULL 1
#endif
/* Phase 0F: A/B cache for identical packed source vertices WITHIN ONE
 * api_drawImpl call. Never crosses material, viewport, sampler or frame
 * boundaries. Disable with GE_NV2A_SOURCE_VERTEX_DECODE_CACHE=0.
 * Per-vertex tile clamps keep their original validation path. */
#ifndef GE_NV2A_SOURCE_VERTEX_DECODE_CACHE
#define GE_NV2A_SOURCE_VERTEX_DECODE_CACHE 1
#endif
/* On an unpadded, CLAMP-addressed NV2A texture, the hardware sampler returns
 * the same edge texel as N64's full-image per-pixel clamp. A smaller tile
 * window, NPOT storage or repeat/mirror retains the original CPU subdivision. */
#ifndef GE_NV2A_HW_TEXCLAMP
#define GE_NV2A_HW_TEXCLAMP 1
#endif

/* Old RXDK forwards texture addressing directly to NV2A. Linear/rectangle
 * textures cannot use repeat/mirror. CPU images therefore use swizzled,
 * power-of-two A8R8G8B8 storage and normalized texture coordinates. */

#define GE_MAX_SHADER 512
#define GE_MAX_NATIVE_SHADER 256
#if defined(GE_XBOX_NATIVE32)
/* Runway has dozens of texture uploads per frame after warm-up. The 512-slot
 * front/back cache can churn even when its 12 MiB GPU byte budget is not
 * exhausted. Raise ID capacity WITHOUT raising that byte budget; old RXDK
 * test can A/B with GE_NV2A_TEXTURE_CACHE_SLOTS=512. Both sides MUST match. */
#ifndef GE_NV2A_TEXTURE_CACHE_SLOTS
#define GE_NV2A_TEXTURE_CACHE_SLOTS 1024
#endif
#if GE_NV2A_TEXTURE_CACHE_SLOTS != 512 && GE_NV2A_TEXTURE_CACHE_SLOTS != 1024
#error GE_NV2A_TEXTURE_CACHE_SLOTS must be 512 or 1024 (power-of-two hash table)
#endif
#define GE_MAX_TEX GE_NV2A_TEXTURE_CACHE_SLOTS
#else
#define GE_MAX_TEX 256
#endif
#define GE_MAX_VERTICES 6144
/* Dynamic streaming VBs replace per-batch immediate user-pointer submissions.
 * A compile-time A/B switch leaves the verified UP path available. */
#ifndef GE_NV2A_STREAM_VB
#define GE_NV2A_STREAM_VB 1
#endif
/* Three frame-owned regions avoid discarding/reusing in-flight GPU geometry.
 * Each has 8K vertices; all regions together fit within the old two-buffer
 * memory budget. Never overwrite a full frame region: spill to the UP path. */
#define GE_STREAM_VERTICES 8192u
#define GE_STREAM_FRAME_COUNT 3u
/* Small one-material draws are faster through UP than lock/copy/unlock/draw
 * on many old Xbox D3D8 devices. A/B by setting this to 0. */
#ifndef GE_NV2A_MIN_VB_VERTICES
#define GE_NV2A_MIN_VB_VERTICES 96u
#endif
/* 440x1 I8 gunbarrel background, 299 rows and duplicated vertical border
 * texels. We capture the uncached rows during the first frame and create one
 * resident swizzled page at a frame boundary (never while drawing them). */
#ifndef GE_NV2A_STRIP_ATLAS
#define GE_NV2A_STRIP_ATLAS 1
#endif
#define GE_STRIP_WIDTH 440u
#define GE_STRIP_ATLAS_W 512u
#define GE_STRIP_ATLAS_H 1024u
#define GE_STRIP_ROWS_REQUIRED 299u
#define GE_STRIP_SLOT_PITCH 3u
#define GE_STRIP_MAX_SLOTS (GE_STRIP_ATLAS_H/GE_STRIP_SLOT_PITCH)
#define GE_MAX_TEX_DIM 1024u
/* Budget excludes the 4 MiB fast3d conversion scratch and GPU backbuffer. */
#define GE_TEXTURE_BUDGET (12u*1024u*1024u)
#define GE_MAX_FRAMEBUFFERS 8
#define GE_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX2)
/* One rollback switch for the complete CPU->NV2A gameplay offload. */
#ifndef GE_XBOX_NATIVE_ROOM_BATCHES
#define GE_XBOX_NATIVE_ROOM_BATCHES 1
#endif
#define GE_NV2A_ROOM_NATIVE_PACKETS GE_XBOX_NATIVE_ROOM_BATCHES
#define GE_ROOM_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX2)
#define GE_ROOM_PACKET_LIMIT GE_XBOX_ROOM_PACKET_RECORDS
/* Halo's native Xbox renderer uses a separate static vertex and index
 * buffer path. Index identical room vertices once at packet upload; never
 * rehash or reassemble during warm rendering. A/B: 0 = original 6B VBs. */
#ifndef GE_NV2A_ROOM_INDEXED_PACKETS
#define GE_NV2A_ROOM_INDEXED_PACKETS 0
#endif
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
#ifndef GE_XBOX_NATIVE_ROOM_SHORT_RUNS
#define GE_XBOX_NATIVE_ROOM_SHORT_RUNS 0
#endif
/* Phase 6D: bounded *resident* geometry, not merely a live allocation total.
 * 6C evicted live VBs/IBs and allocated replacements during rendering; on
 * NV2A this can force a driver/GPU wait and create visible stalls.
 * Use a smaller retail-safe budget and do not evict until detach/reinit.
 * Once full, all new packets take the unmodified Fast3D software path. */
#ifndef GE_XBOX_ROOM_GPU_BUDGET_BYTES
#define GE_XBOX_ROOM_GPU_BUDGET_BYTES (1024u*1024u)
#endif
#if GE_XBOX_ROOM_GPU_BUDGET_BYTES < 65536u || GE_XBOX_ROOM_GPU_BUDGET_BYTES > 2097152u
#error GE_XBOX_ROOM_GPU_BUDGET_BYTES must be between 64 KiB and 2 MiB
#endif
#define GE_ROOM_VERTEX_BYTES GE_XBOX_ROOM_GPU_BUDGET_BYTES
#ifndef GE_XBOX_ROOM_NO_LIVE_EVICTION
#define GE_XBOX_ROOM_NO_LIVE_EVICTION 1
#endif
/* Cold GPU Create/Lock/Unlock is costly even if only one packet is built per
 * frame. Throttle across frames, not just within one graphics task. */
#ifndef GE_XBOX_ROOM_UPLOAD_FRAME_INTERVAL
#define GE_XBOX_ROOM_UPLOAD_FRAME_INTERVAL 8u
#endif
/* Phase 6E: reject cold packet work before assembling 1024 vertices when
 * the upload throttle, fixed resident budget or slot table cannot admit it.
 * Warm packet queries (command_address != 0) are unaffected. */
#ifndef GE_XBOX_ROOM_COLD_PREFLIGHT
#define GE_XBOX_ROOM_COLD_PREFLIGHT 1
#endif
#if GE_XBOX_ROOM_UPLOAD_FRAME_INTERVAL < 1u || GE_XBOX_ROOM_UPLOAD_FRAME_INTERVAL > 120u
#error Invalid room upload frame interval
#endif
#define GE_NOISE_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX3 | D3DFVF_TEXCOORDSIZE4(2))
#define GE_NOISE_SIZE 128u
#define GE_NOISE_BYTES (GE_NOISE_SIZE*GE_NOISE_SIZE*4u)

/* Swizzled Xbox textures use normalized texture coordinates. */
struct GEVertex {
    float x,y,z,rhw;
    DWORD color,specular;
    float u0,v0,u1,v1;
};
/* Only noisy materials carry screen-projected texture coordinates. Keep the
 * existing 40-byte vertex stream for ordinary game rendering. */
struct GERoomVertex {
    float x,y,z;
    DWORD color,specular;
    float u0,v0,u1,v1;
};
typedef char GERoomVertexMustBe36Bytes[(sizeof(GERoomVertex)==36)?1:-1];
struct GENoiseVertex {
    GEVertex vertex;
    float u2,v2,z2,q2;
};
union GEVertexBuffer {
    GEVertex regular[GE_MAX_VERTICES];
    GENoiseVertex noisy[GE_MAX_VERTICES];
};
typedef char GEVertexMustBe40Bytes[(sizeof(GEVertex)==40)?1:-1];
typedef char GENoiseVertexMustBe56Bytes[(sizeof(GENoiseVertex)==56)?1:-1];

struct GENativeShader;
struct ShaderProgram {
    uint64_t id0;
    unsigned id1;
    unsigned char inputs;
    unsigned char useTex[2];
    unsigned char clamps[2][2];
    unsigned char alpha, fog, grayscale;
    CCFeatures features;
    /* When <=2 varying RGB and <=1 fog-alpha inputs are required, the
     * input map is invariant across all triangles using this combiner. */
    GENV2AInputMap stableMap;
    unsigned char stableMapChecked,stableMapValid;
#if GE_NV2A_MATERIAL_SHADER_CACHE
    GENativeShader* nativeRecent[4];
    unsigned nativeRecentNext;
#endif
};
struct GENativeShader {
    ShaderProgram *parent;
    unsigned key;
    DWORD handle;
    unsigned used;
    uint64_t lastUse;
    GENV2AInputMap map;
    GENV2AProgram program;
    unsigned char hasTriangleConstants;
};
struct GESourceVertex {
    float x,y,z,rhw;
    float uv[2][2];
    float input[7][4];
    float fog[4];
};
#if GE_NV2A_SOURCE_VERTEX_DECODE_CACHE
/* 64 direct-mapped entries, renderer-thread-owned, no heap or stack spike.
 * Store the packed input BY VALUE so a reused or mutated input pointer cannot
 * incorrectly return stale geometry. Exact comparison defeats hash collisions. */
#define GE_VERTEX_DECODE_SLOTS 64u
struct GEVertexDecodeEntry {
    unsigned epoch;
    float raw[48]; /* max no-clamp Fast3D layout: 44 floats */
    GESourceVertex decoded;
};
static GEVertexDecodeEntry s_vertexDecodeEntries[GE_VERTEX_DECODE_SLOTS];
static unsigned s_vertexDecodeEpoch=0;
static unsigned geVertexDecodeSlot(const float *v) {
    uint32_t bits[4];
    memcpy(bits,v,sizeof(bits)); /* float aliasing and NaN bit patterns preserved */
    uint32_t mix=(bits[0]*0x9e3779b1u) ^ (bits[1]*0x85ebca6bu) ^
                 (bits[2]*0xc2b2ae35u) ^ (bits[3]*0x27d4eb2du);
    mix^=mix>>16;mix*=0x7feb352du;mix^=mix>>15;
    return (unsigned)(mix & (GE_VERTEX_DECODE_SLOTS-1u));
}
#endif
struct GEPolygon { GESourceVertex vertex[16]; unsigned count; };
struct GESplitScratch { GEPolygon below,above; };
struct GEAddressScratch {
    GEPolygon remaining,piece,above,edgeLow,edgeMiddle,edgeHigh;
};
struct GEFramebuffer {
    IDirect3DTexture8* texture;
    IDirect3DSurface8* surface;
    IDirect3DSurface8* depth;
    unsigned width,height;
    unsigned initialized;
};
struct GETexture {
    IDirect3DTexture8* tex;
    unsigned width,height;
    unsigned storageWidth,storageHeight;
    unsigned live;
    unsigned stripSlot; /* 0=legacy; otherwise 1-based atlas row index. */
};

/* Same graphics worker owns creation, draw and retirement of room resources. */
struct GERoomPacket {
    XBNativeMesh mesh;
    const void *source;
    unsigned room,revision,signature,vertices;
    unsigned shaderKey, texture0, samplerU,samplerV;
    unsigned cullMode,textureUnits;
    unsigned lastUse;
};
static GERoomPacket s_roomPackets[GE_ROOM_PACKET_LIMIT];
static unsigned s_roomPacketBytes=0,s_roomPacketClock=0;
static unsigned s_roomPacketBuildsFrame=0;
static unsigned s_roomPrepareFrame=0;
static bool s_roomGPUAllocationFailed=false;
static unsigned s_roomGPUFrames=0,s_roomGPUTriangles=0;
static unsigned s_roomGPUFrameTris=0;
static void clearRoomPackets() {
    for(unsigned i=0;i<GE_ROOM_PACKET_LIMIT;++i) {
        s_roomPackets[i].mesh.Reset();
        s_roomPackets[i].source=0;
        s_roomPackets[i].vertices=0;
        s_roomPackets[i].lastUse=0;
    }
    s_roomPacketBytes=0;s_roomPacketClock=0;s_roomPacketBuildsFrame=0;
    s_roomPrepareFrame=0;s_roomGPUAllocationFailed=false;
    s_roomGPUFrames=s_roomGPUTriangles=0;s_roomGPUFrameTris=0;
}
static IDirect3DDevice8* s_device = 0;
static unsigned s_width = 640, s_height = 480;
static ShaderProgram s_shaders[GE_MAX_SHADER];
static unsigned s_shaderCount = 0;
/* 50%-full open-addressing table. Fast3D looks up the same materials many
 * times per frame; a linear 512-slot scan was previously in that hot path. */
#define GE_SHADER_BUCKETS 1024u
static unsigned short s_shaderBuckets[GE_SHADER_BUCKETS];
static ShaderProgram* s_shader = 0;
static GENativeShader s_nativeShaders[GE_MAX_NATIVE_SHADER];
static uint64_t s_nativeShaderClock=0;
static GENativeShader *s_batchShader=0;
/* The renderer can select the same combiner for many separate Fast3D draws.
 * A validated one-entry cache avoids rescanning 256 shader slots when the
 * GPU batch was flushed by a texture, depth or alpha-state transition.
 * Shader slots never move: generation is checked by matching both key and
 * parent, so evictions cannot return a recycled shader for the old material. */
static GENativeShader *s_recentNativeShader=0;
static unsigned s_batchCount=0;
static DWORD s_batchConstants[8],s_batchFog=0;
#if GE_NV2A_SKIP_EMPTY_TRI_CONSTANTS
static const DWORD s_noTriangleConstants[8]={0u,0u,0u,0u,0u,0u,0u,0u};
#endif
static GESplitScratch s_splitScratch[8];
/* Rendering runs on a small Xbox thread stack. Keep polygon workspaces here. */
static GEAddressScratch s_addressScratch[4];
static GEPolygon s_addressInput;
static unsigned s_addressPieces;
static float s_trimMin[2][2],s_trimMax[2][2];
#if GE_NV2A_GAMEPLAY_LEAN_VERTEX
/* Storage dimensions are always POT (including the scanline atlas), so this
 * exact binary scaling replaces four float divisions per emitted vertex. */
static float s_uvInvStorage[2][2];
#endif
static unsigned s_trimEnabled[2][2];
static unsigned s_trimTexture[8],s_trimAxis[8],s_trimCount;
static float s_trimBound[8];
static GETexture s_textures[GE_MAX_TEX];
static GEFramebuffer s_fb[GE_MAX_FRAMEBUFFERS];
static unsigned s_fbCount=1;
static int s_currentFB=0;
static IDirect3DSurface8* s_originalRT=0;
static IDirect3DSurface8* s_originalDepth=0;
static unsigned s_textureCount = 0;
static unsigned s_textureBytes=0;
#if GE_NV2A_FAST_SWIZZLE
/* Reusable CPU-linear staging; no GPU owns this allocation. Grow only when
 * needed and free at detach, never malloc/free for each RDP texture upload. */
static DWORD* s_swizzleLinear=0;
static unsigned s_swizzleLinearBytes=0;
#endif
static IDirect3DTexture8* s_stripAtlas=0;
static DWORD* s_stripStaging=0;
static unsigned s_stripCount=0,s_stripReady=0,s_stripFailed=0,s_stripBuilds=0;
/* DrawPrimitiveUP remains a safe fallback if old RXDK rejects dynamic VBs. */
static IDirect3DVertexBuffer8* s_streamVB[2][GE_STREAM_FRAME_COUNT];
static unsigned s_streamOffset[2][GE_STREAM_FRAME_COUNT];
static unsigned s_streamFrameSlot=GE_STREAM_FRAME_COUNT-1u;
static IDirect3DVertexBuffer8* s_streamBound=0;
static unsigned s_streamFallback=0;
static unsigned s_streamVBSubmissions=0,s_streamUPSubmissions=0;
/* One immutable noise image, separate from the evictable game image cache.
 * Neither its pixels nor its frame offsets consume the game's replay RNG. */
static IDirect3DTexture8* s_noiseTexture=0;
static uint32_t s_noiseFrame=0;
static unsigned s_noiseOffsetX=0,s_noiseOffsetY=0;
static float s_noiseScale=1.0f;
static unsigned s_selectedTex[2] = {0,0};
static unsigned s_addressMode[2][2];
/* Cache only backend-owned state. Each original frame starts invalid so the
 * host's state-block overlays cannot leave a stale cross-frame assumption. */
struct GESamplerState {
    DWORD address[2];
    unsigned addressValid,filterValid,linear;
};
struct GEBatchState {
    DWORD shaderHandle,constants[8],fogColor,alphaRef,vertexFVF;
    unsigned shaderValid,constantsValid,vertexValid,fogValid,alphaValid;
    unsigned noiseValid,noiseEnabled;
};
/* These are backend-owned D3D8 states. Duplicates are common when hundreds of
 * Fast3D display-list batches switch materials without changing depth/alpha.
 * Invalidate at each frame: the Xbox host can use state blocks for UI draws. */
struct GEPipelineState {
    DWORD depthEnable,depthWrite,depthFunc,alphaEnable,srcBlend,dstBlend;
    unsigned validMask;
};
static GEPipelineState s_pipelineState;
/* Runtime-independent texture UV conversion values. Computed once per draw
 * rather than reloading four texture descriptors for every output vertex. */
static float s_uvLogical[2][2],s_uvStorage[2][2],s_uvGutter[2][2];
static GESamplerState s_samplerState[2];
static GEBatchState s_batchState;
/* SetTexture is a device command even when a stage already has this texture.
 * Track bindings locally, invalidating at the frame boundary and after any
 * framebuffer or explicit unbind operation. This cache owns no references. */
static IDirect3DTexture8* s_boundGameTexture[2];
static unsigned s_boundGameTextureValid[2];
static void invalidateDeviceState(void) {
    s_streamBound=0;
    memset(s_samplerState,0,sizeof(s_samplerState));
    memset(&s_batchState,0,sizeof(s_batchState));
    memset(&s_pipelineState,0,sizeof(s_pipelineState));
    s_boundGameTexture[0]=s_boundGameTexture[1]=0;
    s_boundGameTextureValid[0]=s_boundGameTextureValid[1]=0;
}
static unsigned s_lastSelectedTile = 0;
static GEVertexBuffer s_vertices;
static unsigned s_unsupported = 0;
static unsigned s_drawCount = 0;
static GEBackendFrameStats s_frameStats;
/* Explicit backend statistics ABI: fail with a readable message if the old
 * header was retained when merging the runtime files.  Never depend on an
 * unchecked hard-coded negative-sized array for version detection. */
#if !defined(GE_XBOX_BACKEND_STATS_ABI) || GE_XBOX_BACKEND_STATS_ABI != 2
#error This xb_ge_fast3d.cpp requires the matching xb_ge_fast3d.h from the RXDK compatibility patch.
#endif
#if GE_NV2A_PHASE_PROFILE
extern "C" uint64_t sysGetMicroseconds(void);
/* NO constructor, destructor, exception unwinding, RTTI, or C++ runtime.
 * A wrapper measures the entire callback, including paths that return early.
 * draw_api_us INCLUDES nested submit_us; never add the two as disjoint phases.
 * Accumulators are bounded so a long frame cannot wrap unsigned counters. */
static void recordNativePhase(unsigned *counter,uint64_t start) {
    const uint64_t now=sysGetMicroseconds();
    if(now<start)return;
    const uint64_t elapsed=now-start;
    const unsigned space=0xFFFFFFFFu-*counter;
    *counter+=(unsigned)(elapsed>(uint64_t)space?space:elapsed);
}
#endif
/* Adjacent Fast3D draw_triangles() calls can carry identical material state.
 * Keep the Xbox vertex stream pending across interpreter calls and drain it
 * only when an actual state/resource/framebuffer boundary is crossed.
 * The decomp's triangle order is never changed. */
static void flushNativeBatch(void);
static void nativeBatchBarrier(void) {
    if(s_batchCount)flushNativeBatch();
}
static unsigned s_filter = 0;
static float s_zNear=0.0f,s_zFar=1.0f;
static D3DVIEWPORT8 s_viewport;
static unsigned s_scissor = 0;
static LONG s_scX=0,s_scY=0,s_scW=640,s_scH=480;
/* Valid only when a hardware rectangle has actually been installed. */
static unsigned s_hwScissorActive=0;
/* Keep clears unaffected by RDP scissoring, and restore the RDP scissor after
 * binding render targets. Team Resurgent's MK64-X uses the same old-RXDK API. */
static void fullHardwareScissor(void) {
#if GE_NV2A_HW_SCISSOR
    if(!s_device)return;
    const LONG w=(LONG)(s_currentFB?s_fb[s_currentFB].width:s_width);
    const LONG h=(LONG)(s_currentFB?s_fb[s_currentFB].height:s_height);
    D3DRECT r={0,0,w,h};
    s_device->SetScissors(1,FALSE,&r);
    s_hwScissorActive=0;
#endif
}
static void applyHardwareScissor(void) {
#if GE_NV2A_HW_SCISSOR
    if(!s_device)return;
    const LONG w=(LONG)(s_currentFB?s_fb[s_currentFB].width:s_width);
    const LONG h=(LONG)(s_currentFB?s_fb[s_currentFB].height:s_height);
    const LONG x=s_scissor?s_scX:0;
    const LONG y=s_scissor?s_scY:0;
    const LONG sw=s_scissor?s_scW:w;
    const LONG sh=s_scissor?s_scH:h;
    /* Overflow, empty, or out-of-target scissors stay with the original
     * software clipper; never issue an invalid NV2A scissor rectangle. */
    if(x<0 || y<0 || sw<=0 || sh<=0 || x>w || y>h || sw>w-x || sh>h-y) {
        fullHardwareScissor();return;
    }
    D3DRECT r={x,y,x+sw,y+sh};
    s_device->SetScissors(1,FALSE,&r);
    s_hwScissorActive=1;
#endif
}
static int s_depthTest=0,s_depthWrite=0;
static int s_decalDepth=0,s_depthClearPending=0;

extern "C" int gfx_rdp_affine,gfx_fog_vertex;
extern "C" void sysFatalError(const char *format,...);
static void backendError(const char *message) {
    ++s_unsupported;
    sysFatalError("%s",message);
}
static void clearNativeShaders(void) {
    nativeBatchBarrier();
    /* A catalogue reset may occur before api_end. Unbind while we still
     * know the owned texture, before invalidating the cached binding. */
    if(s_device && s_noiseTexture)s_device->SetTexture(2,0);
    memset(&s_batchState,0,sizeof(s_batchState));
    if(s_device)for(unsigned i=0;i<GE_MAX_NATIVE_SHADER;++i)
        if(s_nativeShaders[i].used)s_device->DeletePixelShader(s_nativeShaders[i].handle);
    memset(s_nativeShaders,0,sizeof(s_nativeShaders));
    s_batchShader=0;s_batchCount=0;s_nativeShaderClock=0;
    s_recentNativeShader=0;
}

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static unsigned pack8(float f) {
    float a=clamp01(f)*255.0f + .5f;
    /* The clamped value fits signed int; avoid a float-to-unsigned helper
     * on classic x86 compilers before converting the nonnegative result. */
    return (unsigned)(int)a;
}
static DWORD packColor(float r,float g,float b,float a) {
    return (pack8(a)<<24)|(pack8(r)<<16)|(pack8(g)<<8)|pack8(b);
}
static uint32_t noiseHash(uint32_t value) {
    /* Defined unsigned 32-bit arithmetic; no 64-bit helper or game RNG. */
    value^=value>>16;value*=0x7feb352du;
    value^=value>>15;value*=0x846ca68bu;
    return value^(value>>16);
}
static bool ensureNoiseTexture(void) {
    if(s_noiseTexture)return true;
    IDirect3DTexture8* texture=0;
    if(FAILED(s_device->CreateTexture(GE_NOISE_SIZE,GE_NOISE_SIZE,1,0,
            D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture)) || !texture) {
        if(texture)texture->Release();
        backendError("Xbox alpha-noise texture allocation failed");return false;
    }
    D3DLOCKED_RECT lock;
    memset(&lock,0,sizeof(lock));
    if(FAILED(texture->LockRect(0,&lock,0,0))) {
        texture->Release();
        backendError("Xbox alpha-noise texture lock failed");return false;
    }
    if(!lock.pBits) {
        texture->UnlockRect(0);texture->Release();
        backendError("Xbox alpha-noise texture has no upload memory");return false;
    }
    DWORD* pixels=(DWORD*)lock.pBits;
    /* Square POT swizzle: interleave seven x/y bits into a 14-bit index.
     * LockRect pitch does not describe swizzled memory. */
    for(unsigned y=0;y<GE_NOISE_SIZE;++y)for(unsigned x=0;x<GE_NOISE_SIZE;++x) {
        unsigned index=0;
        for(unsigned bit=0;bit<7;++bit)
            index|=((x>>bit)&1u)<<(bit*2u)|((y>>bit)&1u)<<(bit*2u+1u);
        const unsigned random=noiseHash(y*GE_NOISE_SIZE+x+0x007b00d1u)>>24;
        const unsigned high=128u+(random>>1);
        pixels[index]=((255u-random)<<24)|(high<<16)|(high<<8)|high;
    }
    texture->UnlockRect(0);
    s_noiseTexture=texture;
    ++s_frameStats.texture_uploads;s_frameStats.texture_upload_bytes+=GE_NOISE_BYTES;
    return true;
}
static bool bindNoiseTexture(bool enabled) {
    if(enabled && !ensureNoiseTexture())return false;
    if(!s_batchState.noiseValid || s_batchState.noiseEnabled!=(unsigned)enabled) {
        s_device->SetTexture(2,enabled?s_noiseTexture:0);
        if(enabled) {
            s_device->SetTextureStageState(2,D3DTSS_TEXCOORDINDEX,2);
            s_device->SetTextureStageState(2,D3DTSS_MINFILTER,D3DTEXF_POINT);
            s_device->SetTextureStageState(2,D3DTSS_MAGFILTER,D3DTEXF_POINT);
            s_device->SetTextureStageState(2,D3DTSS_MIPFILTER,D3DTEXF_NONE);
            s_device->SetTextureStageState(2,D3DTSS_ADDRESSU,D3DTADDRESS_WRAP);
            s_device->SetTextureStageState(2,D3DTSS_ADDRESSV,D3DTADDRESS_WRAP);
        }
        s_batchState.noiseEnabled=(unsigned)enabled;s_batchState.noiseValid=1;
    }
    return true;
}
static void releaseTexture(unsigned i) {
    if(i>=GE_MAX_TEX) return;
    if(s_textures[i].tex){
        const unsigned used=4u*s_textures[i].storageWidth*s_textures[i].storageHeight;
        if(used<=s_textureBytes)s_textureBytes-=used;
        else s_textureBytes=0;
        s_textures[i].tex->Release();s_textures[i].tex=0;
    }
    s_textures[i].live=0;s_textures[i].width=0;s_textures[i].height=0;
    s_textures[i].stripSlot=0;
    s_textures[i].storageWidth=0;s_textures[i].storageHeight=0;
}
static void clearTextures(void) {
    for(unsigned i=0;i<s_textureCount;++i) releaseTexture(i);
    s_textureCount=0;s_selectedTex[0]=s_selectedTex[1]=0;
    s_textureBytes=0;
    if(s_stripAtlas){s_stripAtlas->Release();s_stripAtlas=0;}
    if(s_stripStaging){free(s_stripStaging);s_stripStaging=0;}
    s_stripCount=s_stripReady=s_stripFailed=0;
    s_boundGameTexture[0]=s_boundGameTexture[1]=0;
    s_boundGameTextureValid[0]=s_boundGameTextureValid[1]=0;
    memset(s_addressMode,0,sizeof(s_addressMode));
}

extern "C" void ge_xbox_d3d8_attach(void* xbox_device,unsigned width,unsigned height){
    s_device=(IDirect3DDevice8*)xbox_device;
    invalidateDeviceState();
    s_width=width?width:640;s_height=height?height:480;
    memset(&s_viewport,0,sizeof(s_viewport));
    s_viewport.X=0;s_viewport.Y=0;
    s_viewport.Width=s_width;s_viewport.Height=s_height;
    s_viewport.MinZ=0.0f;s_viewport.MaxZ=1.0f;
    s_scX=0;s_scY=0;s_scW=s_width;s_scH=s_height;
    memset(s_fb,0,sizeof(s_fb));
    s_fbCount=1;
    s_currentFB=0;
    s_originalRT=0;
    s_originalDepth=0;
    if(s_device){
        s_device->GetRenderTarget(&s_originalRT);
        s_device->GetDepthStencilSurface(&s_originalDepth);
    }
    s_unsupported=0;s_drawCount=0;memset(&s_frameStats,0,sizeof(s_frameStats));
    clearRoomPackets();
    s_textureBytes=0;
    s_stripAtlas=0;s_stripStaging=0;s_stripCount=s_stripReady=s_stripFailed=0;
    s_stripBuilds=0;
    memset(s_streamVB,0,sizeof(s_streamVB));
    memset(s_streamOffset,0,sizeof(s_streamOffset));
    s_streamFrameSlot=GE_STREAM_FRAME_COUNT-1u;
    memset(s_shaderBuckets,0,sizeof(s_shaderBuckets));
    s_streamFallback=s_streamVBSubmissions=s_streamUPSubmissions=0;
    s_noiseFrame=0;s_noiseOffsetX=s_noiseOffsetY=0;s_noiseScale=1.0f;
}
extern "C" void ge_xbox_d3d8_detach(void) {
    nativeBatchBarrier();
    clearRoomPackets();
    clearNativeShaders();
    if(s_device)s_device->SetTexture(2,0);
    if(s_noiseTexture){s_noiseTexture->Release();s_noiseTexture=0;}
    for(unsigned i=1;i<s_fbCount;++i){
        if(s_fb[i].depth){s_fb[i].depth->Release();s_fb[i].depth=0;}
        if(s_fb[i].surface){s_fb[i].surface->Release();s_fb[i].surface=0;}
        if(s_fb[i].texture){s_fb[i].texture->Release();s_fb[i].texture=0;}
    }
    if(s_originalRT){s_originalRT->Release();s_originalRT=0;}
    if(s_originalDepth){s_originalDepth->Release();s_originalDepth=0;}
    if(s_device)s_device->SetStreamSource(0,0,0);
    for(unsigned i=0;i<2u;++i)for(unsigned n=0;n<GE_STREAM_FRAME_COUNT;++n){
        if(s_streamVB[i][n]){s_streamVB[i][n]->Release();s_streamVB[i][n]=0;}
        s_streamOffset[i][n]=0;
    }
    s_streamBound=0;
    if(s_device){s_device->SetTexture(0,0);s_device->SetTexture(1,0);}
    clearTextures();s_shader=0;s_shaderCount=0;s_device=0;
#if GE_NV2A_FAST_SWIZZLE
    free(s_swizzleLinear);s_swizzleLinear=0;s_swizzleLinearBytes=0;
#endif
}
extern "C" unsigned ge_xbox_d3d8_unsupported(void){return s_unsupported;}
extern "C" unsigned ge_xbox_d3d8_drawn_triangles(void){return s_drawCount;}
extern "C" unsigned ge_xbox_d3d8_room_hardware_triangles(void){return s_roomGPUFrameTris;}
extern "C" unsigned ge_xbox_d3d8_room_hardware_triangles_total(void){return s_roomGPUTriangles;}
extern "C" unsigned ge_xbox_d3d8_texture_bytes(void){return s_textureBytes+(s_noiseTexture?GE_NOISE_BYTES:0u);}
extern "C" unsigned ge_xbox_d3d8_atlas_rows(void){return s_stripReady?s_stripCount:0;}
extern "C" unsigned ge_xbox_d3d8_atlas_builds(void){return s_stripBuilds;}
extern "C" unsigned ge_xbox_d3d8_stream_vb_draws(void){return s_streamVBSubmissions;}
extern "C" unsigned ge_xbox_d3d8_stream_up_draws(void){return s_streamUPSubmissions;}
extern "C" void ge_xbox_d3d8_frame_stats(GEBackendFrameStats* out){if(out)*out=s_frameStats;}
/* The N64 display-list extension invokes a native D3D8 logo renderer.
 * gfx_flush() only hands triangles to us; our cross-call batcher can still
 * retain them. Drain BEFORE the logo, or the deferred RDP background can
 * overdraw the custom 007xb wordmark later in the same frame. */
extern "C" void ge_xbox_d3d8_external_begin(void){nativeBatchBarrier();}
extern "C" void ge_xbox_d3d8_external_end(void){
    /* A direct D3D8 renderer changes stream source and FVF. The host restores
     * its D3DSBT_ALL state block, but explicit stream rebind on the next draw
     * is necessary on the classic Xbox D3D8 device. Do not touch RNG/ticks. */
    s_streamBound=0;
    s_batchState.vertexValid=0;
    s_batchState.shaderValid=0;
    s_batchState.constantsValid=0;
    s_batchState.fogValid=0;
    s_batchState.alphaValid=0;
    s_batchState.noiseValid=0;
    /* Re-establish bindings: the external logo cleared stage 0, and some
     * Xbox drivers do not restore stream-bound resource references reliably
     * from a state block. This happens once per logo draw, not per triangle. */
    if(s_device)for(unsigned tile=0;tile<2;++tile)
        if(s_boundGameTextureValid[tile])s_device->SetTexture(tile,s_boundGameTexture[tile]);
    /* Restore GPU scissor after the host's direct D3D8 logo/state block. */
    applyHardwareScissor();
}


static const char* api_name(void) {return "Xbox Direct3D 8 (native)";}
static int api_max_texture(void) {return (int)GE_MAX_TEX_DIM;}
static GfxClipParameters api_clip(void){
    GfxClipParameters c;
    c.z_is_from_0_to_1=false;
    c.invert_y=false;
    return c;
}
static void api_unload_shader(ShaderProgram*){}
static void api_load_shader(ShaderProgram* p){s_shader=p;}
static unsigned shaderBucket(uint64_t k0,unsigned k1){
    unsigned h=(unsigned)k0 ^ (unsigned)(k0>>32) ^ (k1*0x9E3779B9u);
    h^=h>>16;h*=0x7FEB352Du;h^=h>>15;
    return h&(GE_SHADER_BUCKETS-1u);
}
static ShaderProgram* api_lookup(uint64_t k0,unsigned k1){
    unsigned slot=shaderBucket(k0,k1);
    /* At most 512 live entries in 1024 buckets, so an empty slot always
     * terminates lookup. A table reset happens only with clear_shaders. */
    while(s_shaderBuckets[slot]){
        ++s_frameStats.shader_probes;
        ShaderProgram* p=&s_shaders[s_shaderBuckets[slot]-1u];
        if(p->id0==k0 && p->id1==k1)return p;
        slot=(slot+1u)&(GE_SHADER_BUCKETS-1u);
    }
    ++s_frameStats.shader_probes;
    return 0;
}
static ShaderProgram* api_create_shader(uint64_t k0,unsigned k1){
    ShaderProgram* p=api_lookup(k0,k1);
    if(p){s_shader=p;return p;}
    if(s_shaderCount>=GE_MAX_SHADER){backendError("Native combiner catalogue exhausted");return 0;}
    p=&s_shaders[s_shaderCount++];
    memset(p,0,sizeof(*p));p->id0=k0;p->id1=k1;
    CCFeatures f; memset(&f,0,sizeof(f));
    gfx_cc_get_features(k0,k1,&f);
    p->features=f;
    unsigned slot=shaderBucket(k0,k1);
    while(s_shaderBuckets[slot])slot=(slot+1u)&(GE_SHADER_BUCKETS-1u);
    s_shaderBuckets[slot]=(unsigned short)s_shaderCount;
    p->inputs=(unsigned char)f.num_inputs;
    p->useTex[0]=(unsigned char)f.used_textures[0];
    p->useTex[1]=(unsigned char)f.used_textures[1];
    p->fog=(unsigned char)f.opt_fog;
    p->alpha=(unsigned char)f.opt_alpha;
    p->grayscale=(unsigned char)f.opt_grayscale;
    for(int i=0;i<2;++i)for(int j=0;j<2;++j)
        p->clamps[i][j]=(unsigned char)f.clamp[i][j];
    s_shader=p;
    return p;
}
static void api_shader_info(ShaderProgram* p,unsigned char* n,bool used[2]){
    if(n)*n=p?p->inputs:0;
    if(used){used[0]=p?p->useTex[0]!=0:false;used[1]=p?p->useTex[1]!=0:false;}
}
static void api_clear_shaders(void){clearNativeShaders();s_shaderCount=0;s_shader=0;memset(s_shaderBuckets,0,sizeof(s_shaderBuckets));}
static unsigned api_new_texture(void){
    unsigned i=GE_MAX_TEX;
    for(unsigned k=0;k<s_textureCount;++k)if(!s_textures[k].live){i=k;break;}
    if(i==GE_MAX_TEX){
        if(s_textureCount>=GE_MAX_TEX){backendError("Native texture catalogue exhausted");return 0xFFFFFFFFu;}
        i=s_textureCount++;
    }
    memset(&s_textures[i],0,sizeof(s_textures[i]));
    s_textures[i].live=1;
    return i;
}
static unsigned textureStorageSize(unsigned size) {
    if((size&(size-1u))==0)return size;
    /* A wrapped texel at either edge preserves bilinear repeat seams.
     * Clamp/mirror coordinates are clipped to the logical edge centres. */
    unsigned storage=1;
    while(storage<size+2u)storage<<=1;
    return storage;
}
/* A strip is eligible only in the original CLAMP/CLAMP sampler mode.
 * Other states retain their original independent image and CPU addressing. */
static bool stripAtlasSelected(unsigned tile,unsigned id) {
    return GE_NV2A_STRIP_ATLAS && s_stripReady && s_stripAtlas && id<s_textureCount &&
        s_textures[id].stripSlot && s_addressMode[tile][0]==2u &&
        s_addressMode[tile][1]==2u;
}
static IDirect3DTexture8* physicalTexture(unsigned tile,unsigned id) {
    if(id>=s_textureCount)return 0;
    return stripAtlasSelected(tile,id)?s_stripAtlas:s_textures[id].tex;
}
/* Xbox swizzled 512x1024 image address; x has nine bits, y ten. */
static unsigned stripMorton(unsigned x,unsigned y) {
    unsigned index=0;
    for(unsigned k=0;k<9u;++k)
        index|=((x>>k)&1u)<<(2u*k)|((y>>k)&1u)<<(2u*k+1u);
    return index|((y>>9u)&1u)<<18u;
}
static void stripStageRow(unsigned slot,const unsigned char* rgba) {
    if(!s_stripStaging || slot>=GE_STRIP_MAX_SLOTS)return;
    const unsigned centre=slot*GE_STRIP_SLOT_PITCH+1u;
    for(unsigned x=0;x<GE_STRIP_ATLAS_W;++x) {
        const unsigned srcX=x?(x-1u)%GE_STRIP_WIDTH:GE_STRIP_WIDTH-1u;
        const unsigned char* pixel=rgba+4u*srcX;
        const DWORD color=((DWORD)pixel[3]<<24)|((DWORD)pixel[0]<<16)|
            ((DWORD)pixel[1]<<8)|(DWORD)pixel[2];
        for(unsigned y=centre-1u;y<=centre+1u;++y)
            s_stripStaging[stripMorton(x,y)]=color;
    }
}
static void stripBuildAtlas(void) {
    if(s_stripFailed || s_stripReady || !s_stripStaging ||
       s_stripCount<GE_STRIP_ROWS_REQUIRED || !s_device)return;
    const unsigned bytes=GE_STRIP_ATLAS_W*GE_STRIP_ATLAS_H*4u;
    IDirect3DTexture8* tex=0;
    bool usable=bytes<=GE_TEXTURE_BUDGET-s_textureBytes;
    if(usable)usable=SUCCEEDED(s_device->CreateTexture(GE_STRIP_ATLAS_W,
        GE_STRIP_ATLAS_H,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&tex)) && tex;
    if(usable) {
        D3DLOCKED_RECT lock;memset(&lock,0,sizeof(lock));
        usable=SUCCEEDED(tex->LockRect(0,&lock,0,0)) && lock.pBits;
        if(usable) {
            memcpy(lock.pBits,s_stripStaging,bytes);
            tex->UnlockRect(0);
            s_stripAtlas=tex;s_stripReady=1;s_textureBytes+=bytes;
            ++s_stripBuilds;
            ++s_frameStats.texture_uploads;s_frameStats.texture_upload_bytes+=bytes;
        }
    }
    if(!usable) {
        if(tex)tex->Release();
        s_stripFailed=1;
        /* Keep the already uploaded source textures as a permanent fallback;
         * never attempt another costly 2 MiB atlas build every frame. */
        for(unsigned id=0;id<s_textureCount;++id)s_textures[id].stripSlot=0;
        s_stripCount=0;
    }
    free(s_stripStaging);s_stripStaging=0;
}
static void applyTextureAddress(unsigned tile) {
    const GETexture& t=s_textures[s_selectedTex[tile]];
    for(unsigned ax=0;ax<2;++ax) {
        unsigned size=ax?t.height:t.width;
        unsigned storage=ax?t.storageHeight:t.storageWidth;
        unsigned mode=s_addressMode[tile][ax];
        DWORD address=D3DTADDRESS_CLAMP;
        /* Padded axes are addressed in software before UV normalization. */
        if(size && size==storage && !(mode&2u))
            address=(mode&1u)?D3DTADDRESS_MIRROR:D3DTADDRESS_WRAP;
        GESamplerState& state=s_samplerState[tile];
        if(!(state.addressValid&(1u<<ax)) || state.address[ax]!=address) {
            s_device->SetTextureStageState(tile,ax?D3DTSS_ADDRESSV:D3DTSS_ADDRESSU,address);
            state.address[ax]=address;state.addressValid|=1u<<ax;
        }
    }
}
static void api_select_texture(int tile,unsigned texID,bool linear){
    if(tile<0||tile>1||texID>=s_textureCount||!s_textures[texID].live){backendError("Invalid native texture selection");return;}
    /* Even a perfectly cached device binding cannot be changed before the
     * previous material's queued triangles have reached D3D8. */
    IDirect3DTexture8* image=physicalTexture((unsigned)tile,texID);
    if(s_selectedTex[tile]!=texID &&
       (!s_boundGameTextureValid[tile] || s_boundGameTexture[tile]!=image))
        nativeBatchBarrier();
    s_selectedTex[tile]=texID;
    s_lastSelectedTile=(unsigned)tile;
    if(s_device && image) {
        if(!s_boundGameTextureValid[tile] || s_boundGameTexture[tile]!=image) {
            s_device->SetTexture((DWORD)tile,image);
            s_boundGameTexture[tile]=image;
            s_boundGameTextureValid[tile]=1;
        }
        applyTextureAddress((unsigned)tile);
    }
    (void)linear;
}
static void api_upload_textureImpl(const unsigned char* rgba,unsigned width,unsigned height,bool mipmaps){
    if(!s_device || !rgba || !width || !height || width>GE_MAX_TEX_DIM || height>GE_MAX_TEX_DIM){
        backendError("Invalid native texture upload dimensions");return;
    }
    const unsigned id=s_selectedTex[s_lastSelectedTile];
    if(id>=s_textureCount || !s_textures[id].live){backendError("Native texture upload has no selected image");return;}
    nativeBatchBarrier(); /* Never LockRect a texture still referenced by queued primitives. */
    GETexture* t=&s_textures[id];
    /* Reuploads invalidate the immutable atlas copy for that texture ID. */
    if(t->stripSlot)t->stripSlot=0;
    /* Only group the original 440x1 scanline images. Build an atlas after a
     * complete 299-row upload run; during that first frame the legacy images
     * render unchanged. The page is not locked while the GPU reads it. */
    if(GE_NV2A_STRIP_ATLAS && !s_stripReady && !s_stripFailed && !mipmaps &&
       width==GE_STRIP_WIDTH && height==1u &&
       s_stripCount<GE_STRIP_MAX_SLOTS) {
        if(!s_stripStaging) {
            const unsigned bytes=GE_STRIP_ATLAS_W*GE_STRIP_ATLAS_H*4u;
            s_stripStaging=(DWORD*)malloc(bytes);
            if(s_stripStaging)memset(s_stripStaging,0,bytes);
        }
        if(s_stripStaging) {
            const unsigned slot=s_stripCount++;
            stripStageRow(slot,rgba);
            t->stripSlot=slot+1u;
        }
    }
    const unsigned storageWidth=textureStorageSize(width);
    const unsigned storageHeight=textureStorageSize(height);
    if(t->tex&&(t->width!=width||t->height!=height))releaseTexture(id);
    if(!t->tex){
        const unsigned bytes=4u*storageWidth*storageHeight;
        if(bytes>GE_TEXTURE_BUDGET-s_textureBytes){backendError("Native texture memory budget exhausted");return;}
        if(FAILED(s_device->CreateTexture(storageWidth,storageHeight,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&t->tex))||!t->tex){
            backendError("Xbox texture allocation failed");return;
        }
        t->width=width;t->height=height;
        t->storageWidth=storageWidth;t->storageHeight=storageHeight;
        s_textureBytes+=bytes;
    }
    D3DLOCKED_RECT lock;
    const unsigned long long p0TexLockStart=geP0BStart();
    const HRESULT texLockResult=t->tex->LockRect(0,&lock,0,0);
    geP0BEnd(GE_P0B_TEX_LOCK,p0TexLockStart);
    if(FAILED(texLockResult)){backendError("Xbox texture upload lock failed");return;}
    /* Preserve the original odd-NPOT leading gutter EXACTLY. The native
     * texture sees the same 32-bit A8R8G8B8 pixels and identical source
     * x/y wrap order as the manual swizzler, but XGSwizzleRect writes its
     * Morton layout using the Xbox SDK's optimized routine. */
    const unsigned bytes=4u*storageWidth*storageHeight;
    bool usedFastSwizzle=false;
#if GE_NV2A_FAST_SWIZZLE
    /* Avoid retaining a second multi-megabyte copy of a huge texture on a
     * retail 64 MiB Xbox. Large uploads keep the original one-pass path. */
    if(storageWidth>=8u && storageHeight>=8u && bytes<=512u*1024u && s_swizzleLinearBytes<bytes) {
        DWORD* enlarged=(DWORD*)malloc(bytes);
        if(enlarged) {
            free(s_swizzleLinear);s_swizzleLinear=enlarged;
            s_swizzleLinearBytes=bytes;
        }
    }
    if(storageWidth>=8u && storageHeight>=8u && bytes<=512u*1024u && s_swizzleLinear && s_swizzleLinearBytes>=bytes) {
        unsigned sourceY=storageHeight==height?0:height-1u;
        for(unsigned y=0;y<storageHeight;++y) {
            const unsigned char* src=rgba+4u*width*sourceY;
            DWORD* dst=s_swizzleLinear+y*storageWidth;
            unsigned sourceX=storageWidth==width?0:width-1u;
            for(unsigned x=0;x<storageWidth;++x) {
                const unsigned char* pixel=src+4u*sourceX;
                dst[x]=((DWORD)pixel[3]<<24)|((DWORD)pixel[0]<<16)|
                       ((DWORD)pixel[1]<<8)|(DWORD)pixel[2];
                if(++sourceX==width)sourceX=0;
            }
            if(++sourceY==height)sourceY=0;
        }
        /* Pitch=0 selects the tight full rectangle, exactly as in MK64-X.
         * Texture LockRect memory is swizzled, NOT row-pitch addressable. */
        XGSwizzleRect(s_swizzleLinear,0,NULL,lock.pBits,
                      storageWidth,storageHeight,NULL,4);
        usedFastSwizzle=true;
        ++s_frameStats.fast_swizzles;
    }
#endif
    if(!usedFastSwizzle) {
        /* Allocation failure / A-B switch: preserve the verified legacy
         * Morton path with no dependency on any special Xbox swizzle flag. */
        unsigned xmask=0,ymask=0,bit=1;
        for(unsigned coordinate=1;coordinate<storageWidth || coordinate<storageHeight;coordinate<<=1){
            if(coordinate<storageWidth){xmask|=bit;bit<<=1;}
            if(coordinate<storageHeight){ymask|=bit;bit<<=1;}
        }
        DWORD* dst=(DWORD*)lock.pBits;
        unsigned yoffset=0,sourceY=storageHeight==height?0:height-1u;
        for(unsigned y=0;y<storageHeight;++y){
            const unsigned char* src=rgba+4u*width*sourceY;
            unsigned xoffset=0,sourceX=storageWidth==width?0:width-1u;
            for(unsigned x=0;x<storageWidth;++x){
                const unsigned char* pixel=src+4u*sourceX;
                dst[xoffset|yoffset]=((DWORD)pixel[3]<<24)|((DWORD)pixel[0]<<16)|
                    ((DWORD)pixel[1]<<8)|(DWORD)pixel[2];
                xoffset=(xoffset-xmask)&xmask;
                if(++sourceX==width)sourceX=0;
            }
            yoffset=(yoffset-ymask)&ymask;
            if(++sourceY==height)sourceY=0;
        }
    }
    t->tex->UnlockRect(0);
    ++s_frameStats.texture_uploads;s_frameStats.texture_upload_bytes+=4u*storageWidth*storageHeight;
    t->width=width;t->height=height;t->live=1;
    s_device->SetTexture(s_lastSelectedTile,t->tex);
    s_boundGameTexture[s_lastSelectedTile]=t->tex;
    s_boundGameTextureValid[s_lastSelectedTile]=1;
    applyTextureAddress(s_lastSelectedTile);
    if(mipmaps)++s_unsupported;
}
static void api_upload_texture(const unsigned char* rgba,unsigned width,unsigned height,bool mipmaps) {
    const unsigned long long p0UploadStart=geP0BStart();
#if GE_NV2A_PHASE_PROFILE
    const uint64_t start=sysGetMicroseconds();
#endif
    api_upload_textureImpl(rgba,width,height,mipmaps);
    geP0BEnd(GE_P0B_UPLOAD,p0UploadStart);
#if GE_NV2A_PHASE_PROFILE
    recordNativePhase(&s_frameStats.upload_api_us,start);
#endif
}
static void api_sampler(int tile,bool linear,unsigned cms,unsigned cmt,bool mipmaps){
    if(!s_device||tile<0||tile>1)return;
    GESamplerState& state=s_samplerState[tile];
    if((!state.filterValid || state.linear!=(unsigned)linear ||
        s_addressMode[tile][0]!=cms || s_addressMode[tile][1]!=cmt))
        nativeBatchBarrier();
    if(!state.filterValid || state.linear!=(unsigned)linear) {
        s_device->SetTextureStageState(tile,D3DTSS_MINFILTER,linear?D3DTEXF_LINEAR:D3DTEXF_POINT);
        s_device->SetTextureStageState(tile,D3DTSS_MAGFILTER,linear?D3DTEXF_LINEAR:D3DTEXF_POINT);
        s_device->SetTextureStageState(tile,D3DTSS_MIPFILTER,D3DTEXF_NONE);
        state.linear=(unsigned)linear;state.filterValid=1;
    }
    s_addressMode[tile][0]=cms;s_addressMode[tile][1]=cmt;
    /* An atlas can only be used for CLAMP/CLAMP. Transitioning to wrapping
     * must restore the legacy image before emitting the next triangle. */
    const unsigned id=s_selectedTex[tile];
    IDirect3DTexture8* image=physicalTexture((unsigned)tile,id);
    if(image && (!s_boundGameTextureValid[tile] || s_boundGameTexture[tile]!=image)) {
        nativeBatchBarrier();
        s_device->SetTexture(tile,image);
        s_boundGameTexture[tile]=image;s_boundGameTextureValid[tile]=1;
    }
    applyTextureAddress((unsigned)tile);
    if(mipmaps)++s_unsupported;
}
static void api_depth(bool test,bool update,bool compare,bool source,unsigned short zmode){
    const DWORD enable=test?D3DZB_TRUE:D3DZB_FALSE;
    const DWORD write=update?TRUE:FALSE;
    DWORD function=D3DCMP_ALWAYS;
    if(compare)function=((zmode==0 || zmode==0x0800) && !source)?D3DCMP_LESS:D3DCMP_LESSEQUAL;
    const int decal=test && compare && zmode==0x0c00;
    if(s_depthTest!=(int)test || s_depthWrite!=(int)update || s_decalDepth!=decal ||
       !(s_pipelineState.validMask&7u) || s_pipelineState.depthFunc!=function)
        nativeBatchBarrier();
    s_depthTest=test?1:0;s_depthWrite=update?1:0;
    s_decalDepth=decal;
    if(!s_device)return;
    if(!(s_pipelineState.validMask&1u) || s_pipelineState.depthEnable!=enable) {
        s_device->SetRenderState(D3DRS_ZENABLE,enable);
        s_pipelineState.depthEnable=enable;s_pipelineState.validMask|=1u;
    }
    if(!(s_pipelineState.validMask&2u) || s_pipelineState.depthWrite!=write) {
        s_device->SetRenderState(D3DRS_ZWRITEENABLE,write);
        s_pipelineState.depthWrite=write;s_pipelineState.validMask|=2u;
    }
    if(!(s_pipelineState.validMask&4u) || s_pipelineState.depthFunc!=function) {
        s_device->SetRenderState(D3DRS_ZFUNC,function);
        s_pipelineState.depthFunc=function;s_pipelineState.validMask|=4u;
    }
}
static void api_depth_range(float n,float f){if(n!=s_zNear || f!=s_zFar)nativeBatchBarrier();s_zNear=n;s_zFar=f;}
static void api_viewport(int x,int y,int width,int height){
    if(!s_device||width<=0||height<=0)return;
    const int targetHeight=s_currentFB?s_fb[s_currentFB].height:s_height;
    y=targetHeight-y-height;
    D3DVIEWPORT8 next;
    next.X=x<0?0:x;next.Y=y<0?0:y;
    next.Width=(DWORD)width;next.Height=(DWORD)height;
    next.MinZ=0.0f;next.MaxZ=1.0f;
    if(memcmp(&next,&s_viewport,sizeof(next))==0)return;
    nativeBatchBarrier();
    s_viewport=next;
    s_device->SetViewport(&s_viewport);
    applyHardwareScissor();
}
static void api_scissor(int x,int y,int width,int height){
    const int targetHeight=s_currentFB?s_fb[s_currentFB].height:s_height;
    y=targetHeight-y-height;
    if(s_scissor && s_scX==x && s_scY==y && s_scW==width && s_scH==height)return;
    nativeBatchBarrier();
    s_scissor=1;s_scX=x;s_scY=y;s_scW=width;s_scH=height;
    /* The original CPU polygon clipper remains for unusual rectangles and
     * for the GE_NV2A_HW_SCISSOR=0 A/B build. */
    applyHardwareScissor();
}
static void api_alpha(bool enabled,bool modulate){
    if(!s_device)return;
    const DWORD enable=enabled?TRUE:FALSE;
    const DWORD src=modulate?D3DBLEND_DESTCOLOR:D3DBLEND_SRCALPHA;
    const DWORD dst=modulate?D3DBLEND_ZERO:D3DBLEND_INVSRCALPHA;
    if(!(s_pipelineState.validMask&56u) || s_pipelineState.alphaEnable!=enable ||
       s_pipelineState.srcBlend!=src || s_pipelineState.dstBlend!=dst)
        nativeBatchBarrier();
    if(!(s_pipelineState.validMask&8u) || s_pipelineState.alphaEnable!=enable) {
        s_device->SetRenderState(D3DRS_ALPHABLENDENABLE,enable);
        s_pipelineState.alphaEnable=enable;s_pipelineState.validMask|=8u;
    }
    if(!(s_pipelineState.validMask&16u) || s_pipelineState.srcBlend!=src) {
        s_device->SetRenderState(D3DRS_SRCBLEND,src);
        s_pipelineState.srcBlend=src;s_pipelineState.validMask|=16u;
    }
    if(!(s_pipelineState.validMask&32u) || s_pipelineState.dstBlend!=dst) {
        s_device->SetRenderState(D3DRS_DESTBLEND,dst);
        s_pipelineState.dstBlend=dst;s_pipelineState.validMask|=32u;
    }
}

#if GE_NV2A_MATERIAL_SHADER_CACHE
static void rememberNativeShader(ShaderProgram *parent,GENativeShader *n) {
    for(unsigned i=0;i<4u;++i)if(parent->nativeRecent[i]==n)return;
    parent->nativeRecent[(parent->nativeRecentNext++)&3u]=n;
}
#endif
static GENativeShader *nativeShader(ShaderProgram *parent,const GENV2AInputMap& map) {
    const unsigned key=geNV2AMapKey(map);
    /* A Fast3D draw normally keeps the same input map across its triangles.
     * The current batch owns a live, non-evictable cache slot: reuse it before
     * walking the full catalogue, while preserving the original LRU clock. */
    if(s_batchShader && s_batchShader->used && s_batchShader->parent==parent &&
       s_batchShader->key==key) {
        s_batchShader->lastUse=++s_nativeShaderClock;return s_batchShader;
    }
    if(s_recentNativeShader && s_recentNativeShader->used &&
       s_recentNativeShader->parent==parent && s_recentNativeShader->key==key) {
        s_recentNativeShader->lastUse=++s_nativeShaderClock;
#if GE_NV2A_MATERIAL_SHADER_CACHE
        rememberNativeShader(parent,s_recentNativeShader);
#endif
        return s_recentNativeShader;
    }
#if GE_NV2A_MATERIAL_SHADER_CACHE
    /* Validate both owner and key: an evicted slot may have been recycled. */
    for(unsigned i=0;i<4u;++i) {
        GENativeShader *n=parent->nativeRecent[i];
        if(n && n->used && n->parent==parent && n->key==key) {
            n->lastUse=++s_nativeShaderClock;s_recentNativeShader=n;
            return n;
        }
    }
#endif
    GENativeShader *slot=0,*oldest=0;
    for(unsigned i=0;i<GE_MAX_NATIVE_SHADER;++i) {
        GENativeShader *n=&s_nativeShaders[i];
        if(n->used && n->parent==parent && n->key==key) {
            n->lastUse=++s_nativeShaderClock;s_recentNativeShader=n;
#if GE_NV2A_MATERIAL_SHADER_CACHE
            rememberNativeShader(parent,n);
#endif
            return n;
        }
        if(!n->used && !slot)slot=n;
        if(n->used && n!=s_batchShader && (!oldest || n->lastUse<oldest->lastUse))oldest=n;
    }
    GENV2AProgram program;
    const char *error=geNV2ABuild(parent->features,map,&program);
    if(error){backendError(error);return 0;}
    D3DPIXELSHADERDEF def;
    memset(&def,0,sizeof(def));
    for(unsigned stage=0;stage<program.stages;++stage) {
        def.PSRGBInputs[stage]=program.rgbInputs[stage];
        def.PSAlphaInputs[stage]=program.alphaInputs[stage];
        def.PSRGBOutputs[stage]=program.rgbOutputs[stage];
        def.PSAlphaOutputs[stage]=program.alphaOutputs[stage];
    }
    def.PSFinalCombinerInputsABCD=program.finalABCD;
    def.PSFinalCombinerInputsEFG=program.finalEFG;
    def.PSC0Mapping=program.constantMap[0];
    def.PSC1Mapping=program.constantMap[1];
    def.PSTextureModes=program.textureModes;
    /* UNIQUE_C0/C1 + MUX_MSB. Each stage receives its own two constants. */
    def.PSCombinerCount=program.stages|0x00011100u;
    /* No final C0/C1 references; allow texture mode adjustment by the SDK. */
    def.PSFinalCombinerConstants=0x000001ffu;
    DWORD handle=0;
    const unsigned long long p0ShaderStart=geP0BStart();
    const HRESULT shaderResult=s_device->CreatePixelShader(&def,&handle);
    geP0BEnd(GE_P0B_SHADER_CREATE,p0ShaderStart);
    if(FAILED(shaderResult)) {
        backendError("RXDK could not create the native register combiner");return 0;
    }
    ++s_frameStats.shader_creates;
    if(!slot)slot=oldest;
    if(!slot) {
        s_device->DeletePixelShader(handle);
        backendError("Native shader cache has no reusable slot");return 0;
    }
    if(slot->used) {
        /* The SDK may recycle the numeric handle for a different program. */
        if(s_batchState.shaderValid && s_batchState.shaderHandle==slot->handle)
            s_batchState.shaderValid=0;
        s_device->DeletePixelShader(slot->handle);
    }
    memset(slot,0,sizeof(*slot));
    slot->parent=parent;slot->key=key;slot->handle=handle;slot->used=1;
    slot->lastUse=++s_nativeShaderClock;slot->map=map;slot->program=program;
#if GE_NV2A_SKIP_EMPTY_TRI_CONSTANTS
    /* Validate the entire compiled C0/C1 description, not just the source
     * map: a material may use a constant in only one equation channel.
     * Newly created shader slots are cleared above, including on eviction. */
    for(unsigned stage=0;stage<program.constantStages;++stage)
        for(unsigned c=0;c<2;++c)for(unsigned channel=0;channel<2;++channel)
            if(program.constantInput[stage][c][channel])
                slot->hasTriangleConstants=1;
#endif
    s_recentNativeShader=slot;
#if GE_NV2A_MATERIAL_SHADER_CACHE
    rememberNativeShader(parent,slot);
#endif
    return slot;
}

static bool drawStreamVB(const void* vertices,unsigned vertexCount,unsigned stride,unsigned type) {
#if GE_NV2A_STREAM_VB
    if(s_streamFallback || vertexCount<GE_NV2A_MIN_VB_VERTICES)return false;
    if(vertexCount>GE_STREAM_VERTICES)return false;
    const unsigned slot=s_streamFrameSlot;
    IDirect3DVertexBuffer8* vb=s_streamVB[type][slot];
    const unsigned bytes=vertexCount*stride;
    unsigned offset=s_streamOffset[type][slot];
    /* Do not start a second lock at zero on a buffer submitted earlier in
     * this frame. The old RXDK lacks DISCARD, so doing so serializes with
     * NV2A. A full region is sent via DrawPrimitiveUP instead. */
    if(offset+bytes>GE_STREAM_VERTICES*stride)return false;
    if(!vb) {
        if(FAILED(s_device->CreateVertexBuffer(GE_STREAM_VERTICES*stride,
            D3DUSAGE_WRITEONLY|D3DUSAGE_DYNAMIC,
            type?GE_NOISE_FVF:GE_FVF,D3DPOOL_DEFAULT,&vb)) || !vb) {
            s_streamFallback=1;return false;
        }
        s_streamVB[type][slot]=vb;
    }
    BYTE* mapped=0;
    const DWORD flags=offset?D3DLOCK_NOOVERWRITE:0u;
    ++s_frameStats.vb_locks;
    const unsigned long long p0LockStart=geP0BStart();
    const HRESULT lockResult=vb->Lock(offset,bytes,&mapped,flags);
    geP0BEnd(GE_P0B_VB_LOCK,p0LockStart);
    if(FAILED(lockResult) || !mapped) {
        s_streamFallback=1;return false;
    }
    memcpy(mapped,vertices,bytes);
    vb->Unlock();
    if(s_streamBound!=vb) {
        s_device->SetStreamSource(0,vb,stride);
        s_streamBound=vb;
    }
    s_streamOffset[type][slot]=offset+bytes;
    const unsigned long long p0VBDrawStart=geP0BStart();
    const HRESULT vbDrawResult=s_device->DrawPrimitive(D3DPT_TRIANGLELIST,offset/stride,vertexCount/3u);
    geP0BEnd(GE_P0B_DRAW_VB,p0VBDrawStart);
    if(FAILED(vbDrawResult)) {
        s_streamFallback=1;return false;
    }
    ++s_streamVBSubmissions;
    ++s_frameStats.vb_submissions;
    return true;
#else
    (void)vertices;(void)vertexCount;(void)stride;(void)type;
    return false;
#endif
}

static void flushNativeBatchImpl(void) {
    if(!s_batchCount)return;
    if(GENativePlatform_HasFatalError()){s_batchCount=0;return;}
    const CCFeatures& f=s_batchShader->parent->features;
    const bool noise=s_batchShader->program.usesNoise!=0;
    unsigned alphaRef=0;
    /* Upstream applies an unconditional 8/256 alpha-threshold test after
     * its texture-edge 1/256 branch, so eight is the effective threshold. */
    if(f.opt_alpha_threshold)alphaRef=8;
    else if(f.opt_texture_edge && f.opt_alpha)alphaRef=f.opt_noise?1u:128u;
    /* Non-edge noise preserves zero-alpha fragments, as in the PC shader. */
    if(!bindNoiseTexture(noise)){s_batchCount=0;return;}
    if(!s_batchState.shaderValid || s_batchState.shaderHandle!=s_batchShader->handle) {
        s_device->SetPixelShader(s_batchShader->handle);
        s_batchState.shaderHandle=s_batchShader->handle;s_batchState.shaderValid=1;
        /* Binding a pixel shader may reload its definition's constants. */
        s_batchState.constantsValid=s_batchState.fogValid=s_batchState.alphaValid=0;
    }
    if(!s_batchState.constantsValid || memcmp(s_batchState.constants,s_batchConstants,sizeof(s_batchConstants))) {
        float values[8][4];
        for(unsigned i=0;i<8;++i) {
            DWORD c=s_batchConstants[i];
            values[i][0]=((c>>16)&255u)/255.0f;
            values[i][1]=((c>>8)&255u)/255.0f;
            values[i][2]=(c&255u)/255.0f;
            values[i][3]=((c>>24)&255u)/255.0f;
        }
        s_device->SetPixelShaderConstant(0,values,8);
        memcpy(s_batchState.constants,s_batchConstants,sizeof(s_batchConstants));
        s_batchState.constantsValid=1;
    }
    if(!s_batchState.fogValid || s_batchState.fogColor!=s_batchFog) {
        s_device->SetRenderState(D3DRS_FOGENABLE,FALSE);
        s_device->SetRenderState(D3DRS_FOGCOLOR,s_batchFog);
        s_batchState.fogColor=s_batchFog;s_batchState.fogValid=1;
    }
    if(!s_batchState.alphaValid || s_batchState.alphaRef!=alphaRef) {
        s_device->SetRenderState(D3DRS_ALPHATESTENABLE,alphaRef?TRUE:FALSE);
        s_device->SetRenderState(D3DRS_ALPHAREF,alphaRef);
        s_device->SetRenderState(D3DRS_ALPHAFUNC,D3DCMP_GREATEREQUAL);
        s_batchState.alphaRef=alphaRef;s_batchState.alphaValid=1;
    }
    const DWORD fvf=noise?GE_NOISE_FVF:GE_FVF;
    if(!s_batchState.vertexValid || s_batchState.vertexFVF!=fvf) {
        /* NV2A gates V1 even for XYZRHW passthrough vertices. Old RXDK does
         * not infer its use from a custom pixel shader. Keep the secondary
         * RGBA channel available for combiner inputs and the fog factor. */
        s_device->SetRenderState(D3DRS_SPECULARENABLE,TRUE);
        s_device->SetVertexShader(fvf);
        s_batchState.vertexFVF=fvf;s_batchState.vertexValid=1;
    }
    const void* vertices=noise?(const void*)s_vertices.noisy:(const void*)s_vertices.regular;
    const unsigned stride=noise?sizeof(GENoiseVertex):sizeof(GEVertex);
    bool submitted=drawStreamVB(vertices,s_batchCount,stride,noise?1u:0u);
    if(!submitted) {
        /* Preserve the previous backend on old RXDKs or driver allocation
         * failures. This also permits a compile-time A/B comparison. */
        const unsigned long long p0UpDrawStart=geP0BStart();
        const HRESULT upDrawResult=s_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,s_batchCount/3u,vertices,stride);
        geP0BEnd(GE_P0B_DRAW_UP,p0UpDrawStart);
        if(FAILED(upDrawResult))
            backendError("Xbox native triangle submission failed");
        else {
            submitted=true;++s_streamUPSubmissions;
            ++s_frameStats.up_submissions;
            if(s_batchCount<GE_NV2A_MIN_VB_VERTICES)++s_frameStats.small_up_submissions;
        }
    }
    if(submitted) {
        ++s_frameStats.gpu_batches;
        s_frameStats.output_triangles+=s_batchCount/3u;
        s_drawCount+=s_batchCount/3u;
    }
    s_batchCount=0;
}

/* The exported internal name stays the same, so all existing barriers
 * retain their ordering. Only the measurement wrapper is new. */
static void flushNativeBatch(void) {
    /* Match the former early-out BEFORE reading the profiling clock. Most
     * Fast3D state barriers have no pending triangles, especially in menus. */
    if(!s_batchCount)return;
    const unsigned long long p0FlushStart=geP0BStart();
#if GE_NV2A_PHASE_PROFILE
    const uint64_t start=sysGetMicroseconds();
#endif
    flushNativeBatchImpl();
    geP0BEnd(GE_P0B_FLUSH,p0FlushStart);
#if GE_NV2A_PHASE_PROFILE
    recordNativePhase(&s_frameStats.submit_us,start);
#endif
}

static unsigned neededInputs(const CCFeatures& f,unsigned channel) {
    unsigned mask=0;
    if(channel && !f.opt_alpha)return 0;
    for(unsigned c=0;c<(f.opt_2cyc?2u:1u);++c) {
        for(unsigned j=0;j<4;++j) {
            if(j<3 && (f.c[c][channel][2]==SHADER_0 ||
                       f.c[c][channel][0]==f.c[c][channel][1]))continue;
            unsigned n=f.c[c][channel][j];
            if(n>=SHADER_INPUT_1 && n<=SHADER_INPUT_7)mask|=1u<<(n-1u);
        }
    }
    return mask;
}
static void initStableMap(ShaderProgram* p) {
    if(p->stableMapChecked)return;
    p->stableMapChecked=1;
    memset(&p->stableMap,0,sizeof(p->stableMap));
    for(unsigned ch=0;ch<2;++ch) {
        const unsigned mask=neededInputs(p->features,ch);
        const unsigned limit=(ch && p->features.opt_fog)?1u:2u;
        unsigned count=0;
        for(unsigned i=0;i<7u;++i)if(mask&(1u<<i)){
            if(count>=limit)return; // triangle-dependent mapping retained
            (ch?p->stableMap.alpha:p->stableMap.rgb)[count++]=(unsigned char)(i+1u);
        }
    }
    p->stableMapValid=1;
}
static bool chooseMap(const GESourceVertex v[3],const CCFeatures& f,GENV2AInputMap *map) {
    memset(map,0,sizeof(*map));
    for(unsigned ch=0;ch<2;++ch) {
        unsigned mask=neededInputs(f,ch),count=0;
        const unsigned max=ch && f.opt_fog?1u:2u;
        unsigned needed=0;
        for(unsigned input=0;input<7;++input)if(mask&(1u<<input))++needed;
        if(needed<=max) {
            /* Uniform triangle inputs are equally exact as packed vertex
             * colors. Keep this map stable across scanlines instead of
             * rewriting pixel-shader constants for each row's color. */
            for(unsigned input=0;input<7;++input)if(mask&(1u<<input))
                (ch?map->alpha:map->rgb)[count++]=(uint8_t)(input+1u);
            continue;
        }
        for(unsigned input=0;input<7;++input)if(mask&(1u<<input)) {
            bool varies=false;
            for(unsigned k=ch?3u:0u;k<(ch?4u:3u);++k)
                if(pack8(v[0].input[input][k])!=pack8(v[1].input[input][k]) ||
                   pack8(v[0].input[input][k])!=pack8(v[2].input[input][k]))varies=true;
            if(varies) {
                if(count>=max) {
#if GE_NV2A_ALPHA_VARYING_FALLBACK
                    if(ch) {
                        /* V0/V1 alpha are fully occupied (or V1 is fog).
                         * Leave this input unmapped: geNV2ABuild will read
                         * its vertex[0] value from a per-triangle constant.
                         * This is approximate only for the over-limit case,
                         * and does not skip the triangle or change draw order. */
                        continue;
                    }
#endif
                    backendError(ch?"Combiner needs more varying alpha channels than Xbox FVF provides":
                                    "Combiner needs more than two varying RGB inputs");
                    return false;
                }
                (ch?map->alpha:map->rgb)[count++]=(uint8_t)(input+1u);
            }
        }
    }
    return true;
}
static void triangleConstants(const GESourceVertex& v,const GENativeShader& shader,DWORD out[8]) {
    memset(out,0,8*sizeof(DWORD));
    for(unsigned stage=0;stage<shader.program.constantStages;++stage)for(unsigned slot=0;slot<2;++slot) {
        unsigned rgb=shader.program.constantInput[stage][slot][0];
        unsigned a=shader.program.constantInput[stage][slot][1];
        if(!rgb && !a)continue; /* The slot was already cleared above. */
        const float *col=rgb?v.input[rgb-1u]:0;
        out[stage*2u+slot]=packColor(col?col[0]:0,col?col[1]:0,col?col[2]:0,a?v.input[a-1u][3]:0);
    }
}

/* Only live vertices participate in any subsequent operation. Copying every
 * 16-vertex scratch slot made the small scanline quads unnecessarily costly. */
static void copyPolygon(GEPolygon *dest,const GEPolygon& source) {
    dest->count=source.count;
    if(source.count)memcpy(dest->vertex,source.vertex,source.count*sizeof(GESourceVertex));
}
static GESourceVertex interpolate(const GESourceVertex& a,const GESourceVertex& b,float t) {
    GESourceVertex o;
    o.x=a.x+(b.x-a.x)*t;o.y=a.y+(b.y-a.y)*t;
    o.z=a.z+(b.z-a.z)*t;o.rhw=a.rhw+(b.rhw-a.rhw)*t;
    for(unsigned tex=0;tex<2;++tex)for(unsigned ax=0;ax<2;++ax) {
#if GE_NV2A_GAMEPLAY_LEAN_VERTEX
        /* Preserve the old expression's add/subtract order exactly. */
        const float ar=a.uv[tex][ax]*a.rhw;
        o.uv[tex][ax]=(ar+(b.uv[tex][ax]*b.rhw-ar)*t)/o.rhw;
#else
        o.uv[tex][ax]=(a.uv[tex][ax]*a.rhw+(b.uv[tex][ax]*b.rhw-a.uv[tex][ax]*a.rhw)*t)/o.rhw;
#endif
    }
    /* These temporary vertices are only consumed by the CURRENT native
     * combiner. Unused inputs never reach the GPU and need not be interpolated. */
#if GE_NV2A_GAMEPLAY_LEAN_VERTEX
    const unsigned activeInputs=s_batchShader?s_batchShader->parent->inputs:7u;
#else
    const unsigned activeInputs=7u;
#endif
    for(unsigned i=0;i<activeInputs;++i)for(unsigned c=0;c<4;++c)
        o.input[i][c]=a.input[i][c]+(b.input[i][c]-a.input[i][c])*t;
#if GE_NV2A_GAMEPLAY_LEAN_VERTEX
    if(s_batchShader && s_batchShader->parent->fog)
#endif
        for(unsigned c=0;c<4;++c)o.fog[c]=a.fog[c]+(b.fog[c]-a.fog[c])*t;
    return o;
}
static bool addPolygon(GEPolygon *poly,const GESourceVertex& v) {
    if(poly->count>=16u){backendError("Native clipped polygon capacity exceeded");return false;}
    poly->vertex[poly->count++]=v;return true;
}
static float planeDistance(const GESourceVertex& v,unsigned kind,float bound) {
    if(kind==0)return v.x-bound;
    if(kind==1)return bound-v.x;
    if(kind==2)return v.y-bound;
    if(kind==3)return bound-v.y;
    unsigned axis=kind-4u,tex=axis/2u;
    return (v.uv[tex][axis&1u]-bound)*v.rhw;
}
static bool clipPlane(const GEPolygon& source,GEPolygon *dest,unsigned kind,float bound,bool positive) {
    dest->count=0;
    if(!source.count)return true;
    GESourceVertex a=source.vertex[source.count-1u];
    float da=planeDistance(a,kind,bound);if(!positive)da=-da;
    for(unsigned i=0;i<source.count;++i) {
        const GESourceVertex& b=source.vertex[i];
        float db=planeDistance(b,kind,bound);if(!positive)db=-db;
        /* A vertex exactly on the plane is already retained below. Do not
         * emit it again as an intersection and inflate the bounded polygon. */
        if((da>0 && db<0) || (da<0 && db>0)) {
            const float t=da/(da-db);
            if(!addPolygon(dest,interpolate(a,b,t)))return false;
        }
        if(db>=0 && !addPolygon(dest,b))return false;
        a=b;da=db;
    }
    return true;
}
static void emitVertex(const GESourceVertex& v,GEVertex *out) {
    out->x=v.x-0.5f;out->y=v.y-0.5f; /* D3D8 integer pixel-centre convention. */
    out->z=clamp01(v.z);out->rhw=v.rhw;
    DWORD colors[2];
    for(unsigned i=0;i<2;++i) {
        unsigned rgb=s_batchShader->map.rgb[i],a=s_batchShader->map.alpha[i];
        if(!rgb && !a && !(i==1 && s_batchShader->parent->fog)) {
            colors[i]=0;continue;
        }
        const float *c=rgb?v.input[rgb-1u]:0;
        float alpha=a?v.input[a-1u][3]:0;
        if(i==1 && s_batchShader->parent->fog)alpha=1.0f-clamp01(v.fog[3]);
        colors[i]=packColor(c?c[0]:0,c?c[1]:0,c?c[2]:0,alpha);
    }
    out->color=colors[0];out->specular=colors[1];
    float texcoords[2][2];
    for(unsigned tex=0;tex<2;++tex)for(unsigned ax=0;ax<2;++ax) {
        float u=v.uv[tex][ax];
#if GE_NV2A_GAMEPLAY_LEAN_VERTEX
        texcoords[tex][ax]=s_uvStorage[tex][ax] ?
            (u*s_uvLogical[tex][ax]+s_uvGutter[tex][ax])*s_uvInvStorage[tex][ax] : 0.0f;
#else
        texcoords[tex][ax]=s_uvStorage[tex][ax] ?
            (u*s_uvLogical[tex][ax]+s_uvGutter[tex][ax])/s_uvStorage[tex][ax] : 0.0f;
#endif
    }
    out->u0=texcoords[0][0];out->v0=texcoords[0][1];
    out->u1=texcoords[1][0];out->v1=texcoords[1][1];
}
static bool appendVertex(const GESourceVertex& v) {
    if(!s_batchShader->program.usesNoise) {
        emitVertex(v,&s_vertices.regular[s_batchCount++]);return true;
    }
    GENoiseVertex* out=&s_vertices.noisy[s_batchCount];
    emitVertex(v,&out->vertex);
    const float height=(float)(s_currentFB?s_fb[s_currentFB].height:s_height);
    if(!(v.rhw>0.0f && v.rhw<=FLT_MAX)) {
        backendError("Xbox alpha-noise projection has invalid reciprocal W");return false;
    }
    const float q=1.0f/v.rhw;
    /* Correct for perspective interpolation, then let PROJECT2D divide by
     * q. The result is affine screen-space noise, including on 3D models.
     * v.x/y precede the D3D half-pixel correction; point filtering therefore
     * samples floor(gl_FragCoord * noiseScale), with GL's bottom origin. */
    const float u=(v.x*s_noiseScale+(float)s_noiseOffsetX)/(float)GE_NOISE_SIZE;
    const float t=((height-v.y)*s_noiseScale+(float)s_noiseOffsetY)/(float)GE_NOISE_SIZE;
    out->u2=u*q;out->v2=t*q;out->z2=0.0f;out->q2=q;
    if(!(q>0.0f && q<=FLT_MAX && out->u2>=-FLT_MAX && out->u2<=FLT_MAX &&
         out->v2>=-FLT_MAX && out->v2<=FLT_MAX)) {
        backendError("Xbox alpha-noise projection is nonfinite");return false;
    }
    ++s_batchCount;return true;
}
static void emitPolygon(const GEPolygon& poly) {
    if(poly.count<3)return;
    for(unsigned i=1;i+1<poly.count;++i) {
        if(s_batchCount+3u>GE_MAX_VERTICES)flushNativeBatch();
        if(GENativePlatform_HasFatalError())return;
        if(!appendVertex(poly.vertex[0]) || !appendVertex(poly.vertex[i]) ||
           !appendVertex(poly.vertex[i+1]))return;
    }
}
static void addressPolygon(const GEPolygon& poly,unsigned axis);
static void mirrorAddressPolygon(const GEPolygon& poly,unsigned axis) {
    if(poly.count<3)return;
    const unsigned tex=axis/2u,ax=axis&1u,kind=4u+axis;
    const GETexture& t=s_textures[s_selectedTex[tex]];
    const float minimum=0.5f/(float)(ax?t.height:t.width),maximum=1.0f-minimum;
    GEAddressScratch& scratch=s_addressScratch[axis];
    float low=poly.vertex[0].uv[tex][ax],high=low;
    for(unsigned v=1;v<poly.count;++v) {
        float u=poly.vertex[v].uv[tex][ax];
        if(u<low)low=u;
        if(u>high)high=u;
    }
    /* A whole polygon on a clamp plane belongs to one region only. Sending
     * it to both inclusive clip halfspaces would blend the same pixels twice. */
    if(high<=minimum || low>=maximum) {
        const float edge=high<=minimum?minimum:maximum;
        copyPolygon(&scratch.edgeLow,poly);
        for(unsigned n=0;n<scratch.edgeLow.count;++n)scratch.edgeLow.vertex[n].uv[tex][ax]=edge;
        addressPolygon(scratch.edgeLow,axis+1u);return;
    }
    if(low>=minimum && high<=maximum){addressPolygon(poly,axis+1u);return;}
    /* The stored gutter repeats. Mirroring instead repeats the edge texel;
     * split before clamping so interpolation inside the image is unchanged. */
    if(!clipPlane(poly,&scratch.edgeLow,kind,minimum,false) ||
       !clipPlane(poly,&scratch.edgeMiddle,kind,minimum,true))return;
    for(unsigned i=0;i<scratch.edgeLow.count;++i)scratch.edgeLow.vertex[i].uv[tex][ax]=minimum;
    addressPolygon(scratch.edgeLow,axis+1u);
    if(!clipPlane(scratch.edgeMiddle,&scratch.edgeHigh,kind,maximum,true) ||
       !clipPlane(scratch.edgeMiddle,&scratch.edgeLow,kind,maximum,false))return;
    for(unsigned j=0;j<scratch.edgeHigh.count;++j)scratch.edgeHigh.vertex[j].uv[tex][ax]=maximum;
    addressPolygon(scratch.edgeLow,axis+1u);
    addressPolygon(scratch.edgeHigh,axis+1u);
}
static void addressPolygon(const GEPolygon& poly,unsigned axis) {
    if(poly.count<3 || GENativePlatform_HasFatalError())return;
    if(axis==4u) {
        if(++s_addressPieces>4096u){backendError("Native texture address subdivision limit exceeded");return;}
        emitPolygon(poly);return;
    }
    const unsigned tex=axis/2u,ax=axis&1u;
    const GETexture& t=s_textures[s_selectedTex[tex]];
    const unsigned size=ax?t.height:t.width,storage=ax?t.storageHeight:t.storageWidth;
    const unsigned mode=s_addressMode[tex][ax];
    if(!s_batchShader->parent->useTex[tex] || size==storage || (mode&2u)) {
        addressPolygon(poly,axis+1u);return;
    }
    /* A padded axis cannot repeat over the physical POT period. Split in
     * logical UV space using the same perspective-correct clipping as tiles,
     * then fold each piece into one source-image period. Never fmod only the
     * vertices of an unsplit triangle: that would interpolate across seams. */
    float minimum=poly.vertex[0].uv[tex][ax],maximum=minimum;
    for(unsigned i=1;i<poly.count;++i) {
        float u=poly.vertex[i].uv[tex][ax];
        if(u<minimum)minimum=u;
        if(u>maximum)maximum=u;
    }
    if(!(minimum>=-65536.0f && maximum<=65536.0f)) {
        backendError("Native texture coordinate exceeds addressing bounds");return;
    }
    int first=(int)minimum,last=(int)maximum;
    if((float)first>minimum)--first;
    if((float)last>maximum)--last;
    if(maximum==(float)last && last>first)--last;
    if(last-first>512){backendError("Native texture repeats exceed subdivision limit");return;}
    GEAddressScratch& scratch=s_addressScratch[axis];
    copyPolygon(&scratch.remaining,poly);
    for(int cell=first;cell<=last && scratch.remaining.count>=3;++cell) {
        if(cell==last)copyPolygon(&scratch.piece,scratch.remaining);
        else {
            const float boundary=(float)(cell+1);
            if(!clipPlane(scratch.remaining,&scratch.piece,4u+axis,boundary,false) ||
               !clipPlane(scratch.remaining,&scratch.above,4u+axis,boundary,true))return;
            copyPolygon(&scratch.remaining,scratch.above);
        }
        for(unsigned n=0;n<scratch.piece.count;++n) {
            float u=scratch.piece.vertex[n].uv[tex][ax]-(float)cell;
            if((mode&1u) && (cell%2!=0))u=1.0f-u;
            scratch.piece.vertex[n].uv[tex][ax]=u;
        }
        if(mode&1u)mirrorAddressPolygon(scratch.piece,axis);
        else addressPolygon(scratch.piece,axis+1u);
        if(GENativePlatform_HasFatalError())return;
    }
}
static void trimPolygon(const GEPolygon& poly,unsigned plane) {
    if(poly.count<3 || GENativePlatform_HasFatalError())return;
    if(plane==s_trimCount) {
        copyPolygon(&s_addressInput,poly);
        /* Fast3D shader clamp comes BEFORE texture addressing. */
        for(unsigned n=0;n<s_addressInput.count;++n)
            for(unsigned t=0;t<2;++t)for(unsigned ax=0;ax<2;++ax)if(s_trimEnabled[t][ax]) {
                float& u=s_addressInput.vertex[n].uv[t][ax];
                if(u<s_trimMin[t][ax])u=s_trimMin[t][ax];
                if(u>s_trimMax[t][ax])u=s_trimMax[t][ax];
            }
        addressPolygon(s_addressInput,0);return;
    }
    GESplitScratch& scratch=s_splitScratch[plane];
    unsigned kind=4u+s_trimTexture[plane]*2u+s_trimAxis[plane];
    float bound=s_trimBound[plane];
    bool positive=false,negative=false;
    for(unsigned i=0;i<poly.count;++i) {
        float d=planeDistance(poly.vertex[i],kind,bound);
        if(d>0)positive=true;else if(d<0)negative=true;
    }
    if(!positive || !negative){trimPolygon(poly,plane+1u);return;}
    if(!clipPlane(poly,&scratch.below,kind,bound,false) ||
       !clipPlane(poly,&scratch.above,kind,bound,true))return;
    trimPolygon(scratch.below,plane+1u);trimPolygon(scratch.above,plane+1u);
}

static void api_drawImpl(float vbo[],size_t nFloats,size_t nTriangles){
    if(GENativePlatform_HasFatalError())return;
    if(!s_device||!vbo||!nFloats||!nTriangles||!s_shader){backendError("Incomplete native draw submission");return;}
    ShaderProgram* p=s_shader;
    unsigned stride=4;
    bool softwareAddress=false;
    for(unsigned t=0;t<2;++t)if(p->useTex[t]) {
        stride+=2;
        for(unsigned j=0;j<2;++j)if(p->clamps[t][j])++stride;
        unsigned id=s_selectedTex[t];
        if(id>=s_textureCount || !s_textures[id].live || !s_textures[id].tex) {
            backendError("Native draw references an unavailable texture");return;
        }
        const GETexture& image=s_textures[id];
        if((image.width!=image.storageWidth && !(s_addressMode[t][0]&2u)) ||
           (image.height!=image.storageHeight && !(s_addressMode[t][1]&2u)))
            softwareAddress=true;
    }
    if(p->fog)stride+=4;
    if(p->grayscale)stride+=4;
    stride+=p->inputs*(p->alpha?4u:3u);
    if(p->inputs>7 || nTriangles>GE_MAX_VERTICES/3u || nFloats!=3u*nTriangles*stride) {
        backendError("Invalid original Fast3D vertex layout");return;
    }
    ++s_frameStats.draw_calls;s_frameStats.input_triangles+=(unsigned)nTriangles;
    /* Texture dimensions remain constant during a Fast3D draw, even if the
     * CPU clips and splits many output triangles. Keep the exact original
     * floating-point arithmetic and division order for UV bit parity. */
    for(unsigned tex=0;tex<2;++tex)for(unsigned ax=0;ax<2;++ax) {
        const GETexture& image=s_textures[s_selectedTex[tex]];
        const unsigned logical=ax?image.height:image.width;
        const unsigned storage=ax?image.storageHeight:image.storageWidth;
        s_uvLogical[tex][ax]=(float)logical;
        s_uvStorage[tex][ax]=(float)storage;
        s_uvGutter[tex][ax]=(logical==storage)?0.0f:1.0f;
        if(p->useTex[tex] && stripAtlasSelected(tex,s_selectedTex[tex])) {
            if(ax==0u) {
                /* Horizontal padding is the same as legacy 440->512. */
                s_uvStorage[tex][ax]=(float)GE_STRIP_ATLAS_W;
            } else {
                /* Three vertical copies of the same strip isolate bilinear
                 * sampling from neighboring differently tinted rows. */
                s_uvStorage[tex][ax]=(float)GE_STRIP_ATLAS_H;
                s_uvGutter[tex][ax]=(float)((s_textures[s_selectedTex[tex]].stripSlot-1u)*3u+1u);
            }
        }
    }
#if GE_NV2A_GAMEPLAY_LEAN_VERTEX
    for(unsigned tex=0;tex<2;++tex)for(unsigned ax=0;ax<2;++ax)
        s_uvInvStorage[tex][ax]=s_uvStorage[tex][ax] ?
            1.0f/s_uvStorage[tex][ax] : 0.0f;
#endif
    /* The viewport/scissor rectangle and padded sampler clamp geometry are
     * immutable throughout this draw call.  Do not recalculate them for
     * every triangle: a dense level can submit thousands in one call.  Keep
     * per-vertex shader clamp bounds below (those can vary per draw). */
    const float clipBounds[4]={
        s_scissor?(float)s_scX:0.0f,
        s_scissor?(float)(s_scX+s_scW):(float)s_width,
        s_scissor?(float)s_scY:0.0f,
        s_scissor?(float)(s_scY+s_scH):(float)s_height
    };
    /* Only use the position-first path when no per-vertex clamp bounds
     * require validation. The existing later cull remains authoritative for
     * every other material and for the A/B-disabled configuration. */
#if GE_NV2A_POSITION_FIRST_CULL && GE_NV2A_EARLY_OFFSCREEN_REJECT
    bool hasVertexClamp=false;
    for(unsigned tex=0;tex<2;++tex)if(p->useTex[tex])
        for(unsigned ax=0;ax<2;++ax)
            if(p->clamps[tex][ax])hasVertexClamp=true;
    const bool positionFirst=!hasVertexClamp;
#endif
    unsigned paddedTrim[2][2]={{0,0},{0,0}};
    float paddedTrimMin[2][2],paddedTrimMax[2][2];
    bool hasPaddedTrim=false;
    for(unsigned tex=0;tex<2;++tex)if(p->useTex[tex])for(unsigned ax=0;ax<2;++ax) {
        const GETexture& image=s_textures[s_selectedTex[tex]];
        const unsigned size=ax?image.height:image.width;
        const unsigned storage=ax?image.storageHeight:image.storageWidth;
        if(size!=storage && (s_addressMode[tex][ax]&2u)) {
            hasPaddedTrim=true;paddedTrim[tex][ax]=1;
            paddedTrimMin[tex][ax]=0.5f/(float)size;
            paddedTrimMax[tex][ax]=1.0f-paddedTrimMin[tex][ax];
        }
    }
    /* The stable map covers the common 1/2-input geometry shaders; complex
     * combiner programs keep their per-triangle mapping unchanged. */
    initStableMap(p);
#if GE_NV2A_SOURCE_VERTEX_DECODE_CACHE
    /* Clamp-enabled materials validate each vertex's tile bound against the
     * first vertex; those side effects MUST execute even for duplicate data. */
    bool decodeCacheEligible=nTriangles>=3u && stride>=14u && stride<=48u;
    for(unsigned tex=0;tex<2u;++tex)if(p->useTex[tex])
        for(unsigned axis=0;axis<2u;++axis)
            if(p->clamps[tex][axis])decodeCacheEligible=false;
    if(++s_vertexDecodeEpoch==0u) {
        memset(s_vertexDecodeEntries,0,sizeof(s_vertexDecodeEntries));
        s_vertexDecodeEpoch=1u;
    }
#endif
#if GE_NV2A_STABLE_DRAW_SHADER_REUSE
    GENativeShader *stableDrawShader=0;
#endif
    for(size_t tri=0;tri<nTriangles;++tri) {
        GESourceVertex vertex[3];
#if !GE_NV2A_GAMEPLAY_LEAN_VERTEX
        memset(vertex,0,sizeof(vertex));
#endif
        s_trimCount=0;s_addressPieces=0;
        memset(s_trimEnabled,0,sizeof(s_trimEnabled));
#if GE_NV2A_POSITION_FIRST_CULL && GE_NV2A_EARLY_OFFSCREEN_REJECT
        unsigned projectedCount=0;
        if(positionFirst) {
            /* Compute source vertex zero exactly once. If it lies inside
             * every inclusive clip halfspace, the triangle cannot be fully
             * outside, so decode vertices one and two normally below. */
            const float *v=vbo+tri*3u*stride;
            GESourceVertex& o=vertex[0];
            if(!(v[3]>0.0000001f)){backendError("Native draw has an unclipped nonpositive W");return;}
            o.rhw=1.0f/v[3];
            o.x=(float)s_viewport.X+(v[0]*o.rhw*.5f+.5f)*(float)s_viewport.Width;
            o.y=(float)s_viewport.Y+(.5f-v[1]*o.rhw*.5f)*(float)s_viewport.Height;
            projectedCount=1;
            bool firstInside=true;
            for(unsigned plane=0;plane<4u;++plane) {
                if(!(planeDistance(o,plane,clipBounds[plane])>=0.0f)) {
                    firstInside=false;break;
                }
            }
            if(!firstInside) {
                for(unsigned n=1;n<3;++n) {
                    const float *source=vbo+(tri*3u+n)*stride;
                    GESourceVertex& projected=vertex[n];
                    if(!(source[3]>0.0000001f)){backendError("Native draw has an unclipped nonpositive W");return;}
                    projected.rhw=1.0f/source[3];
                    projected.x=(float)s_viewport.X+(source[0]*projected.rhw*.5f+.5f)*(float)s_viewport.Width;
                    projected.y=(float)s_viewport.Y+(.5f-source[1]*projected.rhw*.5f)*(float)s_viewport.Height;
                }
                projectedCount=3;
                /* Keep the original four-plane classification identical. */
                bool outside=false;
                for(unsigned plane=0;plane<4u;++plane) {
                    bool allOutside=true;
                    for(unsigned n=0;n<3u;++n)
                        if(planeDistance(vertex[n],plane,clipBounds[plane])>=0.0f) {
                            allOutside=false;break;
                        }
                    if(allOutside){outside=true;break;}
                }
                if(outside){++s_frameStats.screen_rejects;continue;}
            }
        }
#endif
        if(hasPaddedTrim)for(unsigned tex=0;tex<2;++tex)for(unsigned ax=0;ax<2;++ax)
            if(paddedTrim[tex][ax]) {
                s_trimEnabled[tex][ax]=1;
                s_trimMin[tex][ax]=paddedTrimMin[tex][ax];
                s_trimMax[tex][ax]=paddedTrimMax[tex][ax];
            }
        for(unsigned n=0;n<3;++n) {
            const float *v=vbo+(tri*3u+n)*stride;
            GESourceVertex& o=vertex[n];
#if GE_NV2A_SOURCE_VERTEX_DECODE_CACHE
            GEVertexDecodeEntry *cacheEntry=0;
            if(decodeCacheEligible) {
                cacheEntry=&s_vertexDecodeEntries[geVertexDecodeSlot(v)];
                if(cacheEntry->epoch==s_vertexDecodeEpoch &&
                   memcmp(cacheEntry->raw,v,stride*sizeof(float))==0) {
                    /* Only memcpy raw decoded values: no extra arithmetic,
                     * interpolation or changed precision on the hit path. */
                    memcpy(&o,&cacheEntry->decoded,sizeof(o));
                    continue;
                }
            }
#endif
#if GE_NV2A_POSITION_FIRST_CULL && GE_NV2A_EARLY_OFFSCREEN_REJECT
            if(!positionFirst || n>=projectedCount) {
#endif
                if(!(v[3]>0.0000001f)){backendError("Native draw has an unclipped nonpositive W");return;}
                o.rhw=1.0f/v[3];
                o.x=(float)s_viewport.X+(v[0]*o.rhw*.5f+.5f)*(float)s_viewport.Width;
                o.y=(float)s_viewport.Y+(.5f-v[1]*o.rhw*.5f)*(float)s_viewport.Height;
#if GE_NV2A_POSITION_FIRST_CULL && GE_NV2A_EARLY_OFFSCREEN_REJECT
            }
#endif
            o.z=(v[2]*o.rhw*.5f+.5f)*(s_zFar-s_zNear)+s_zNear;
            unsigned k=4;
            for(unsigned t=0;t<2;++t)if(p->useTex[t]) {
                o.uv[t][0]=v[k++];o.uv[t][1]=v[k++];
                for(unsigned ax=0;ax<2;++ax)if(p->clamps[t][ax]) {
                    float maximum=v[k++];
                    unsigned size=ax?s_textures[s_selectedTex[t]].height:s_textures[s_selectedTex[t]].width;
                    float minimum=0.5f/(float)size;
                    if(maximum<minimum)maximum=minimum;
                    /* Combine shader clamp with a padded image's clamp edge.
                     * Both are monotonic clamps in original logical UVs. */
                    unsigned storage=ax?s_textures[s_selectedTex[t]].storageHeight:s_textures[s_selectedTex[t]].storageWidth;
                    if(size!=storage && (s_addressMode[t][ax]&2u) && maximum>1.0f-minimum)
                        maximum=1.0f-minimum;
                    if(n==0) {
                        s_trimMin[t][ax]=minimum;s_trimMax[t][ax]=maximum;
                        /* A full-image clamp on an unpadded, hardware-CLAMP
                         * sampler is identical to RDP's edge-texel clamp at
                         * sampling time. Avoid splitting all intersecting
                         * geometry into edge/middle polygons. */
                        const bool nativeFullClamp=GE_NV2A_HW_TEXCLAMP &&
                            size==storage && (s_addressMode[t][ax]&2u)!=0 &&
                            maximum>=1.0f-minimum;
                        s_trimEnabled[t][ax]=nativeFullClamp?0u:1u;
                    } else if(maximum!=s_trimMax[t][ax]) {
                        backendError("Varying tile clamp bounds are unsupported");return;
                    }
                }
            }
#if GE_NV2A_GAMEPLAY_LEAN_VERTEX
            /* The clipper still walks both UV pairs; absent textures are
             * explicitly zeroed without touching the seven-input color slab. */
            for(unsigned t=0;t<2;++t)if(!p->useTex[t]) {
                o.uv[t][0]=0.0f;o.uv[t][1]=0.0f;
            }
#endif
            if(p->fog)for(unsigned c=0;c<4;++c)o.fog[c]=v[k++];
#if GE_NV2A_GAMEPLAY_LEAN_VERTEX
            else for(unsigned c=0;c<4;++c)o.fog[c]=0.0f;
#endif
            if(p->grayscale)k+=4;
            for(unsigned input=0;input<p->inputs;++input) {
                for(unsigned c=0;c<3;++c)o.input[input][c]=clamp01(v[k++]);
                o.input[input][3]=p->alpha?clamp01(v[k++]):1.0f;
            }
#if GE_NV2A_SOURCE_VERTEX_DECODE_CACHE
            if(cacheEntry) {
                memcpy(cacheEntry->raw,v,stride*sizeof(float));
                memcpy(&cacheEntry->decoded,&o,sizeof(o));
                cacheEntry->epoch=s_vertexDecodeEpoch; /* publish last */
            }
#endif
        }
        for(unsigned texture=0;texture<2;++texture)for(unsigned axis=0;axis<2;++axis)
            if(s_trimEnabled[texture][axis]) {
                unsigned plane=s_trimCount++;
                s_trimTexture[plane]=texture;s_trimAxis[plane]=axis;s_trimBound[plane]=s_trimMin[texture][axis];
                plane=s_trimCount++;
                s_trimTexture[plane]=texture;s_trimAxis[plane]=axis;s_trimBound[plane]=s_trimMax[texture][axis];
            }
        /* Hardware clipping needs no pixel-shader setup for a triangle
         * trivially rejected by the viewport/scissor, and skipping this
         * work cannot affect game state or RNG. */
        bool triviallyOutside=false;
#if GE_NV2A_POSITION_FIRST_CULL && GE_NV2A_EARLY_OFFSCREEN_REJECT
        if(!positionFirst)
#endif
        for(unsigned plane=0;plane<4u;++plane){
            bool allOutside=true;
            for(unsigned n=0;n<3u;++n)
                if(planeDistance(vertex[n],plane,clipBounds[plane])>=0.0f){
                    allOutside=false;break;
                }
            if(allOutside){triviallyOutside=true;break;}
        }
#if GE_NV2A_EARLY_OFFSCREEN_REJECT
        /* The triangle cannot contribute any pixel when all three decoded
         * vertices are outside the SAME inclusive screen/scissor halfspace.
         * Rejecting it before combiner lookup avoids shader/cache work,
         * without touching a crossing triangle, the caller's state barriers,
         * or the original clipping math. */
        if(triviallyOutside){++s_frameStats.screen_rejects;continue;}
#endif
        GENV2AInputMap map;
        if(s_decalDepth) {
            const float dx1=vertex[1].x-vertex[0].x,dy1=vertex[1].y-vertex[0].y;
            const float dx2=vertex[2].x-vertex[0].x,dy2=vertex[2].y-vertex[0].y;
            const float det=dx1*dy2-dx2*dy1;
            if(det!=0.0f) {
                const float dz1=vertex[1].z-vertex[0].z,dz2=vertex[2].z-vertex[0].z;
                float sx=(dz1*dy2-dz2*dy1)/det,sy=(dx1*dz2-dx2*dz1)/det;
                if(sx<0)sx=-sx;
                if(sy<0)sy=-sy;
                const float bias=-2.0f*(sx>sy?sx:sy)-2.0f/16777215.0f;
                for(unsigned n=0;n<3;++n)vertex[n].z+=bias;
            }
        }
        if(p->stableMapValid)map=p->stableMap;
        else if(!chooseMap(vertex,p->features,&map))return;
        GENativeShader *shader=0;
#if GE_NV2A_STABLE_DRAW_SHADER_REUSE
        if(p->stableMapValid) {
            if(!stableDrawShader)stableDrawShader=nativeShader(p,map);
            shader=stableDrawShader;
        } else
#endif
            shader=nativeShader(p,map);
        if(!shader)return;
        DWORD constants[8];
        const DWORD *triangleConstantsPtr=constants;
#if GE_NV2A_SKIP_EMPTY_TRI_CONSTANTS
        if(!shader->hasTriangleConstants)
            triangleConstantsPtr=s_noTriangleConstants;
        else
#endif
            triangleConstants(vertex[0],*shader,constants);
#if !GE_NV2A_EARLY_OFFSCREEN_REJECT
        if(triviallyOutside){++s_frameStats.screen_rejects;continue;}
#endif
        DWORD fog;
#if GE_NV2A_STATIC_NOFOG_COLOR
        if(!p->fog)
            fog=0xFF000000u; /* exactly packColor(0,0,0,1) */
        else
#endif
            fog=packColor(vertex[0].fog[0],vertex[0].fog[1],vertex[0].fog[2],1);
        if(p->fog)for(unsigned n=1;n<3;++n)
            if(fog!=packColor(vertex[n].fog[0],vertex[n].fog[1],vertex[n].fog[2],1)) {
                backendError("Varying fog colour cannot use the native fog register");return;
            }
        if(s_batchShader!=shader || s_batchFog!=fog ||
           memcmp(triangleConstantsPtr,s_batchConstants,sizeof(constants))) {
            flushNativeBatch();
            s_batchShader=shader;s_batchFog=fog;
            memcpy(s_batchConstants,triangleConstantsPtr,sizeof(constants));
        }
        /* Most gameplay triangles are entirely inside the viewport. Avoid
         * copying them through the 16-slot clipping polygon or testing four
         * separate planes when no software tile addressing is needed.
         * Per-vertex checks retain exactly the same inclusive boundaries. */
        if(softwareAddress)++s_frameStats.software_address_triangles;
        /* A clamp plane needs software subdivision only if some triangle UV
         * actually crosses it. If all 3 inputs are already within the N64
         * texel-centre limits, clipping/clamping would be a pure identity. */
        bool uvInside=true;
        if(s_trimCount && !softwareAddress) {
            for(unsigned tex=0;tex<2 && uvInside;++tex)
                for(unsigned ax=0;ax<2 && uvInside;++ax)if(s_trimEnabled[tex][ax])
                    for(unsigned n=0;n<3;++n) {
                        const float u=vertex[n].uv[tex][ax];
                        if(u<s_trimMin[tex][ax] || u>s_trimMax[tex][ax]) {
                            uvInside=false;break;
                        }
                    }
        }
        if(!softwareAddress && (!s_trimCount || uvInside)) {
            bool fullyInside=s_hwScissorActive!=0;
            if(!fullyInside) {
                fullyInside=true;
                for(unsigned n=0;n<3;++n) {
                    const GESourceVertex& v=vertex[n];
                    if(v.x<clipBounds[0] || v.x>clipBounds[1] ||
                       v.y<clipBounds[2] || v.y>clipBounds[3]) {
                        fullyInside=false;break;
                    }
                }
            }
            if(fullyInside) {
                ++s_addressPieces;
                if(s_batchCount+3u>GE_MAX_VERTICES)flushNativeBatch();
                if(GENativePlatform_HasFatalError())return;
                if(!appendVertex(vertex[0]) || !appendVertex(vertex[1]) ||
                   !appendVertex(vertex[2]))return;
                continue;
            }
        }
        GEPolygon a,b;a.count=3;for(unsigned n=0;n<3;++n)a.vertex[n]=vertex[n];
        /* NV2A clips screen pixels using SetScissors; clipping source
         * polygons again here costs CPU and multiplies geometry. Preserve
         * the full original Sutherland-Hodgman path as an A/B fallback. */
        if(!s_hwScissorActive)for(unsigned plane=0;plane<4 && a.count;++plane) {
            bool inside=false,outside=false;
            for(unsigned n=0;n<a.count;++n) {
                if(planeDistance(a.vertex[n],plane,clipBounds[plane])>=0.0f)inside=true;
                else outside=true;
            }
            if(!inside){a.count=0;break;}
            if(!outside)continue;
            if(!clipPlane(a,&b,plane,clipBounds[plane],true))return;
            copyPolygon(&a,b);
        }
        /* POT wrap/mirror/clamp is already handled by the sampler. With no
         * software clamp planes, the clipped polygon can use the unchanged
         * vertex emitter directly instead of another 480-byte triangle copy
         * and the four-axis addressing walk. Padded repeat/mirror and every
         * shader clamp retain their existing subdivision path. */
        if(!s_trimCount && !softwareAddress) {
            if(a.count>=3)++s_addressPieces;
            emitPolygon(a);
        } else trimPolygon(a,0);
        if(GENativePlatform_HasFatalError())return;
    }
    /* End-of-call is NOT a GPU ordering barrier: state-changing API
     * entry points and api_end() drain the queued vertices instead. */
}
static void api_draw(float vbo[],size_t nFloats,size_t nTriangles) {
    const unsigned long long p0DrawStart=geP0BStart();
#if GE_NV2A_PHASE_PROFILE
    const uint64_t start=sysGetMicroseconds();
#endif
    api_drawImpl(vbo,nFloats,nTriangles);
    geP0BEnd(GE_P0B_DRAW,p0DrawStart);
#if GE_NV2A_PHASE_PROFILE
    recordNativePhase(&s_frameStats.draw_api_us,start);
#endif
}
static void api_init(void){
    if(!s_device)return;
    gfx_rdp_affine=1;gfx_fog_vertex=1;
    s_device->SetViewport(&s_viewport);
    s_device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    s_device->SetRenderState(D3DRS_LIGHTING,FALSE);
    s_device->SetRenderState(D3DRS_ZENABLE,D3DZB_TRUE);
    s_device->SetTextureStageState(0,D3DTSS_TEXCOORDINDEX,0);
    s_device->SetTextureStageState(1,D3DTSS_TEXCOORDINDEX,1);
    s_device->SetTextureStageState(2,D3DTSS_COLOROP,D3DTOP_DISABLE);
    applyHardwareScissor();
}
static void api_resize(void){nativeBatchBarrier();if(s_device){s_device->SetViewport(&s_viewport);applyHardwareScissor();}}
static void api_begin(void){
    nativeBatchBarrier();
    ++s_roomPrepareFrame;
    s_roomPacketBuildsFrame=0;
    s_roomGPUFrameTris=0;
    s_streamFrameSlot=(s_streamFrameSlot+1u)%GE_STREAM_FRAME_COUNT;
    s_streamOffset[0][s_streamFrameSlot]=0;
    s_streamOffset[1][s_streamFrameSlot]=0;
    s_batchCount=0;s_batchShader=0;
    s_drawCount=0;s_depthClearPending=1;invalidateDeviceState();
    applyHardwareScissor();
    memset(&s_frameStats,0,sizeof(s_frameStats));
    const uint32_t frame=noiseHash(++s_noiseFrame);
    s_noiseOffsetX=frame&(GE_NOISE_SIZE-1u);
    s_noiseOffsetY=(frame>>8)&(GE_NOISE_SIZE-1u);
}
static void api_end(void){
    nativeBatchBarrier();
    /* All the first-frame 1-pixel strips have been emitted using their
     * independent textures; atlas allocation happens at a safe boundary. */
    stripBuildAtlas();
    s_batchShader=0;
    if(s_device && s_batchState.noiseEnabled)s_device->SetTexture(2,0);
    s_batchState.noiseEnabled=0;s_batchState.noiseValid=0;
}
static void api_finish(void){/* Present belongs to Xbox window manager. */}
static int api_create_fb(void){
    if(s_fbCount>=GE_MAX_FRAMEBUFFERS){++s_unsupported;return -1;}
    const unsigned id=s_fbCount++;
    memset(&s_fb[id],0,sizeof(GEFramebuffer));
    return (int)id;
}
static void api_fb_params(int id,unsigned width,unsigned height,unsigned msaa,
                          bool flip,bool target,bool depth,bool extract){
    nativeBatchBarrier();
    if(id==0 || id<0 || id>=(int)s_fbCount)return;
    GEFramebuffer* f=&s_fb[id];
    if(!width || !height || width>2048u || height>2048u){++s_unsupported;return;}
    if(f->initialized && f->width==width && f->height==height)return;
    if(f->depth){f->depth->Release();f->depth=0;}
    if(f->surface){f->surface->Release();f->surface=0;}
    if(f->texture){f->texture->Release();f->texture=0;}
    f->initialized=0;
    if(!s_device)return;
    if(FAILED(s_device->CreateTexture(width,height,1,D3DUSAGE_RENDERTARGET,
        D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&f->texture))||!f->texture){++s_unsupported;return;}
    if(FAILED(f->texture->GetSurfaceLevel(0,&f->surface))||!f->surface){++s_unsupported;return;}
    if(depth && FAILED(s_device->CreateDepthStencilSurface(width,height,D3DFMT_D24S8,
                    D3DMULTISAMPLE_NONE,&f->depth))){++s_unsupported;return;}
    f->width=width;f->height=height;f->initialized=1;
    if(msaa>1 || flip || !target || extract)++s_unsupported;
}
static bool api_fb_start(int id,float noise){
    if(!s_device || id<0 || id>=(int)s_fbCount)return false;
    nativeBatchBarrier();
    IDirect3DSurface8* color=0;
    IDirect3DSurface8* depth=0;
    if(id==0){color=s_originalRT;depth=s_originalDepth;}
    else {
        if(!s_fb[id].initialized)return false;
        color=s_fb[id].surface;depth=s_fb[id].depth;
    }
    if(!color || FAILED(s_device->SetRenderTarget(color,depth))){backendError("Xbox render target binding failed");return false;}
    s_currentFB=id;
    if(s_depthClearPending) {
        s_depthClearPending=0;
        if(depth) {
            D3DVIEWPORT8 full={0,0,id?s_fb[id].width:s_width,id?s_fb[id].height:s_height,0.0f,1.0f};
            s_device->SetViewport(&full);
            fullHardwareScissor();
            if(FAILED(s_device->Clear(0,0,D3DCLEAR_ZBUFFER,0,1.0f,0)))
                backendError("Xbox frame depth clear failed");
            s_device->SetViewport(&s_viewport);
        }
    }
    applyHardwareScissor();
    /* The PC backend retains its scale when a zero argument is supplied. */
    if(noise>0.0f)s_noiseScale=1.0f/noise;
    return true;
}
static void api_fb_copy(int dest,int src,int left,int top,bool flip,bool back){
    nativeBatchBarrier();
    if(!s_device || dest<0 || dest>=(int)s_fbCount || src<0 || src>=(int)s_fbCount){++s_unsupported;return;}
    IDirect3DSurface8* sd=src==0?s_originalRT:s_fb[src].surface;
    IDirect3DSurface8* dd=dest==0?s_originalRT:s_fb[dest].surface;
    if(!sd||!dd || FAILED(s_device->CopyRects(sd,0,0,dd,0))) ++s_unsupported;
    if(left||top||flip||back)++s_unsupported;
}
static void api_fb_clear(bool color,bool depth){
    nativeBatchBarrier();
    if(!s_device)return;
    DWORD bits=0;
    if(color)bits|=D3DCLEAR_TARGET;
    if(depth)bits|=D3DCLEAR_ZBUFFER;
    if(bits) {
        const unsigned width=s_currentFB?s_fb[s_currentFB].width:s_width;
        const unsigned height=s_currentFB?s_fb[s_currentFB].height:s_height;
        D3DVIEWPORT8 full={0,0,width,height,0.0f,1.0f};
        s_device->SetViewport(&full);
        fullHardwareScissor();
        if(FAILED(s_device->Clear(0,0,bits,0xFF000000,1.0f,0)))
            backendError("Xbox framebuffer clear failed");
        s_device->SetViewport(&s_viewport);
        applyHardwareScissor();
    }
}
static void api_fb_resolve(int dest,int src){api_fb_copy(dest,src,0,0,false,false);}
static void* api_fb_tex(int id){
    nativeBatchBarrier();
    if(id>0 && id<(int)s_fbCount && s_fb[id].initialized)return s_fb[id].texture;
    ++s_unsupported;return 0;
}
static void api_fb_select(int id){
    nativeBatchBarrier();
    if(!s_device || id<=0 || id>=(int)s_fbCount || !s_fb[id].initialized){++s_unsupported;return;}
    s_device->SetTexture(0,s_fb[id].texture);
    s_boundGameTexture[0]=s_fb[id].texture;
    s_boundGameTextureValid[0]=1;
}
static void api_tex_delete(unsigned id){
    nativeBatchBarrier();
    /* SetTexture owns a device reference. Drop both bindings before releasing
     * a stage's cache entry so its allocation leaves the texture budget. */
    if(id>=s_textureCount || !s_textures[id].live)return;
    if(s_device)for(unsigned tile=0;tile<2;++tile)
        if(s_selectedTex[tile]==id) {
            s_device->SetTexture(tile,0);
            s_boundGameTexture[tile]=0;s_boundGameTextureValid[tile]=1;
        }
    releaseTexture(id);
}
static void api_filter(FilteringMode f){s_filter=(unsigned)f;}
static FilteringMode api_get_filter(void){return (FilteringMode)s_filter;}
static void api_mip_filter(MipmapFilteringMode f){if(f!=MIPMAP_DISABLED)++s_unsupported;}
static void api_aniso(int n){if(n>1)++s_unsupported;}
static int api_get_aniso(void){return 1;}


/* Phase 6B: native static room packets. No per-triangle fast-path probing.
 * Each lookup is for an entire contiguous TRI1/TRI4 command run. Original
 * RDP state commands are still interpreted in order by the Fast3D frontend.
 * Only source-proven room vertices can reach this path. */
static bool roomProjectionSafe(const GfxNativeRoomDraw *d) {
    if(!d || d->vertex_count<(GE_XBOX_NATIVE_ROOM_SHORT_RUNS?24u:48u) ||
       d->vertex_count>1024u ||
       d->cull_mode>2u || d->texture_units>1u ||
       !s_device || !s_shader || s_currentFB!=0 ||
       !s_hwScissorActive || !s_viewport.Width || !s_viewport.Height || s_decalDepth ||
       s_zNear<0.0f || s_zFar>1.0f || s_zFar<s_zNear ||
       !(d->aspect_scale>0.0f && d->aspect_scale<4.0f))return false;
#if !GE_XBOX_ROOM_GPU_CLIP
    /* Rollback: old room-AABB test (very conservative around the camera). */
    for(unsigned k=0;k<8u;++k) {
        const float x=(k&1u)?d->max_bound[0]:d->min_bound[0];
        const float y=(k&2u)?d->max_bound[1]:d->min_bound[1];
        const float z=(k&4u)?d->max_bound[2]:d->min_bound[2];
        const float wz=x*d->matrix[3]+y*d->matrix[7]+z*d->matrix[11]+d->matrix[15];
        const float cz=x*d->matrix[2]+y*d->matrix[6]+z*d->matrix[10]+d->matrix[14];
        if(!(wz>0.005f && cz > -wz+0.005f && cz < wz-0.005f))return false;
    }
#endif
    /* Hardware is allowed to clip opaque single-texture room triangles.
     * Room materials with fog/alpha/noise/tile-dependent UVs still fall back
     * through roomMaterialSafe(), including the N64 RSP near-plane lerp. */
    return true;
}
static bool roomMaterialSafe(const GfxNativeRoomDraw *d,GENativeShader **native) {
    if(native)*native=0;
    if(!roomProjectionSafe(d))return false;
    ShaderProgram *p=s_shader;
    if(!p || p->alpha || p->fog || p->grayscale || p->inputs>1u ||
       p->useTex[1] || (unsigned)p->useTex[0]!=d->texture_units ||
       p->features.opt_alpha || p->features.opt_noise ||
       p->features.opt_texture_edge || p->features.opt_fog ||
       p->features.opt_grayscale || p->features.opt_blur ||
       p->features.opt_invisible || p->features.opt_2cyc)return false;
    for(unsigned t=0;t<2u;++t)for(unsigned ax=0;ax<2u;++ax)
        if(p->clamps[t][ax])return false;
    const unsigned tex=s_selectedTex[0];
    if(d->texture_units) {
        if(tex>=s_textureCount)return false;
        const GETexture &texture=s_textures[tex];
        if(!texture.live || !texture.tex || texture.stripSlot ||
           texture.width!=texture.storageWidth ||
           texture.height!=texture.storageHeight)return false;
    }
    initStableMap(p);
    if(!p->stableMapValid)return false;
    /* With a single SHADE varying, RGB maps directly to V0 (diffuse). */
    if(p->inputs && p->stableMap.rgb[0]!=1u)return false;
    for(unsigned i=1;i<2u;++i)if(p->stableMap.rgb[i])return false;
    if(p->stableMap.alpha[0] || p->stableMap.alpha[1])return false;
    /* Defer native pixel-shader creation until a cold packet is built and
     * revisited; cache misses otherwise cannot increase shader churn. */
    GENativeShader *shader=nativeShader(p,p->stableMap);
    if(!shader || shader->hasTriangleConstants || shader->program.usesNoise)return false;
    if(native)*native=shader;
    return true;
}
#if GE_NV2A_ROOM_INDEXED_PACKETS
/* Graphics-thread-only cold packet scratch.  No heap allocation or hashing
 * on a warm draw. Identical vertex byte patterns are reusable even when
 * they appeared in different triangle positions; the index stream preserves
 * original triangle order exactly. 2048 slots, <=1024 source vertices. */
enum { GE_ROOM_INDEX_HASH_SLOTS=2048u, GE_ROOM_INDEX_MAX_VERTS=1024u };
static GERoomVertex s_roomUnique[GE_ROOM_INDEX_MAX_VERTS];
static unsigned short s_roomIndices[GE_ROOM_INDEX_MAX_VERTS];
static unsigned short s_roomIndexHash[GE_ROOM_INDEX_HASH_SLOTS];
static unsigned geRoomVertexHash(const GERoomVertex &v) {
    const unsigned char *p=(const unsigned char*)&v;
    unsigned h=2166136261u;
    for(unsigned i=0;i<sizeof(v);++i)h=(h^(unsigned)p[i])*16777619u;
    return h & (GE_ROOM_INDEX_HASH_SLOTS-1u);
}
static bool geRoomIndexPacket(const GfxNativeRoomDraw *d,unsigned *uniqueOut) {
    if(!d || !uniqueOut || !d->vertices ||
       !d->vertex_count || d->vertex_count>GE_ROOM_INDEX_MAX_VERTS)return false;
    memset(s_roomIndexHash,0,sizeof(s_roomIndexHash));
    unsigned n=0;
    for(unsigned i=0;i<d->vertex_count;++i) {
        const GfxNativeRoomVertex &v=d->vertices[i];
        GERoomVertex candidate;
        candidate.x=v.x; candidate.y=v.y; candidate.z=v.z;
        candidate.color=(d->texture_units || s_shader->inputs)?v.color:0u;
        candidate.specular=0u;
        candidate.u0=v.u0; candidate.v0=v.v0;
        candidate.u1=0.0f; candidate.v1=0.0f;
        unsigned bucket=geRoomVertexHash(candidate);
        bool found=false;
        for(unsigned probe=0;probe<GE_ROOM_INDEX_HASH_SLOTS;++probe) {
            const unsigned short record=s_roomIndexHash[bucket];
            if(!record) {
                if(n>=GE_ROOM_INDEX_MAX_VERTS)return false;
                s_roomUnique[n]=candidate;
                s_roomIndices[i]=(unsigned short)n;
                s_roomIndexHash[bucket]=(unsigned short)(++n);
                found=true;break;
            }
            if(memcmp(&candidate,&s_roomUnique[record-1u],sizeof(candidate))==0) {
                s_roomIndices[i]=(unsigned short)(record-1u);
                found=true;break;
            }
            bucket=(bucket+1u)&(GE_ROOM_INDEX_HASH_SLOTS-1u);
        }
        if(!found)return false;
    }
    *uniqueOut=n;
    return true;
}
#endif

static GERoomPacket *findRoomPacket(const GfxNativeRoomDraw *d) {
    for(unsigned i=0;i<GE_ROOM_PACKET_LIMIT;++i) {
        GERoomPacket &r=s_roomPackets[i];
        if(r.mesh.Ready() && r.source==d->command_address &&
           r.room==d->room && r.revision==d->revision &&
           r.signature==d->signature && r.vertices==d->vertex_count &&
           r.texture0==s_selectedTex[0] &&
           r.samplerU==s_addressMode[0][0] &&
           r.samplerV==s_addressMode[0][1] &&
           r.shaderKey==(unsigned)(uintptr_t)s_shader &&
           r.cullMode==d->cull_mode && r.textureUnits==d->texture_units) {
            r.lastUse=++s_roomPacketClock;
            return &r;
        }
    }
    return 0;
}
static void nativeRoomState(const GfxNativeRoomDraw *d,GENativeShader *shader) {
    nativeBatchBarrier();
    /* A freshly selected native combiner is identical to that used by the
     * nonresident Fast3D packet. This subset has no per-triangle constants. */
    if(!s_batchState.shaderValid || s_batchState.shaderHandle!=shader->handle) {
        s_device->SetPixelShader(shader->handle);
        s_batchState.shaderHandle=shader->handle;s_batchState.shaderValid=1;
        s_batchState.constantsValid=s_batchState.fogValid=s_batchState.alphaValid=0;
    }
    static const DWORD emptyConstants[8]={0,0,0,0,0,0,0,0};
    if(!s_batchState.constantsValid ||
       memcmp(s_batchState.constants,emptyConstants,sizeof(emptyConstants))) {
        float values[8][4];memset(values,0,sizeof(values));
        s_device->SetPixelShaderConstant(0,values,8);
        memcpy(s_batchState.constants,emptyConstants,sizeof(emptyConstants));
        s_batchState.constantsValid=1;
    }
    if(!s_batchState.fogValid || s_batchState.fogColor!=0xFF000000u) {
        s_device->SetRenderState(D3DRS_FOGENABLE,FALSE);
        s_device->SetRenderState(D3DRS_FOGCOLOR,0xFF000000u);
        s_batchState.fogColor=0xFF000000u;s_batchState.fogValid=1;
    }
    if(!s_batchState.alphaValid || s_batchState.alphaRef!=0) {
        s_device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
        s_device->SetRenderState(D3DRS_ALPHAREF,0);
        s_batchState.alphaRef=0;s_batchState.alphaValid=1;
    }
    bindNoiseTexture(false);
    s_device->SetRenderState(D3DRS_SPECULARENABLE,TRUE);
    s_device->SetRenderState(D3DRS_CULLMODE,
        d->cull_mode==1u?D3DCULL_CW:
        d->cull_mode==2u?D3DCULL_CCW:D3DCULL_NONE);
    s_device->SetVertexShader(GE_ROOM_FVF);
    /* Row-vector projection, same MP_matrix that Fast3D used for original
     * software clip vertices. Account for widescreen offset and Xbox D3D
     * [0,1] depth while preserving the original clip W exactly. */
    D3DMATRIX identity,projection;
    memset(&identity,0,sizeof(identity));
    identity._11=identity._22=identity._33=identity._44=1.0f;
    float *m=(float*)&projection;
    const float scale=d->aspect_scale;
    for(unsigned row=0;row<4u;++row) {
        const float w=d->matrix[row*4u+3u];
        m[row*4u+0u]=(d->matrix[row*4u]+d->aspect_offset*w)*scale;
        m[row*4u+1u]=d->matrix[row*4u+1u];
        m[row*4u+2u]=0.5f*(d->matrix[row*4u+2u]+w)*(s_zFar-s_zNear)+s_zNear*w;
        m[row*4u+3u]=w;
        /* Original XYZRHW backend applies -0.5 screen-space pixel offset. */
        m[row*4u+0u]-=w/(float)s_viewport.Width;
        m[row*4u+1u]+=w/(float)s_viewport.Height;
    }
    s_device->SetTransform(D3DTS_WORLD,&identity);
    s_device->SetTransform(D3DTS_VIEW,&identity);
    s_device->SetTransform(D3DTS_PROJECTION,&projection);
}
static bool api_nativeRoomDraw(const GfxNativeRoomDraw *d) {
#if GE_NV2A_ROOM_NATIVE_PACKETS
    GERoomPacket *p=d?findRoomPacket(d):0;
    if(!p)return false;
    GENativeShader *shader=0;
    if(!roomMaterialSafe(d,&shader))return false;
    nativeRoomState(d,shader);
    const bool drawn=p->mesh.DrawTriangles(s_device);
    /* Rebind the screen-space pipeline next time through flushNativeBatch.
     * The N64 frontend's RDP state and the shader selection are untouched. */
    s_device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    s_device->SetVertexShader(GE_FVF);
    s_streamBound=0;
    s_batchState.vertexValid=0;
    if(drawn) {
        const unsigned tris=d->vertex_count/3u;
        ++s_roomGPUFrames;s_roomGPUTriangles+=tris;
        s_roomGPUFrameTris+=tris;
        ++s_frameStats.gpu_batches;s_frameStats.output_triangles+=tris;
        s_drawCount+=tris;
    }
    return drawn;
#else
    (void)d;return false;
#endif
}
static bool api_nativeRoomPrepare(const GfxNativeRoomDraw *d) {
#if GE_NV2A_ROOM_NATIVE_PACKETS
    if(!d)return false;
#if GE_XBOX_ROOM_COLD_PREFLIGHT
    /* A NULL source and NULL vertex array denote a *cold admission probe*,
     * never a native draw. This must remain read-only: no D3D calls, no
     * shader builds, no allocation, no change to the packet LRU or throttle.
     * The source has already counted triangles but hasn't copied/UV-packed
     * their vertices. A warm lookup has a non-NULL command_address. */
    if(!d->vertices && !d->command_address) {
        if(!s_device || s_roomGPUAllocationFailed ||
           s_roomPacketBuildsFrame>=GE_XBOX_ROOM_UPLOADS_PER_FRAME ||
           (s_roomPrepareFrame%GE_XBOX_ROOM_UPLOAD_FRAME_INTERVAL)!=0u ||
           !d->vertex_count || d->vertex_count>1024u ||
           (d->vertex_count%3u)!=0u)return false;
        /* With 6D's default non-indexed packets, the byte count is exact.
         * For optional indexed packets, unique-vertex count is unknown
         * until conversion: never reject solely on a direct-size estimate
         * because an indexed packet could still fit. */
#if GE_XBOX_ROOM_NO_LIVE_EVICTION
#if GE_NV2A_ROOM_INDEXED_PACKETS
        if(s_roomPacketBytes>=GE_ROOM_VERTEX_BYTES)return false;
#else
        const unsigned coldBytes=d->vertex_count*sizeof(GERoomVertex);
        if(s_roomPacketBytes>GE_ROOM_VERTEX_BYTES ||
           coldBytes>GE_ROOM_VERTEX_BYTES-s_roomPacketBytes)return false;
#endif
        for(unsigned i=0;i<GE_ROOM_PACKET_LIMIT;++i)
            if(!s_roomPackets[i].mesh.Ready())return true;
        return false; /* No live eviction: nothing safe to allocate. */
#else
        /* Do not change legacy LRU eviction semantics in an A/B build. */
        return true;
#endif
    }
#endif
    /* No vertex pointer: a read-only residency query after a camera-bound
     * hardware fallback. Preserve the frontend metadata when the GPU
     * packet still exists, avoiding rebuilds during near-plane crossings. */
    if(!d->vertices)return findRoomPacket(d)!=0;
    if(s_roomGPUAllocationFailed ||
       s_roomPacketBuildsFrame>=GE_XBOX_ROOM_UPLOADS_PER_FRAME ||
       !d->vertex_count || d->vertex_count>1024u ||
       (d->vertex_count%3u)!=0u ||
       (s_roomPrepareFrame%GE_XBOX_ROOM_UPLOAD_FRAME_INTERVAL)!=0u)return false;
    GENativeShader *shader=0;
    if(!roomMaterialSafe(d,&shader) || findRoomPacket(d))return false;
#if GE_NV2A_ROOM_INDEXED_PACKETS
    unsigned uniqueVertices=0;
    if(!geRoomIndexPacket(d,&uniqueVertices))return false;
    const unsigned directBytes=d->vertex_count*sizeof(GERoomVertex);
    const unsigned indexedBytes=uniqueVertices*sizeof(GERoomVertex)+
                                d->vertex_count*sizeof(unsigned short);
    /* Use the index buffer only if it actually saves >=25% of resident
     * space. Avoid extra index submission overhead for nearly unique runs. */
    const bool useIndexed=(indexedBytes*4u<=directBytes*3u);
    const unsigned bytes=useIndexed?indexedBytes:directBytes;
#else
    const unsigned bytes=d->vertex_count*sizeof(GERoomVertex);
#endif
    if(bytes>GE_ROOM_VERTEX_BYTES)return false;
    /* Always enforce the actual resident-byte cap *before* any D3D allocation.
     * Prevent unsigned overflow even if corrupt metadata slips through. */
#if GE_XBOX_ROOM_NO_LIVE_EVICTION
    if(s_roomPacketBytes>GE_ROOM_VERTEX_BYTES ||
       bytes>GE_ROOM_VERTEX_BYTES-s_roomPacketBytes)return false;
    GERoomPacket *dest=0;
    for(unsigned i=0;i<GE_ROOM_PACKET_LIMIT;++i)
        if(!s_roomPackets[i].mesh.Ready()){dest=&s_roomPackets[i];break;}
    /* Never Release/replace a VB or IB while the NV2A could reference it.
     * A full cache is a normal software-fallback condition, not an error. */
    if(!dest)return false;
#else
    GERoomPacket *dest=0;
    for(unsigned i=0;i<GE_ROOM_PACKET_LIMIT;++i)
        if(!s_roomPackets[i].mesh.Ready()){dest=&s_roomPackets[i];break;}
    if(!dest) {
        for(unsigned i=0;i<GE_ROOM_PACKET_LIMIT;++i)
            if(!dest || s_roomPackets[i].lastUse<dest->lastUse)dest=&s_roomPackets[i];
    }
    if(!dest)return false;
    /* Reclaim the destination FIRST; 6C previously evicted unrelated packets
     * even when replacing the selected slot would have freed enough space. */
    if(dest->mesh.Ready()){
        s_roomPacketBytes-=dest->mesh.Bytes();
        dest->mesh.Reset();dest->source=0;dest->vertices=0;
    }
    while(bytes>GE_ROOM_VERTEX_BYTES-s_roomPacketBytes) {
        GERoomPacket *oldest=0;
        for(unsigned i=0;i<GE_ROOM_PACKET_LIMIT;++i) {
            GERoomPacket *r=&s_roomPackets[i];
            if(r==dest || !r->mesh.Ready())continue;
            if(!oldest || r->lastUse<oldest->lastUse)oldest=r;
        }
        if(!oldest)return false;
        s_roomPacketBytes-=oldest->mesh.Bytes();
        oldest->mesh.Reset();oldest->source=0;oldest->vertices=0;
    }
#endif
    ++s_roomPacketBuildsFrame;
    dest->source=0;dest->vertices=0;
#if GE_NV2A_ROOM_INDEXED_PACKETS
    if(useIndexed) {
        if(!dest->mesh.BuildIndexed(s_device,uniqueVertices,sizeof(GERoomVertex),
                                    GE_ROOM_FVF,s_roomUnique,d->vertex_count,
                                    s_roomIndices)) {
            s_roomGPUAllocationFailed=true;
            return false;
        }
    } else
#endif
    {
    void *mapped=0;
    if(!dest->mesh.BeginBuild(s_device,d->vertex_count,sizeof(GERoomVertex),GE_ROOM_FVF,&mapped)) {
        s_roomGPUAllocationFailed=true;
        return false;
    }
    GERoomVertex *dst=(GERoomVertex*)mapped;
    for(unsigned i=0;i<d->vertex_count;++i) {
        const GfxNativeRoomVertex &v=d->vertices[i];
        GERoomVertex &o=dst[i];
        o.x=v.x;o.y=v.y;o.z=v.z;
        o.color=d->texture_units || s_shader->inputs?v.color:0u;
        /* The original backend uses diffuse RGB from V0, no varying
         * alpha or V1 for this narrowly selected material subset. */
        o.specular=0u;o.u0=v.u0;o.v0=v.v0;o.u1=o.v1=0.0f;
    }
    if(!dest->mesh.EndBuild()) {
        s_roomGPUAllocationFailed=true;
        return false;
    }
    }
    dest->source=d->command_address;dest->room=d->room;
    dest->revision=d->revision;dest->signature=d->signature;
    dest->vertices=d->vertex_count;dest->lastUse=++s_roomPacketClock;
    dest->shaderKey=(unsigned)(uintptr_t)s_shader;
    dest->texture0=s_selectedTex[0];dest->samplerU=s_addressMode[0][0];
    dest->samplerV=s_addressMode[0][1];
    dest->cullMode=d->cull_mode;dest->textureUnits=d->texture_units;
    s_roomPacketBytes+=bytes;
    return true;
#else
    (void)d;return false;
#endif
}

extern "C" {
GfxRenderingAPI gfx_xbox_d3d8_api={
    api_name,api_max_texture,api_clip,api_unload_shader,api_load_shader,
    api_create_shader,api_lookup,api_shader_info,api_clear_shaders,
    api_new_texture,api_select_texture,api_upload_texture,api_sampler,
    api_depth,api_depth_range,api_viewport,api_scissor,api_alpha,
    api_draw,api_init,api_resize,api_begin,api_end,api_finish,
    api_create_fb,api_fb_params,api_fb_start,api_fb_copy,api_fb_clear,
    api_fb_resolve,api_fb_tex,api_fb_select,api_tex_delete,
    api_filter,api_get_filter,api_mip_filter,api_aniso,api_get_aniso,
    api_nativeRoomDraw,api_nativeRoomPrepare
};
}
