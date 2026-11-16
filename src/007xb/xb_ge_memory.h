#ifndef XB_GE_MEMORY_H
#define XB_GE_MEMORY_H

/* Phase 2: GoldenEye N64 DRAM and cartridge-memory translation on Xbox.
 * The game expects 8 MB N64 RDRAM and addresses at 0x70000000 (PC-port
 * virtual), 0x80000000 (KSEG0) and 0xA0000000 (KSEG1).  The Xbox is not
 * guaranteed to map memory at those literal VMAs.  All converted DMA/GBI
 * accesses must pass through these checked translators.  This is NOT an
 * unsafe claim that the original game can dereference raw KSEG0 pointers.
 *
 * Original N64 ROM is streamed directly from the v0.4 asset archive or an
 * optional loose .z64.  It is never loaded wholesale into 64 MB RAM.
 * C++98, no STL / exceptions / RTTI, explicit lifetime.
 */
#include <stdio.h>
#include "xb_archive.h"

#if defined(XBOX) || defined(_XBOX)
#include <xtl.h>
#else
#include <pthread.h>
#endif

typedef unsigned int GEU32;
#define GE_N64_DRAM_BYTES  0x00800000u
#define GE_N64_CART_BASE   0x10000000u
#define GE_N64_CART_BYTES  0x00C00000u

class GEDram {
public:
    GEDram();
    bool Init();
    void Shutdown();
    /* Resolve a 32-bit N64 virtual address or a DRAM physical offset. */
    void* Resolve(GEU32 n64Address, GEU32 bytes);
    const void* ResolveConst(GEU32 n64Address, GEU32 bytes) const;
    bool ToPhysical(const void* ptr, GEU32* offset) const;
    void* FromPhysical(GEU32 offset, GEU32 bytes);
    /* Returns a 0x70000000-series virtual address for a live DRAM pointer. */
    bool ToGameAddress(const void* ptr, GEU32* address) const;
    GEU32 AllocatedBytes() const { return ram_ ? GE_N64_DRAM_BYTES : 0; }
private:
    GEDram(const GEDram&);
    GEDram& operator=(const GEDram&);
    unsigned char* raw_;
    unsigned char* ram_;
};

class GECart {
public:
    GECart();
    void Close();
    bool OpenLoose(const char* path);
    /* Uses a SECOND file handle: game DMA cannot race XBArchive::Read. */
    bool OpenArchive(const XBArchive& index,const char* archivePath);
    bool OpenDefault(const XBArchive& index);
    bool Read(GEU32 cartAddress,void* dst,GEU32 size);
    /* Xbox animation hot path: return true only when the requested byte span
       is already resident in the immutable ROM cache. Never read from disk.
       False means the caller must use ordinary cartridge DMA. */
    bool ReadCachedAnimation(GEU32 cartAddress,void* dst,GEU32 size);
    GEU32 Bytes() const { return bytes_; }
    bool Ready() const { return file_ != 0; }
    const char* Source() const { return source_; }
    const char* Error() const { return error_; }
private:
    GECart(const GECart&);
    GECart& operator=(const GECart&);
    bool OpenFile(const char* path,GEU32 base,GEU32 len);
    bool VerifyHeader();
    bool SetError(const char* value);
    FILE* file_;
    GEU32 base_,bytes_;
    /* Optional immutable animation-ROM cache (~1.41 MiB).  Owned by GECart,
       released in Close(); NULL retains the original disk-backed reads. */
    unsigned char* animCache_;
    char source_[128],error_[128];
#if defined(XBOX) || defined(_XBOX)
    CRITICAL_SECTION lock_;
#else
    pthread_mutex_t lock_;
#endif
};

#endif
