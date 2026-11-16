#ifndef _NO_CRT_STDIO_INLINE
#define _NO_CRT_STDIO_INLINE
#endif
#include "ge_native_platform.h"
#include "xb_ge_runtime.h"
#include "xb_ge_libultra.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Normal playtest mode: WARN/ERROR/FATAL only, with a single compile-time
 * rollback switch. Does not alter the game's scheduler, RNG or error state. */
#ifndef GE_XBOX_WARN_ERROR_ONLY
#define GE_XBOX_WARN_ERROR_ONLY 1
#endif
/* Quiet mode wins even if a local build had verbose tracing enabled. */
#if GE_NATIVE_TRACE_VERBOSE && !GE_XBOX_WARN_ERROR_ONLY
#define GE_NATIVE_TRACE_OUTPUT_VERBOSE 1
#else
#define GE_NATIVE_TRACE_OUTPUT_VERBOSE 0
#endif

#if defined(XBOX) || defined(_XBOX)
#include <xtl.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#endif

/* All N64 game functions are provided by their original translation units.
 * This file implements only the host services those units use. */
static GERuntime *s_runtime = 0;
static char s_dataDirectory[256] = "D:\\data";
static char s_error[512] = "";
static volatile int s_fatal = 0;
static void (*s_fatalHook)(void) = 0;
static int s_romdataReady = 0;
static unsigned char s_eeprom[2048];
static int s_eepromState = 0;
static char s_eepromDirectory[260] = "";
static char s_tracePath[512] = "";
static char s_traceError[256] = "Trace storage has not been initialized";
#if defined(XBOX) || defined(_XBOX)
static volatile LONG s_traceReady = 0;
#else
static volatile int s_traceReady = 0;
#endif
static const unsigned long kTraceLimit = 256u * 1024u;
/* Progress records have fixed storage: 16 KiB plus record headers, and one
 * 64 KiB writer stack. No allocations or storage calls occur under the
 * producer lock. The existing trace lock serializes every disk writer. */
enum { TRACE_QUEUE_CAPACITY = 8, TRACE_RECORD_BYTES = 2048,
       TRACE_WRITER_STACK_BYTES = 64u * 1024u,
       TRACE_DEFERRED_STOPPED = 0, TRACE_DEFERRED_ASYNC = 1,
       TRACE_DEFERRED_SYNC = 2 };
struct TraceRecord { unsigned bytes; char text[TRACE_RECORD_BYTES]; };
static TraceRecord s_traceQueue[TRACE_QUEUE_CAPACITY];
static unsigned s_traceQueueFirst = 0, s_traceQueueCount = 0;
static unsigned s_traceQueueDropped = 0, s_tracePendingDrops = 0;
static unsigned s_tracePendingInvalid = 0;
static int s_traceDeferredMode = TRACE_DEFERRED_STOPPED;
static int s_traceWriterStop = 0, s_traceWriterStarted = 0;
#if defined(XBOX) || defined(_XBOX)
static CRITICAL_SECTION s_traceQueueLock;
static HANDLE s_traceWriter = 0, s_traceWake = 0;
#else
static pthread_mutex_t s_traceQueueLock;
static pthread_cond_t s_traceWake;
static pthread_t s_traceWriter;
#endif
static void InitTraceStorage(void);
struct GENativeImageRange { uint32_t base, bytes; };
static GENativeImageRange s_imageRanges[64];
static unsigned s_imageRangeCount = 0;
extern "C" unsigned int *_bssSegmentEnd;
unsigned int *_bssSegmentEnd = 0;
static int s_locksReady = 0;
#if defined(XBOX) || defined(_XBOX)
static CRITICAL_SECTION s_platformLock;
static CRITICAL_SECTION s_interruptLock;
static CRITICAL_SECTION s_traceLock;
/* RXDK's Interlocked declarations take LPLONG. Keep storage volatile and
 * remove that qualifier only at the atomic API boundary below. */
static volatile LONG s_interruptOwner = 0;
#else
static pthread_mutex_t s_platformLock;
static pthread_mutex_t s_interruptLock;
static pthread_mutex_t s_traceLock;
static volatile uintptr_t s_interruptOwner = 0;
#endif

static void PlatformLock(void) {
#if defined(XBOX) || defined(_XBOX)
    EnterCriticalSection(&s_platformLock);
#else
    pthread_mutex_lock(&s_platformLock);
#endif
}
static void PlatformUnlock(void) {
#if defined(XBOX) || defined(_XBOX)
    LeaveCriticalSection(&s_platformLock);
#else
    pthread_mutex_unlock(&s_platformLock);
#endif
}
static uintptr_t ThreadIdentifier(void) {
#if defined(XBOX) || defined(_XBOX)
    return (uintptr_t)GetCurrentThreadId();
#else
    return (uintptr_t)pthread_self();
#endif
}

static void InitLocks(void) {
    if (s_locksReady) return;
    /* Called by the host before any original game thread is created. */
#if defined(XBOX) || defined(_XBOX)
    InitializeCriticalSection(&s_platformLock);
    InitializeCriticalSection(&s_interruptLock);
    InitializeCriticalSection(&s_traceLock);
    InitializeCriticalSection(&s_traceQueueLock);
#else
    pthread_mutex_init(&s_platformLock, 0);
    pthread_mutex_init(&s_interruptLock, 0);
    pthread_mutex_init(&s_traceLock, 0);
    pthread_mutex_init(&s_traceQueueLock, 0);
    pthread_cond_init(&s_traceWake, 0);
#endif
    s_locksReady = 1;
}

static void CopyString(char *out, size_t capacity, const char *value) {
    size_t n = 0;
    if (!out || !capacity) return;
    if (value) while (n + 1 < capacity && value[n]) { out[n] = value[n]; ++n; }
    out[n] = 0;
}

/* XBE layout follows the primary format definitions in xemu-xbe.h and
 * Cxbx-Reloaded/src/common/xbe/Xbe.h. Read words explicitly so the parser
 * does not depend on packed compiler structures or a PE linker symbol. */
static uint32_t ImageWord(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int BindImageRanges(const void *memory, size_t available, uintptr_t loadAddress) {
    const unsigned char *header = (const unsigned char *)memory;
    s_imageRangeCount = 0;
    if (!header || available < 0x124u || loadAddress > UINT32_MAX) return 0;
    if (memcmp(header, "XBEH", 4) != 0) return 0;
    const uint32_t base = ImageWord(header + 0x104u);
    const uint32_t headers = ImageWord(header + 0x108u);
    const uint32_t image = ImageWord(header + 0x10Cu);
    const uint32_t count = ImageWord(header + 0x11Cu);
    const uint32_t table = ImageWord(header + 0x120u);
    if (base != loadAddress || headers < 0x124u || headers > available ||
        image < headers || image >= 0x04000000u || image > UINT32_MAX - base ||
        !count || count > 64u || table < base) return 0;
    const uint32_t offset = table - base;
    if (offset < 0x124u || offset > headers || count > (headers - offset) / 56u) return 0;
    GENativeImageRange ranges[64];
    unsigned loaded = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char *section = header + offset + i * 56u;
        const uint32_t flags = ImageWord(section);
        const uint32_t address = ImageWord(section + 4u);
        const uint32_t size = ImageWord(section + 8u);
        const uint32_t references = ImageWord(section + 24u);
        if (!size) continue;
        if (address < base || address - base >= image || size > image - (address - base)) return 0;
        /* PRELOAD sections are resident from launch. A later loaded section
         * has a positive reference count. Never classify unloaded extents. */
        if ((flags & 2u) || references) {
            ranges[loaded].base = address; ranges[loaded].bytes = size; ++loaded;
        }
    }
    if (!loaded) return 0;
    memcpy(s_imageRanges, ranges, loaded * sizeof(ranges[0]));
    s_imageRangeCount = loaded;
    return 1;
}
#if defined(GE_NATIVE_PLATFORM_TEST)
extern "C" int GENativePlatform_TestImageHeader(const void *memory, size_t available, uintptr_t loadAddress) {
    return BindImageRanges(memory, available, loadAddress);
}
#endif

