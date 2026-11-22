#ifndef GE_NATIVE_RUN_CACHE_H
#define GE_NATIVE_RUN_CACHE_H
/* Phase 10: bounded, CPU-side compiled dynamic GPU draw packets.
 * The render thread owns these records. NO GPU buffers, STL allocations,
 * cross-thread pointers or changes to the original RSP/VI simulation.
 * Camera / modelview transforms are deliberately not cached.
 * Material and source inputs are checked exactly before packet reuse.
 * Keep this C++98-compatible for the classic Xbox toolchain. */
#include <stdint.h>
#include <string.h>
#include "gfx_rendering_api.h"

#ifndef GE_XBOX_COMPILED_RUN_RECORDS
#define GE_XBOX_COMPILED_RUN_RECORDS 24u
#endif
#if GE_XBOX_COMPILED_RUN_RECORDS < 4u || GE_XBOX_COMPILED_RUN_RECORDS > 64u
#error GE_XBOX_COMPILED_RUN_RECORDS must be 4..64
#endif
#define GE_COMPILED_RUN_MAX_COMMANDS 32u
#define GE_COMPILED_RUN_MAX_SLOTS 128u
#define GE_COMPILED_RUN_MAX_INDICES (GE_COMPILED_RUN_MAX_COMMANDS*12u)
#define GE_COMPILED_MATERIAL_WORDS 40u

struct GECompiledSourceVertex {
    float x,y,z;
    float u,v;             /* original G_VTX-scaled S/T (before tile UV) */
    uint32_t color;        /* final RSP SHADE, exact bit pattern */
};
typedef char GECompiledSourceVertexMustBe24Bytes[
    (sizeof(GECompiledSourceVertex)==24u)?1:-1];
struct GECompiledNativeRun {
    const void* command_address;
    uint32_t command_words[GE_COMPILED_RUN_MAX_COMMANDS*2u];
    uint32_t material[GE_COMPILED_MATERIAL_WORDS];
    unsigned requested_commands,accepted_commands;
    unsigned index_count,unique_count;
    uint8_t slots[GE_COMPILED_RUN_MAX_SLOTS];
    GECompiledSourceVertex source[GE_COMPILED_RUN_MAX_SLOTS];
    GfxNativeRoomVertex vertices[GE_COMPILED_RUN_MAX_SLOTS];
    uint16_t indices[GE_COMPILED_RUN_MAX_INDICES];
    float min_bound[3],max_bound[3];
    unsigned age;
    uint32_t serial; /* changes on every overwritten/rebuilt record */
    bool valid;
};

/* No hash-only equality; even a reused arena or address is verified by
 * command bytes, material inputs and all used original source vertex bits. */
static inline bool geCompiledVertexEqual(const GECompiledSourceVertex &a,
                                         const GECompiledSourceVertex &b) {
    return memcmp(&a,&b,sizeof(a))==0;
}
static inline bool geCompiledMaterialEqual(const uint32_t* a,const uint32_t* b) {
    return memcmp(a,b,GE_COMPILED_MATERIAL_WORDS*sizeof(uint32_t))==0;
}
static inline bool geCompiledCommandWordsEqual(const uint32_t* a,const uint32_t* b,
                                               unsigned count) {
    return count<=GE_COMPILED_RUN_MAX_COMMANDS &&
           memcmp(a,b,2u*count*sizeof(uint32_t))==0;
}
#endif