extern "C" int GENativePlatform_Bind(void *runtime, const char *directory) {
    GERuntime *candidate = (GERuntime *)runtime;
    if (!candidate || !candidate->Ready()) return 0;
    InitLocks();
#if defined(XBOX) || defined(_XBOX)
    /* Xbox loads the XBE header at 0x10000. The current image's base and
     * every accepted range must agree with the header before use. */
    const unsigned char *imageHeader = (const unsigned char *)0x10000u;
    const uint32_t headerBytes = ImageWord(imageHeader + 0x108u);
    if (headerBytes > 0x00100000u ||
        !BindImageRanges(imageHeader, headerBytes, 0x10000u)) {
        GENativePlatform_SetError("Current XBE has an invalid image section table");
        return 0;
    }
#endif
    PlatformLock();
    s_runtime = candidate;
    s_error[0] = 0;
    s_fatal = 0;
    s_romdataReady = 1;
    s_eepromState = 0;
    s_eepromDirectory[0] = 0;
    if (directory && directory[0]) CopyString(s_dataDirectory, sizeof(s_dataDirectory), directory);
    PlatformUnlock();
    InitTraceStorage();
    return 1;
}

extern "C" const char *GENativePlatform_Error(void) { return s_error; }
extern "C" int GENativePlatform_HasFatalError(void) { return s_fatal != 0; }
extern "C" void GENativePlatform_SetFatalHook(void (*hook)(void)) { s_fatalHook = hook; }
extern "C" void GENativePlatform_SetError(const char *message) {
    if (s_locksReady) PlatformLock();
    CopyString(s_error, sizeof(s_error), message);
    if (s_locksReady) PlatformUnlock();
}
extern "C" void *geNativeDramBase(void) {
    return s_runtime ? s_runtime->Dram().Resolve(0u, GE_N64_DRAM_BYTES) : 0;
}
extern "C" size_t geNativeDramBytes(void) {
    return s_runtime ? s_runtime->Dram().AllocatedBytes() : 0u;
}
extern "C" int geNativeAddressIsRam(uintptr_t address) {
    const uintptr_t base = (uintptr_t)geNativeDramBase();
    return base && address >= base && address - base < GE_N64_DRAM_BYTES;
}
extern "C" int geNativeAddressIsNativeImage(uintptr_t address) {
    if (address > UINT32_MAX) return 0;
    const uint32_t value = (uint32_t)address;
    for (unsigned i = 0; i < s_imageRangeCount; ++i) {
        const GENativeImageRange &range = s_imageRanges[i];
        if (value >= range.base && value - range.base < range.bytes) return 1;
    }
    return 0;
}
extern "C" void *geNativeAddressToPointer(uintptr_t address) {
    if (address <= UINT32_MAX) {
        const uint32_t a = (uint32_t)address;
        const uint32_t tag = a & 0xFF000000u;
        if (tag == 0x70000000u || tag == 0x80000000u ||
            tag == 0xA0000000u || tag == 0xF0000000u) {
            const uint32_t offset = a & 0x00FFFFFFu;
            return s_runtime && offset < GE_N64_DRAM_BYTES ?
                s_runtime->Dram().Resolve(offset, 1u) : 0;
        }
    }
    return (void *)address;
}
extern "C" uint32_t geNativePointerToPhysical(const void *pointer) {
    if (!pointer) return 0u;
    if (pointer == (const void *)&_bssSegmentEnd)
        return GE_NATIVE_DRAM_TOKEN + 0x50000u;
    const uintptr_t p = (uintptr_t)pointer;
    const uintptr_t base = (uintptr_t)geNativeDramBase();
    if (base && p >= base && p - base < GE_N64_DRAM_BYTES)
        return GE_NATIVE_DRAM_TOKEN + (uint32_t)(p - base);
    if (p > UINT32_MAX) {
        GENativePlatform_SetError("Native engine pointer exceeds the 32-bit Xbox ABI");
        return 0u;
    }
    return (uint32_t)p;
}
extern "C" int GENativePlatform_ReadCartridge(uint32_t address, void *out, uint32_t bytes) {
    return s_runtime && s_runtime->Cart().Read(address, out, bytes) ? 1 : 0;
}
extern "C" int romdataInit(void) {
    s_romdataReady = s_runtime && s_runtime->CartReady();
    if (!s_romdataReady) GENativePlatform_SetError("Cartridge stream is not initialized");
    return s_romdataReady ? 0 : -1;
}
extern "C" void romdataDestroy(void) {
    /* GERuntime owns the stream and arena. It must not release either while
     * an original game thread is alive; invalidate only this accessor view. */
    s_romdataReady = 0;
}
extern "C" const unsigned char *romdataGetRom(void) {
    /* There is no contiguous host ROM image. Native code must use bounded
     * cartridge DMA; returning the cart token as a CPU pointer is unsafe. */
    return 0;
}
extern "C" uint32_t romdataGetRomSize(void) {
    return s_romdataReady && s_runtime ? s_runtime->Cart().Bytes() : 0u;
}
extern "C" int romdataCartAddrValid(uint32_t address, uint32_t bytes) {
    if (!s_romdataReady || !s_runtime || address < GE_N64_CART_BASE) return 0;
    const uint32_t offset = address - GE_N64_CART_BASE;
    const uint32_t size = s_runtime->Cart().Bytes();
    return offset <= size && bytes <= size - offset;
}
extern "C" const void *romdataMapVa(uint32_t address) {
    void *pointer = geNativeAddressToPointer(address);
    return geNativeAddressIsRam((uintptr_t)pointer) ? pointer : 0;
}

/* Original PORT builds use osClockRate for timer conversion and a distinct
 * ~46.55 MHz CPU count for frame pacing. Keep the contracts separate. */
extern "C" {
unsigned long long osClockRate = 6250000ull;
unsigned int osMemSize = GE_N64_DRAM_BYTES;
unsigned int osTvType = 1u; /* NTSC-U */
unsigned int osResetType = 0u;
int osViClock = 0;
XBGEAbiQueue vi_c_debug_MQ;
unsigned char *g_pc_animdata_base = 0;
/* libultra's sinf/cosf declare this as float data, not a function. */
unsigned int __libm_qnan_f = 0x7FC00000u;
}
static void *s_viMessages[8];

/* The game's masked sections protect shared libaudio lists and scheduler
 * clients across native threads. An atomic mask-value assignment is not
 * sufficient. N64 nested disable/restore(NONE) is idempotent, not counted. */
extern "C" unsigned int osSetIntMask(unsigned int mask) {
    const unsigned int all = 0x003FFF01u, none = 1u;
    const uintptr_t self = ThreadIdentifier();
    InitLocks();
    if (mask == none) {
#if defined(XBOX) || defined(_XBOX)
        if ((uintptr_t)(DWORD)InterlockedCompareExchange(const_cast<LONG *>(&s_interruptOwner), 0, 0) == self)
            return none;
        EnterCriticalSection(&s_interruptLock);
        InterlockedExchange(const_cast<LONG *>(&s_interruptOwner), (LONG)self);
#else
        if (__atomic_load_n(&s_interruptOwner, __ATOMIC_ACQUIRE) == self) return none;
        pthread_mutex_lock(&s_interruptLock);
        __atomic_store_n(&s_interruptOwner, self, __ATOMIC_RELEASE);
#endif
        return all;
    }
    if (mask == all) {
#if defined(XBOX) || defined(_XBOX)
        if ((uintptr_t)(DWORD)InterlockedCompareExchange(const_cast<LONG *>(&s_interruptOwner), 0, 0) == self) {
            InterlockedExchange(const_cast<LONG *>(&s_interruptOwner), 0);
            LeaveCriticalSection(&s_interruptLock);
            return none;
        }
#else
        if (__atomic_load_n(&s_interruptOwner, __ATOMIC_ACQUIRE) == self) {
            __atomic_store_n(&s_interruptOwner, 0u, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&s_interruptLock);
            return none;
        }
#endif
    }
    return mask; /* VI-only hardware masks do not alter the game lock. */
}

/* Calling conventions and layouts match the original PR headers. The
 * private names prevent duplicate type definitions beside the C++ host. */
struct GEViCommonRegs { unsigned ctrl, width, burst, vSync, hSync, leap, hStart, xScale, vCurrent; };
struct GEViFieldRegs { unsigned origin, yScale, vStart, vBurst, vIntr; };
struct GEViMode { unsigned char type; GEViCommonRegs comRegs; GEViFieldRegs fldRegs[2]; };
typedef char GEViModeSizeCheck[sizeof(GEViMode) == 80 ? 1 : -1];
extern "C" void videoUpdateNativeResolution(int width, int height);
extern "C" void osViSetMode(GEViMode *mode) {
    if (!mode) return;
    int width = (int)mode->comRegs.width;
    int height = (int)(((mode->fldRegs[0].yScale & 0xFFFu) * 480u) / 0x800u);
    if (width <= 0 || width > 640) width = 320;
    if (height <= 0 || height > 480) height = 240;
    videoUpdateNativeResolution(width, height);
}
extern "C" void osCreateViManager(int) {}
extern "C" void osViSetSpecialFeatures(unsigned) {}
extern "C" void osDpSetStatus(unsigned) {}
extern "C" unsigned osDpGetStatus(void) { return 0; }
extern "C" void osWritebackDCache(void *, int) {}
extern "C" void osWritebackDCacheAll(void) {}
extern "C" void osInvalDCache(void *, int) {}
extern "C" void osInvalICache(void *, int) {}
extern "C" unsigned osVirtualToPhysical(void *pointer) { return geNativePointerToPhysical(pointer); }
extern "C" void *osPhysicalToVirtual(unsigned address) { return geNativeAddressToPointer(address); }
extern "C" void piCreateManager(void) {}
extern "C" void viDebugRemoved(void) {}
extern "C" void viInit(void) { osCreateMesgQueue(&vi_c_debug_MQ, s_viMessages, 8); }
extern "C" int osPiReadIo(unsigned, unsigned *value) { if (value) *value = 0; return 0; }

/* N64 reset/MMU bootstrap is compiled but never executed on Xbox: startup
 * enters the original mainproc directly after native host initialization. */
extern "C" unsigned get_csegmentSegmentStart(void) { return 0; }
extern "C" unsigned get_cdataSegmentRomStart(void) { return 0; }
extern "C" unsigned get_cdataSegmentRomEnd(void) { return 0; }
extern "C" unsigned get_inflateSegmentRomStart(void) { return 0; }
extern "C" unsigned get_inflateSegmentRomEnd(void) { return 0; }
extern "C" unsigned jump_decompressfile(unsigned, unsigned, unsigned) { return 0; }
extern "C" void initTLBPrepareContext(void) {}
extern "C" void resolve_TLBaddress_for_InvalidHit(void) {}
extern "C" unsigned __osGetFpcCsr(void) { return 0; }
extern "C" unsigned __osSetFpcCsr(unsigned) { return 0; }
extern "C" void osUnmapTLB(int) {}
extern "C" void tlbmanageEstablishManagementTable(void) {}
extern "C" void tlbmanageResetCurrentEntriesCount(void) {}
extern "C" void tlbmanageTranslateLoadRomFromTlbAddress(unsigned) {}
extern "C" void *tlbmanageGetTlbAllocatedBlock(void) {
    unsigned char *base = (unsigned char *)geNativeDramBase();
    /* Match the original port's 7 MiB heap ceiling; the final MiB is the
     * separate dynamic vertex pool referenced by game/dyn.c. */
    return base ? base + 0x700000u : 0;
}
extern "C" void rmonMain(void *) {}
extern "C" int rmonGetToken(void) { return 0; }
extern "C" int rmonStatus(void) { return 0; }
extern "C" void osReadHost(void *, unsigned) {}
extern "C" void osWriteHost(void *, unsigned) {}
extern "C" void crashInit(void) {}
extern "C" void crashRenderFrame(void *) {}
#if defined(XBOX) || defined(_XBOX)
extern "C" void bcopy(const void *source, void *dest, int bytes) { if (bytes > 0) memmove(dest, source, (size_t)bytes); }
extern "C" void bzero(void *dest, int bytes) { if (bytes > 0) memset(dest, 0, (size_t)bytes); }
#endif

/* Native time and legacy CRT formatting. Explicit _vsnprintf avoids the
 * modern UCRT inline formatter that is unavailable in classic RXDK libs. */
extern "C" uint64_t sysGetMicroseconds(void) {
    if (s_runtime) return s_runtime->OS().GetTimeUs();
#if defined(XBOX) || defined(_XBOX)
    return (uint64_t)GetTickCount() * 1000u;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000u;
#endif
}
extern "C" int64_t sysGetTime(void) {
#if defined(XBOX) || defined(_XBOX)
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int64_t)((t - 116444736000000000ull) / 10000000ull);
#else
    return (int64_t)time(0);
#endif
}
extern "C" uintptr_t sysImageBase(void) { return 0x10000u; }
extern "C" void sysSleep(uint32_t micros) {
#if defined(XBOX) || defined(_XBOX)
    Sleep(micros / 1000u + (micros % 1000u != 0u));
#else
    struct timespec t; t.tv_sec = micros / 1000000u; t.tv_nsec = (long)(micros % 1000000u) * 1000L;
    nanosleep(&t, 0);
#endif
}
extern "C" void sysCpuRelax(void) {
#if defined(XBOX) || defined(_XBOX)
    Sleep(0);
#else
    sched_yield();
#endif
}
static int Format(char *out, size_t capacity, const char *fmt, va_list ap) {
    if (!out || !capacity || !fmt) return -1;
#if defined(XBOX) || defined(_XBOX)
    int n = _vsnprintf(out, capacity, fmt, ap);
#else
    int n = vsnprintf(out, capacity, fmt, ap);
#endif
    out[capacity - 1u] = 0;
    return n;
}
extern "C" int geXboxVsnprintf(char *out, size_t capacity, const char *fmt, va_list ap) {
    return Format(out, capacity, fmt, ap);
}
extern "C" int geXboxSnprintf(char *out, size_t capacity, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int n = Format(out, capacity, fmt, ap); va_end(ap); return n;
}
static void WriteLog(const char *message) {
#if defined(XBOX) || defined(_XBOX)
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
#else
    fputs(message, stderr); fputc('\n', stderr);
#endif
}
#if GE_XBOX_WARN_ERROR_ONLY
/* Legacy osSyncPrintf/stdio have no severity argument. Accept unmistakable
 * warning/failure wording, suppress unclassified game debug chatter. The
 * severity-tagged sysLogPrintf path below uses its actual numeric level. */
static unsigned char QuietAsciiLower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}
static int QuietContains(const char *text, const char *needle) {
    if (!text || !needle || !*needle) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        const unsigned char *x = p;
        const unsigned char *y = (const unsigned char *)needle;
        while (*x && *y && QuietAsciiLower(*x) == QuietAsciiLower(*y)) { ++x; ++y; }
        if (!*y) return 1;
    }
    return 0;
}
static int QuietActionableText(const char *message) {
    static const char *const terms[] = {
        "error", "warn", "fatal", "assert", "failed", "failure", "invalid",
        "corrupt", "overflow", "underflow", "overrun", "underrun",
        "out of memory", "out of samples", "out of bounds", "not found",
        "missing", "unknown", "unsupported", "runaway", "cannot",
        "unable", "no free", "no valid", "stuck", "mismatch", "desync",
        "bad pointer", "null pointer", "allocation failed", "dma not done"
    };
    for (unsigned i = 0; i < sizeof(terms) / sizeof(terms[0]); ++i)
        if (QuietContains(message, terms[i])) return 1;
    return 0;
}
/* Some profiling/status call sites used TraceIssue for persistence. Only
 * exclude known routine record headers; preserve all other TraceIssue calls
 * (including VIDEO PRESENT_FAILED, STARTUP FAILED, FATAL, replay mismatch). */
static int QuietRoutineIssue(const char *format) {
    if (!format) return 0;
    static const char *const prefixes[] = {
        "PH0 ", "PH0B ", "PH0E ",
        "SCHED VI stage_enter=", "SCHED GFX_WORKER ",
        "F3D PREP stage_enter=", "F3D TEX stage_enter=",
        "NV2A PATH stage_enter=", "FRAME DIP "
    };
    for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        const size_t len = strlen(prefixes[i]);
        if (strncmp(format, prefixes[i], len) == 0) return 1;
    }
    return 0;
}
#endif /* GE_XBOX_WARN_ERROR_ONLY */
extern "C" void *geNativeStdStream(int index) {
    /* Opaque sentinels; engine-facing FILE never depends on the modern CRT's
     * FILE layout or __acrt_iob_func. Only these wrappers consume them. */
    return index >= 0 && index <= 2 ? (void *)(uintptr_t)(index + 1) : 0;
}
static int PrintStream(void *stream, const char *format, va_list args) {
    char buffer[2048];
    int n = Format(buffer, sizeof(buffer), format, args);
    const size_t actual = strlen(buffer);
    if ((uintptr_t)stream >= 1u && (uintptr_t)stream <= 3u) {
#if GE_XBOX_WARN_ERROR_ONLY
        if (QuietActionableText(buffer)) WriteLog(buffer);
#else
        WriteLog(buffer);
#endif
    } else if (!stream || fwrite(buffer, 1, actual, (FILE *)stream) != actual) return -1;
    return n >= 0 && (size_t)n < sizeof(buffer) ? n : -1;
}
extern "C" int geNativeFprintf(void *stream, const char *format, ...) {
    va_list ap; va_start(ap, format); int n = PrintStream(stream, format, ap); va_end(ap); return n;
}
extern "C" int geNativePrintf(const char *format, ...) {
    va_list ap; va_start(ap, format); int n = PrintStream(geNativeStdStream(1), format, ap); va_end(ap); return n;
}
extern "C" int geNativeFflush(void *stream) {
    if ((uintptr_t)stream >= 1u && (uintptr_t)stream <= 3u) return 0;
    return fflush((FILE *)stream);
}
extern "C" int geNativeSetvbuf(void *stream, char *buffer, int mode, size_t bytes) {
    if ((uintptr_t)stream >= 1u && (uintptr_t)stream <= 3u) return 0;
    return stream ? setvbuf((FILE *)stream, buffer, mode, bytes) : -1;
}
extern "C" void sysLogPrintf(int level, const char *fmt, ...) {
#if GE_XBOX_WARN_ERROR_ONLY
    /* port/include/platform.h: ERROR=0, WARNING=1, NOTE=2, INFO=3, DEBUG=4. */
    if (level > 1) return;
#else
    (void)level;
#endif
    char buffer[1024]; va_list ap; va_start(ap, fmt); Format(buffer, sizeof(buffer), fmt, ap); va_end(ap);
    WriteLog(buffer);
}
extern "C" void osSyncPrintf(const char *fmt, ...) {
    char buffer[1024]; va_list ap; va_start(ap, fmt); Format(buffer, sizeof(buffer), fmt, ap); va_end(ap);
#if GE_XBOX_WARN_ERROR_ONLY
    if (!QuietActionableText(buffer)) return;
#endif
    WriteLog(buffer);
}
extern "C" void sysFatalError(const char *fmt, ...) {
    char buffer[512]; va_list ap; va_start(ap, fmt); Format(buffer, sizeof(buffer), fmt, ap); va_end(ap);
    GENativePlatform_SetError(buffer);
    s_fatal = 1;
    WriteLog(buffer);
    /* SetError released the platform lock. The trace has its own lock and
     * never raises an engine fatal if disk I/O fails. */
    GENativePlatform_TraceIssue("FATAL %s", buffer);
    if (s_fatalHook) s_fatalHook();
    /* The host remains alive to display the failure. Never continue through
     * an invalid ROM pointer or failed allocation, and never free live RAM. */
    for (;;) sysSleep(1000000u);
}
extern "C" void sysExit(int code) {
#if defined(XBOX) || defined(_XBOX)
    (void)code; XLaunchNewImage(0, 0);
    for (;;) Sleep(1000u);
#else
    exit(code);
#endif
}
extern "C" void portKernelInit(void) {
    if (!s_runtime || !s_runtime->Ready()) sysFatalError("Native game runtime is not initialized");
}
extern "C" void sysSetArgs(int, char **) {}
extern "C" int sysArgCheck(const char *) { return 0; }
extern "C" const char *sysArgGetString(const char *) { return 0; }
extern "C" const char *sysGetTokenString(void) { return ""; }
extern "C" const char *sysGetExeDir(void) { return "D:\\"; }
extern "C" int sysRelaunchSelf(void) { return -1; }
extern "C" const char *sysResolvePath(const char *path) {
    static char output[8][512];
    static unsigned next = 0;
    if (!path) return "";
    if (s_locksReady) PlatformLock();
    char *out = output[next++ & 7u];
    const char *suffix = path;
    if ((path[0] == '$') && (path[1] == 'S' || path[1] == 'E') && path[2] == '/') {
        CopyString(out, 512, path[1] == 'S' ? s_dataDirectory : "D:");
        size_t n = strlen(out);
        if (n && out[n-1] != '\\' && out[n-1] != '/') {
#if defined(XBOX) || defined(_XBOX)
            out[n++] = '\\';
#else
            out[n++] = '/';
#endif
            out[n] = 0;
        }
        suffix = path + 3;
        CopyString(out + n, 512 - n, suffix);
    } else CopyString(out, 512, path);
#if defined(XBOX) || defined(_XBOX)
    for (char *p = out; *p; ++p) if (*p == '/') *p = '\\';
#endif
    if (s_locksReady) PlatformUnlock();
    return out;
}

/* Preserve the upstream port's 16 Kbit (256 x 8-byte block) save format. A missing file
 * yields a fresh zeroed device, while corrupt/truncated files and I/O
 * failures are errors. Write through a temporary file and keep the previous
 * image until replacement succeeds, so a failed write cannot truncate it. */
static int PathInDirectory(char *path, size_t capacity, const char *directory, const char *name) {
    size_t dir = strlen(directory), file = strlen(name);
    if (dir + file + 2u > capacity) return 0;
    memcpy(path, directory, dir);
    if (dir && path[dir - 1u] != '/' && path[dir - 1u] != '\\') {
#if defined(XBOX) || defined(_XBOX)
        path[dir++] = '\\';
#else
        path[dir++] = '/';
#endif
    }
    memcpy(path + dir, name, file + 1u);
    return 1;
}
static int SavePath(char *path, size_t capacity, const char *name) {
    return PathInDirectory(path, capacity, s_dataDirectory, name);
}
static int EepromPath(char *path, size_t capacity, const char *name) {
    return s_eepromDirectory[0] &&
        PathInDirectory(path, capacity, s_eepromDirectory, name);
}
static int FilePresence(const char *path) {
#if defined(XBOX) || defined(_XBOX)
    DWORD attr = GetFileAttributesA(path);
    /* Classic RXDK omits the INVALID_FILE_ATTRIBUTES convenience macro. */
    if (attr != (DWORD)0xFFFFFFFFu) return (attr & FILE_ATTRIBUTE_DIRECTORY) ? -1 : 1;
    DWORD code = GetLastError();
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ? 0 : -1;
#else
    if (access(path, F_OK) == 0) return 1;
    return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
#endif
}
static int EepromDirectoryLocked(void) {
    if (s_eepromDirectory[0]) return 1;
#if defined(XBOX) || defined(_XBOX)
    /* Game progress belongs to the current title's persistent U: container.
     * D: remains the asset/legacy-import location and may be read-only.
     * This is the same RXDK container API used by PokeRBXB. */
    char directory[MAX_PATH];
    memset(directory, 0, sizeof(directory));
    DWORD result = XCreateSaveGame("U:\\", L"007XB", OPEN_ALWAYS,
        0, directory, sizeof(directory));
    if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS) {
        CopyString(s_error, sizeof(s_error), "Cannot create the Xbox game-save container");
        return 0;
    }
    size_t n = 0;
    while (n < sizeof(directory) && directory[n]) ++n;
    if (!n || n >= sizeof(directory) - 1u || n >= sizeof(s_eepromDirectory)) {
        CopyString(s_error, sizeof(s_error), "The Xbox game-save path is invalid");
        return 0;
    }
    DWORD attributes = GetFileAttributesA(directory);
    if (attributes == (DWORD)0xFFFFFFFFu || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        CopyString(s_error, sizeof(s_error), "The Xbox game-save directory is unavailable");
        return 0;
    }
    CopyString(s_eepromDirectory, sizeof(s_eepromDirectory), directory);
#else
    CopyString(s_eepromDirectory, sizeof(s_eepromDirectory), s_dataDirectory);
#endif
    return 1;
}
static int EepromReadImage(const char *path, unsigned char *image) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        CopyString(s_error, sizeof(s_error), "Cannot read ge007.eep save data");
        return 0;
    }
    size_t got = fread(image, 1, sizeof(s_eeprom), fp);
    int extra = fgetc(fp), error = ferror(fp);
    if (fclose(fp) != 0) error = 1;
    if (got != sizeof(s_eeprom) || extra != EOF || error) {
        CopyString(s_error, sizeof(s_error), "ge007.eep is not a valid 2048-byte save image");
        return 0;
    }
    return 1;
}
static int EepromStoreLocked(const unsigned char *next) {
    char path[512], temporary[512], backup[512];
    if (!EepromPath(path, sizeof(path), "ge007.eep") ||
        !EepromPath(temporary, sizeof(temporary), "ge007.eep.tmp") ||
        !EepromPath(backup, sizeof(backup), "ge007.eep.bak")) {
        CopyString(s_error, sizeof(s_error), "The game-save path is too long");
        return 0;
    }
#if defined(XBOX) || defined(_XBOX)
    HANDLE fp = CreateFileA(temporary, GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, 0);
    const int temporaryCreated = fp != INVALID_HANDLE_VALUE;
    int ok = temporaryCreated;
    if (temporaryCreated) {
        DWORD written = 0;
        ok = WriteFile(fp, next, sizeof(s_eeprom), &written, 0) &&
            written == sizeof(s_eeprom);
        if (ok && !FlushFileBuffers(fp)) ok = 0;
        if (!CloseHandle(fp)) ok = 0;
    }
#else
    FILE *fp = fopen(temporary, "wb");
    const int temporaryCreated = fp != 0;
    int ok = fp != 0;
    if (fp) {
        ok = fwrite(next, 1, sizeof(s_eeprom), fp) == sizeof(s_eeprom);
        if (fflush(fp) != 0) ok = 0;
        if (ok && fsync(fileno(fp)) != 0) ok = 0;
        if (fclose(fp) != 0) ok = 0;
    }
#endif
    /* Confirm the complete, closed image before touching the last save.
     * Keep the existing raw 2048-byte format and its game-owned slot CRCs. */
    unsigned char verify[2048];
    if (ok) ok = EepromReadImage(temporary, verify) &&
        memcmp(verify, next, sizeof(verify)) == 0;
    int savedOld = 0;
    if (ok) {
        const int exists = FilePresence(path);
        if (exists < 0) ok = 0;
        else if (exists > 0) {
            if (FilePresence(backup) != 0 && remove(backup) != 0) ok = 0;
            if (ok) { savedOld = rename(path, backup) == 0; ok = savedOld; }
        }
    }
    if (ok && rename(temporary, path) != 0) {
        ok = 0;
        if (savedOld) rename(backup, path);
    }
    if (ok) {
        if (savedOld) remove(backup);
    } else {
        if (temporaryCreated) remove(temporary);
        CopyString(s_error, sizeof(s_error), "Cannot write ge007.eep save data");
    }
    return ok;
}
static int EepromLoadLocked(void) {
    if (s_eepromState) return s_eepromState > 0;
    s_eepromState = -1; /* An unreadable save must never become a blank one. */
    if (!EepromDirectoryLocked()) return 0;
    char path[512], backup[512];
    if (!EepromPath(path, sizeof(path), "ge007.eep") ||
        !EepromPath(backup, sizeof(backup), "ge007.eep.bak")) return 0;
    int present = FilePresence(path), old = FilePresence(backup);
    if (present < 0 || old < 0) {
        CopyString(s_error, sizeof(s_error), "Cannot inspect ge007.eep save data");
        return 0;
    }
    if (present == 0 && old == 0) {
#if defined(XBOX) || defined(_XBOX)
        /* Import once only when the new container has no image. Preserve
         * the legacy main/backup files and never replace an existing U: save. */
        char legacy[512], legacyBackup[512];
        if (!SavePath(legacy, sizeof(legacy), "ge007.eep") ||
            !SavePath(legacyBackup, sizeof(legacyBackup), "ge007.eep.bak")) return 0;
        int legacyPresent = FilePresence(legacy), legacyOld = FilePresence(legacyBackup);
        if (legacyPresent < 0 || legacyOld < 0) {
            CopyString(s_error, sizeof(s_error), "Cannot inspect the existing game save");
            return 0;
        }
        if (legacyPresent || legacyOld) {
            if (!EepromReadImage(legacyPresent ? legacy : legacyBackup, s_eeprom) ||
                !EepromStoreLocked(s_eeprom)) return 0;
            s_eepromState = 1;
            return 1;
        }
#endif
        memset(s_eeprom, 0, sizeof(s_eeprom));
        s_eepromState = 1;
        return 1;
    }
    if (!EepromReadImage(present ? path : backup, s_eeprom)) return 0;
    if (!present && rename(backup, path) != 0) {
        CopyString(s_error, sizeof(s_error), "Cannot restore the previous game save");
        return 0;
    }
    s_eepromState = 1;
    return 1;
}
static int EepromRW(unsigned char block, unsigned char *bytes, int count, int write) {
    const unsigned offset = (unsigned)block * 8u;
    if (!bytes || count < 0 || (unsigned)count > sizeof(s_eeprom) - offset) return -1;
    InitLocks(); PlatformLock();
    if (!EepromLoadLocked()) { PlatformUnlock(); return -1; }
    if (!write) {
        memcpy(bytes, s_eeprom + offset, (size_t)count);
        PlatformUnlock(); return 0;
    }
    unsigned char next[2048]; memcpy(next, s_eeprom, sizeof(next));
    memcpy(next + offset, bytes, (size_t)count);
    int ok = EepromStoreLocked(next);
    if (ok) memcpy(s_eeprom, next, sizeof(next));
    PlatformUnlock(); return ok ? 0 : -1;
}
extern "C" int osEepromProbe(void *) {
    InitLocks(); PlatformLock();
    int available = EepromLoadLocked();
    PlatformUnlock();
    return available ? 2 : 0;
}
extern "C" int osEepromRead(void *, unsigned char block, unsigned char *out) { return EepromRW(block, out, 8, 0); }
extern "C" int osEepromWrite(void *, unsigned char block, unsigned char *in) { return EepromRW(block, in, 8, 1); }
extern "C" int osEepromLongRead(void *, unsigned char block, unsigned char *out, int n) { return EepromRW(block, out, n, 0); }
extern "C" int osEepromLongWrite(void *, unsigned char block, unsigned char *in, int n) { return EepromRW(block, in, n, 1); }

/* No N64 Memory/Rumble Pak is attached to the SI channels. Return the
 * actual unavailable-device result; do not claim successful rumble output. */
extern "C" int osPfsInit(void *, void *, int) { return 1; }
extern "C" int osPfsIsPlug(void *, unsigned char *mask) { if (mask) *mask = 0; return 0; }
extern "C" int osMotorInit(void *, void *, int) { return 1; }
extern "C" int osMotorStart(void *) { return 1; }
extern "C" int osMotorStop(void *) { return 1; }

static void TraceLock(void) {
#if defined(XBOX) || defined(_XBOX)
    EnterCriticalSection(&s_traceLock);
#else
    pthread_mutex_lock(&s_traceLock);
#endif
}
static void TraceUnlock(void) {
#if defined(XBOX) || defined(_XBOX)
    LeaveCriticalSection(&s_traceLock);
#else
    pthread_mutex_unlock(&s_traceLock);
#endif
}
static void TraceQueueLock(void) {
#if defined(XBOX) || defined(_XBOX)
    EnterCriticalSection(&s_traceQueueLock);
#else
    pthread_mutex_lock(&s_traceQueueLock);
#endif
}
static void TraceQueueUnlock(void) {
#if defined(XBOX) || defined(_XBOX)
    LeaveCriticalSection(&s_traceQueueLock);
#else
    pthread_mutex_unlock(&s_traceQueueLock);
#endif
}
static void TraceWakeWriterLocked(void) {
#if defined(XBOX) || defined(_XBOX)
    if (s_traceWake) SetEvent(s_traceWake);
#else
    pthread_cond_signal(&s_traceWake);
#endif
}
static void TraceAddCount(unsigned *value, unsigned count) {
    *value = count > 0xFFFFFFFFu - *value ? 0xFFFFFFFFu : *value + count;
}
/* The host queries status at frame end. Publish path/error changes without
 * making that reader wait for a synchronous disk flush. During a run, the
 * path is immutable and the error is finalized before ready becomes false. */
static void TracePublish(int ready) {
#if defined(XBOX) || defined(_XBOX)
    InterlockedExchange(const_cast<LONG *>(&s_traceReady), (LONG)ready);
#else
    __atomic_store_n(&s_traceReady, ready, __ATOMIC_RELEASE);
#endif
}
static int TraceFailure(const char *operation, unsigned long code) {
    geXboxSnprintf(s_traceError, sizeof(s_traceError),
        "007XB trace: %s (0x%08lX)", operation, code);
    TracePublish(0);
    WriteLog(s_traceError);
    return 0;
}
static void TraceMemory(unsigned long *total, unsigned long *available) {
#if defined(XBOX) || defined(_XBOX)
    MEMORYSTATUS status;
    memset(&status, 0, sizeof(status));
    status.dwLength = sizeof(status);
    GlobalMemoryStatus(&status);
    *total = status.dwTotalPhys;
    *available = status.dwAvailPhys;
#else
    const long page = sysconf(_SC_PAGESIZE);
    const long all = sysconf(_SC_PHYS_PAGES);
    const long freePages = sysconf(_SC_AVPHYS_PAGES);
    *total = page > 0 && all > 0 ? (unsigned long)page * (unsigned long)all : 0u;
    *available = page > 0 && freePages > 0 ? (unsigned long)page * (unsigned long)freePages : 0u;
#endif
}
static unsigned long TraceMilliseconds(void) {
#if defined(XBOX) || defined(_XBOX)
    return GetTickCount();
#else
    return (unsigned long)(sysGetMicroseconds() / 1000u);
#endif
}

/* Every disk writer holds s_traceLock. The latest failure evidence wins:
 * cap the issue log at 256 KiB and restart it when the next complete record
 * would overflow, including when an old build left a multi-MB trace file.
 * Record writes remain serialized, flushed, and checked. No background
 * writer starts in the default issue-only build. */
static int TraceAppend(const char *text, unsigned bytes, int verify) {
    const char *failure = 0;
    unsigned long code = 0, offset = 0;
#if defined(XBOX) || defined(_XBOX)
    HANDLE file = CreateFileA(s_tracePath, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, 0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) return TraceFailure("CreateFileA failed", GetLastError());
    offset = GetFileSize(file, 0);
    if (offset == (DWORD)0xFFFFFFFFu) {
        failure = "GetFileSize failed"; code = GetLastError();
    }
    if (!failure && bytes > kTraceLimit)
        failure = "issue record exceeds 256 KiB limit";
    if (!failure && (offset > kTraceLimit || bytes > kTraceLimit - offset)) {
        /* Close before reopening with CREATE_ALWAYS. This is supported by
         * the same old XDK API used by normal issue writes; no rename or
         * unbounded rollover archive is needed on the console. */
        if (!CloseHandle(file)) {
            failure = "rollover close failed"; code = GetLastError();
            file = INVALID_HANDLE_VALUE;
        } else {
            file = CreateFileA(s_tracePath, GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
            if (file == INVALID_HANDLE_VALUE) {
                failure = "rollover recreate failed"; code = GetLastError();
            } else offset = 0;
        }
    }
    if (!failure && SetFilePointer(file, (LONG)offset, 0, FILE_BEGIN) != offset) {
        failure = "append seek failed"; code = GetLastError();
    }
    DWORD written = 0;
    if (!failure && (!WriteFile(file, text, bytes, &written, 0) || written != bytes)) {
        failure = "WriteFile failed or short"; code = GetLastError();
    }
    if (!failure && !FlushFileBuffers(file)) {
        failure = "FlushFileBuffers failed"; code = GetLastError();
    }
    if (file != INVALID_HANDLE_VALUE && !CloseHandle(file) && !failure) {
        failure = "CloseHandle failed"; code = GetLastError();
    }
#else
    FILE *file = fopen(s_tracePath, "ab");
    if (!file) return TraceFailure("open failed", (unsigned long)errno);
    long end = -1;
    if (fseek(file, 0, SEEK_END) != 0 || (end = ftell(file)) < 0) {
        failure = "append seek failed"; code = (unsigned long)errno;
    } else offset = (unsigned long)end;
    if (!failure && bytes > kTraceLimit)
        failure = "issue record exceeds 256 KiB limit";
    if (!failure && (offset > kTraceLimit || bytes > kTraceLimit - offset)) {
        if (fclose(file) != 0) {
            failure = "rollover close failed"; code = (unsigned long)errno;
        }
        /* A failed rollover close must not be closed again below. */
        file = NULL;
        if (!failure) {
            file = fopen(s_tracePath, "wb");
            if (!file) { failure = "rollover reopen failed"; code = (unsigned long)errno; }
            else offset = 0;
        }
    }
    if (!failure && fwrite(text, 1, bytes, file) != bytes) {
        failure = "write failed or short"; code = (unsigned long)errno;
    }
    if (!failure && (fflush(file) != 0 || fsync(fileno(file)) != 0)) {
        failure = "flush failed"; code = (unsigned long)errno;
    }
    if (file && fclose(file) != 0 && !failure) {
        failure = "close failed"; code = (unsigned long)errno;
    }
#endif
    if (failure) return TraceFailure(failure, code);
    if (verify) {
        char check[512];
        if (bytes > sizeof(check)) return TraceFailure("BOOT record too large", 0);
#if defined(XBOX) || defined(_XBOX)
        file = CreateFileA(s_tracePath, GENERIC_READ, FILE_SHARE_READ,
            0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
        if (file == INVALID_HANDLE_VALUE) return TraceFailure("readback open failed", GetLastError());
        DWORD got = 0;
        if (SetFilePointer(file, (LONG)offset, 0, FILE_BEGIN) != offset ||
            !ReadFile(file, check, bytes, &got, 0) || got != bytes) {
            failure = "BOOT readback failed"; code = GetLastError();
        }
        if (!CloseHandle(file) && !failure) {
            failure = "readback close failed"; code = GetLastError();
        }
#else
        file = fopen(s_tracePath, "rb");
        if (!file) return TraceFailure("readback open failed", (unsigned long)errno);
        if (fseek(file, (long)offset, SEEK_SET) != 0 || fread(check, 1, bytes, file) != bytes) {
            failure = "BOOT readback failed"; code = (unsigned long)errno;
        }
        if (fclose(file) != 0 && !failure) {
            failure = "readback close failed"; code = (unsigned long)errno;
        }
#endif
        if (!failure && memcmp(check, text, bytes) != 0) failure = "BOOT readback mismatch";
        if (failure) return TraceFailure(failure, code);
    }
    return 1;
}

/* Format on the producer, before any disk lock: queued records must retain
 * the occurrence time and originating thread/stack, not the writer's. */
static const char *TraceFormatRecord(TraceRecord *record,
    const char *format, va_list args, int issue) {
    unsigned long total = 0, available = 0;
    unsigned stackBase = 0, stackLimit = 0, stackPointer = 0;
    const unsigned long milliseconds = TraceMilliseconds();
    const unsigned long thread = (unsigned long)ThreadIdentifier();
    const int stackValid = GEOSGetCurrentStackSnapshot(&stackBase, &stackLimit, &stackPointer);
    TraceMemory(&total, &available);
    /* This is remaining stack at the logger, not a frame's high-water mark.
     * The native helper only reads the current thread's bounds; it never
     * grows a stack or treats spare physical RAM as spare stack space. */
    int prefix = geXboxSnprintf(record->text, sizeof(record->text),
        "ms=%lu avail_phys_bytes=%lu thread=%08lX stack_valid=%d stack_free_bytes=%u stack_size_bytes=%u ",
        milliseconds, available, thread, stackValid,
        stackValid ? stackPointer - stackLimit : 0u,
        stackValid ? stackBase - stackLimit : 0u);
    if (prefix < 0 || (unsigned)prefix >= sizeof(record->text) - 2u)
        return "record prefix format failed";
    if (issue) {
        /* Quiet launches have no BOOT record. Keep each actual failure
         * self-identifying without a separate startup write or readback. */
        const size_t remaining = sizeof(record->text) - (size_t)prefix;
        int metadata = geXboxSnprintf(record->text + prefix, remaining,
            "trace_version=4 build=%s_%s total_phys_bytes=%lu ",
            __DATE__, __TIME__, total);
        if (metadata < 0 || (size_t)metadata >= remaining - 2u)
            return "issue metadata format failed";
        prefix += metadata;
    }
    const size_t capacity = sizeof(record->text) - (size_t)prefix - 1u;
    int length = Format(record->text + prefix, capacity, format, args);
    if (length < 0 || (size_t)length >= capacity)
        return "record exceeds 2047 bytes";
    unsigned bytes = (unsigned)(prefix + length);
    if (!bytes || record->text[bytes - 1u] != '\n') record->text[bytes++] = '\n';
    record->bytes = bytes;
    return 0;
}
static int TraceInternalLocked(const char *format, ...) {
    if (!GENativePlatform_TraceReady()) return 0;
    TraceRecord record;
    va_list args; va_start(args, format);
    const char *failure = TraceFormatRecord(&record, format, args, 0);
    va_end(args);
    return failure ? TraceFailure(failure, 0) : TraceAppend(record.text, record.bytes, 0);
}
/* Take the disk lock BEFORE removing records. A synchronous fatal/stage
 * writer cannot overtake a background batch that has already been popped.
 * Bound each drain to the records present at entry so an active producer
 * cannot indefinitely delay an urgent record. */
static int TraceDrainLocked(void) {
    TraceQueueLock();
    unsigned count = s_traceQueueCount;
    const unsigned drops = s_tracePendingDrops, invalid = s_tracePendingInvalid;
    const unsigned droppedTotal = s_traceQueueDropped;
    s_tracePendingDrops = s_tracePendingInvalid = 0;
    TraceQueueUnlock();
    while (count--) {
        TraceRecord record;
        TraceQueueLock();
        record = s_traceQueue[s_traceQueueFirst];
        s_traceQueueFirst = (s_traceQueueFirst + 1u) % TRACE_QUEUE_CAPACITY;
        --s_traceQueueCount;
        TraceQueueUnlock();
        if (!GENativePlatform_TraceReady() || !TraceAppend(record.text, record.bytes, 0)) {
            TraceQueueLock();
            TraceAddCount(&s_traceQueueDropped, 1u + s_traceQueueCount);
            s_traceQueueFirst = s_traceQueueCount = 0;
            s_tracePendingDrops = s_tracePendingInvalid = 0;
            TraceQueueUnlock();
            return 0;
        }
    }
    if (drops && !TraceInternalLocked(
        "TRACE DEFERRED_DROPPED count=%u queue_full=%u invalid_record=%u total=%u\n",
        drops, drops - invalid, invalid, droppedTotal)) return 0;
    return GENativePlatform_TraceReady();
}
static int TraceWriteSync(const TraceRecord *record, const char *failure) {
    TraceLock();
    int ok = GENativePlatform_TraceReady() && TraceDrainLocked();
    if (ok) ok = failure ? TraceFailure(failure, 0) : TraceAppend(record->text, record->bytes, 0);
    TraceUnlock();
    return ok;
}
#if GE_NATIVE_TRACE_OUTPUT_VERBOSE
static void TraceWriterLoop(void) {
    for (;;) {
        int pending, stop;
        unsigned long waitError = 0;
        TraceQueueLock();
#if !defined(XBOX) && !defined(_XBOX)
        while (!s_traceWriterStop && !s_traceQueueCount && !s_tracePendingDrops) {
            int result = pthread_cond_wait(&s_traceWake, &s_traceQueueLock);
            if (result) { waitError = (unsigned long)result; break; }
        }
#endif
        pending = s_traceQueueCount || s_tracePendingDrops;
        stop = s_traceWriterStop;
        TraceQueueUnlock();
        if (!pending && stop) return;
#if defined(XBOX) || defined(_XBOX)
        if (!pending && WaitForSingleObject(s_traceWake, INFINITE) != WAIT_OBJECT_0) {
            waitError = GetLastError();
            if (!waitError) waitError = 1;
        }
#endif
        if (waitError) {
            TraceLock();
            TraceQueueLock();
            if (!s_traceWriterStop) s_traceDeferredMode = TRACE_DEFERRED_SYNC;
            TraceQueueUnlock();
            TraceDrainLocked();
            TraceInternalLocked("TRACE DEFERRED mode=synchronous reason=wait_failed code=%lu\n", waitError);
            TraceUnlock();
            return;
        }
        if (pending) {
            TraceLock();
            TraceDrainLocked();
            TraceUnlock();
        }
    }
}
#if defined(XBOX) || defined(_XBOX)
static DWORD WINAPI TraceWriterEntry(LPVOID) { TraceWriterLoop(); return 0; }
#else
static void *TraceWriterEntry(void *) { TraceWriterLoop(); return 0; }
#endif
static void TraceStartWriter(void) {
    unsigned long code = 0;
    const char *reason = "ready";
    /* Bind has no producers yet. Keep the new worker behind this lock until
     * its initial mode is published; an immediate wait failure may then
     * switch to synchronous mode without startup overwriting that decision. */
    TraceQueueLock();
#if defined(XBOX) || defined(_XBOX)
    s_traceWake = CreateEvent(0, FALSE, FALSE, 0);
    if (!s_traceWake) { code = GetLastError(); reason = "event_creation_failed"; }
    else {
        s_traceWriter = CreateThread(0, TRACE_WRITER_STACK_BYTES, TraceWriterEntry, 0, 0, 0);
        if (!s_traceWriter) {
            code = GetLastError(); reason = "thread_creation_failed";
            CloseHandle(s_traceWake); s_traceWake = 0;
        } else s_traceWriterStarted = 1;
    }
#else
    pthread_attr_t attributes;
    int result = pthread_attr_init(&attributes);
    if (!result) {
        result = pthread_attr_setstacksize(&attributes, TRACE_WRITER_STACK_BYTES);
        if (!result) result = pthread_create(&s_traceWriter, &attributes, TraceWriterEntry, 0);
        pthread_attr_destroy(&attributes);
    }
    if (result) { code = (unsigned long)result; reason = "thread_creation_failed"; }
    else s_traceWriterStarted = 1;
#endif
    s_traceDeferredMode = s_traceWriterStarted ? TRACE_DEFERRED_ASYNC : TRACE_DEFERRED_SYNC;
    TraceQueueUnlock();
    /* Thread/resource failure preserves progress diagnostics through an
     * explicit synchronous fallback instead of silently dropping all logs. */
    TraceLock();
    TraceQueueLock();
    const int mode = s_traceDeferredMode;
    TraceQueueUnlock();
    /* If an immediate worker wait failure already announced fallback, do
     * not append a later, misleading startup record claiming async mode. */
    if (!s_traceWriterStarted || mode == TRACE_DEFERRED_ASYNC)
        TraceInternalLocked("TRACE DEFERRED mode=%s capacity=%u stack_bytes=%u reason=%s code=%lu\n",
            s_traceWriterStarted ? "async" : "synchronous", (unsigned)TRACE_QUEUE_CAPACITY,
            (unsigned)TRACE_WRITER_STACK_BYTES, reason, code);
    TraceUnlock();
}
#endif
extern "C" int GENativePlatform_TraceShutdown(void) {
    if (!s_locksReady) return 1;
    TraceQueueLock();
    s_traceDeferredMode = TRACE_DEFERRED_STOPPED;
    s_traceWriterStop = 1;
    TraceWakeWriterLocked();
    TraceQueueUnlock();
    if (s_traceWriterStarted) {
        unsigned long failure = 0;
#if defined(XBOX) || defined(_XBOX)
        if (WaitForSingleObject(s_traceWriter, INFINITE) != WAIT_OBJECT_0) {
            failure = GetLastError();
            if (!failure) failure = 1;
        }
#else
        failure = (unsigned long)pthread_join(s_traceWriter, 0);
#endif
        if (failure) {
            TraceLock(); TraceFailure("deferred writer join failed", failure); TraceUnlock();
            return 0;
        }
#if defined(XBOX) || defined(_XBOX)
        CloseHandle(s_traceWriter); s_traceWriter = 0;
        CloseHandle(s_traceWake); s_traceWake = 0;
#endif
        s_traceWriterStarted = 0;
    }
    TraceLock();
    TraceDrainLocked();
    if (GENativePlatform_TraceReady())
        CopyString(s_traceError, sizeof(s_traceError), "Trace storage has been shut down");
    TracePublish(0);
    TraceUnlock();
    return 1;
}
static void InitTraceStorage(void) {
    /* Bind is a host lifecycle boundary, before concurrent producers exist. */
    if (!GENativePlatform_TraceShutdown()) return;
    TraceQueueLock();
    s_traceQueueFirst = s_traceQueueCount = s_traceQueueDropped = 0;
    s_tracePendingDrops = s_tracePendingInvalid = 0;
    s_traceWriterStop = 0;
    TraceQueueUnlock();
    TraceLock();
    TracePublish(0);
    s_tracePath[0] = s_traceError[0] = 0;
#if defined(XBOX) || defined(_XBOX)
    /* Keep diagnostics beside the XBE. EEPROM saves have their own U:
     * container; trace I/O reports a D: failure without selecting a fallback. */
    CopyString(s_tracePath, sizeof(s_tracePath), "D:\\007xb-trace.log");
#else
    if (!SavePath(s_tracePath, sizeof(s_tracePath), "007xb-trace.log")) {
        TraceFailure("save path is too long", 0); TraceUnlock(); return;
    }
#endif
#if GE_NATIVE_TRACE_OUTPUT_VERBOSE
    unsigned long total = 0, available = 0;
    TraceMemory(&total, &available);
    char boot[512];
    int length = geXboxSnprintf(boot, sizeof(boot),
        "ms=%lu avail_phys_bytes=%lu BOOT trace_version=3 build=%s_%s total_phys_bytes=%lu path=%s\n",
        TraceMilliseconds(), available, __DATE__, __TIME__, total, s_tracePath);
    if (length < 0 || (unsigned)length >= sizeof(boot))
        TraceFailure("BOOT format failed", 0);
    else if (TraceAppend(boot, (unsigned)length, 1)) TracePublish(1);
    TraceUnlock();
    if (GENativePlatform_TraceReady()) TraceStartWriter();
#else
    /* Storage is opened by the first issue, never by a healthy launch.
     * No routine producer needs a queue worker in this mode. */
    TracePublish(1);
    TraceUnlock();
#endif
}
static int TraceV(const char *format, va_list args, int issue) {
    if (!format || !s_locksReady || !GENativePlatform_TraceReady()) return 0;
    TraceRecord record;
    const char *failure = TraceFormatRecord(&record, format, args, issue);
    return TraceWriteSync(&record, failure);
}
extern "C" int GENativePlatform_TraceIssue(const char *format, ...) {
#if GE_XBOX_WARN_ERROR_ONLY
    /* Do not open/create the persistent trace for ordinary profiler/status
     * snapshots. Return accepted as with suppressed routine tracing. */
    if (QuietRoutineIssue(format)) return 1;
#endif
    va_list args; va_start(args, format);
    int ok = TraceV(format, args, 1);
    va_end(args); return ok;
}
extern "C" int GENativePlatform_Trace(const char *format, ...) {
#if GE_NATIVE_TRACE_OUTPUT_VERBOSE
    va_list args; va_start(args, format);
    int ok = TraceV(format, args, 0);
    va_end(args); return ok;
#else
    return format != 0;
#endif
}
extern "C" int GENativePlatform_TraceDeferred(const char *format, ...) {
#if GE_NATIVE_TRACE_OUTPUT_VERBOSE
    if (!format || !s_locksReady || !GENativePlatform_TraceReady()) return 0;
    TraceRecord record;
    va_list args; va_start(args, format);
    const char *failure = TraceFormatRecord(&record, format, args, 0);
    va_end(args);
    TraceQueueLock();
    const int mode = s_traceDeferredMode;
    if (mode == TRACE_DEFERRED_SYNC) {
        TraceQueueUnlock();
        return TraceWriteSync(&record, failure);
    }
    if (mode != TRACE_DEFERRED_ASYNC || !GENativePlatform_TraceReady()) {
        TraceQueueUnlock(); return 0;
    }
    if (failure || s_traceQueueCount == TRACE_QUEUE_CAPACITY) {
        TraceAddCount(&s_traceQueueDropped, 1u);
        TraceAddCount(&s_tracePendingDrops, 1u);
        if (failure) TraceAddCount(&s_tracePendingInvalid, 1u);
        TraceWakeWriterLocked();
        TraceQueueUnlock();
        return 0;
    }
    s_traceQueue[(s_traceQueueFirst + s_traceQueueCount) % TRACE_QUEUE_CAPACITY] = record;
    ++s_traceQueueCount;
    TraceWakeWriterLocked();
    TraceQueueUnlock();
    return 1;
#else
    return format != 0;
#endif
}
extern "C" int GENativePlatform_TraceDeferredReady(void) {
#if GE_NATIVE_TRACE_OUTPUT_VERBOSE
    if (!s_locksReady) return 0;
    TraceQueueLock();
    int ready = s_traceDeferredMode == TRACE_DEFERRED_ASYNC && GENativePlatform_TraceReady();
    TraceQueueUnlock();
    return ready;
#else
    return 0;
#endif
}
extern "C" unsigned GENativePlatform_TraceDeferredDropped(void) {
#if GE_NATIVE_TRACE_OUTPUT_VERBOSE
    if (!s_locksReady) return 0;
    TraceQueueLock();
    unsigned dropped = s_traceQueueDropped;
    TraceQueueUnlock();
    return dropped;
#else
    return 0;
#endif
}
extern "C" int GENativePlatform_TraceReady(void) {
#if defined(XBOX) || defined(_XBOX)
    return InterlockedCompareExchange(const_cast<LONG *>(&s_traceReady), 0, 0) != 0;
#else
    return __atomic_load_n(&s_traceReady, __ATOMIC_ACQUIRE) != 0;
#endif
}
extern "C" const char *GENativePlatform_TracePath(void) { return s_tracePath; }
extern "C" const char *GENativePlatform_TraceError(void) { return s_traceError; }

extern "C" void geTracePrintf(const char *path, const char *fmt, ...) {
#if GE_NATIVE_TRACE_OUTPUT_VERBOSE
    if (!path || !fmt) return;
#if defined(XBOX) || defined(_XBOX)
    /* Legacy callers also use the checked persistent stream. A D: path
     * cannot establish writable storage when the title runs from disc. */
    va_list ap; va_start(ap, fmt); TraceV(fmt, ap, 0); va_end(ap);
#else
    char text[2048]; va_list ap; va_start(ap, fmt); Format(text, sizeof(text), fmt, ap); va_end(ap);
    if (s_locksReady) PlatformLock();
    FILE *fp = fopen(path, "ab");
    if (fp) { fwrite(text, 1, strlen(text), fp); fclose(fp); }
    if (s_locksReady) PlatformUnlock();
#endif
#else
    (void)path; (void)fmt;
#endif
}
