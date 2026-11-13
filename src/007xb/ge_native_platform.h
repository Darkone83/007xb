#ifndef GE_NATIVE_PLATFORM_H
#define GE_NATIVE_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* Routine disk tracing is a deliberate diagnostic-build opt-in. Normal
 * builds write only explicit issues, without a background trace worker. */
#ifndef GE_NATIVE_TRACE_VERBOSE
#define GE_NATIVE_TRACE_VERBOSE 0
#endif

/* The original game is built as 32-bit C; this boundary also serves the
 * C++ Xbox host without importing N64 or Xbox declarations into the other. */
#ifdef __cplusplus
extern "C" {
#endif

/* runtime is GERuntime*, bound before original mainproc is started. */
int GENativePlatform_Bind(void *runtime, const char *data_directory);
const char *GENativePlatform_Error(void);
int GENativePlatform_HasFatalError(void);
void GENativePlatform_SetError(const char *message);
/* Hook runs on the failing thread before it is parked. Register before
 * mainproc so a renderer can release its scene and show the diagnostic. */
void GENativePlatform_SetFatalHook(void (*hook)(void));

/* Bind arms issue logging without opening a file. Xbox writes
 * D:\\007xb-trace.log beside the XBE, independently of the EEPROM's U:
 * container. TraceIssue creates/appends the file only for an explicit
 * failure, then flushes and closes it before returning success. Its record
 * includes build, memory, time, thread and stack context. Storage failure is
 * nonfatal and retained separately from the engine error.
 * TraceReady means armed with no reported storage failure; in the default
 * mode writability is first tested by an actual issue. It is nonblocking;
 * failure text is finalized before readiness becomes false during a run. */
int GENativePlatform_TraceIssue(const char *format, ...);
/* Routine lifecycle/progress messages are suppressed unless the build
 * defines GE_NATIVE_TRACE_VERBOSE=1. Suppression returns one for a non-null
 * format without formatting or touching storage; NULL returns zero. */
int GENativePlatform_Trace(const char *format, ...);
/* Verbose builds only: capture the caller's context, then enqueue a fixed
 * record. Success means accepted, not durable. A full queue rejects the
 * newest record; writer creation failure falls back to synchronous output.
 * Default builds suppress this exactly like Trace, with no writer thread. */
int GENativePlatform_TraceDeferred(const char *format, ...);
int GENativePlatform_TraceDeferredReady(void);
unsigned GENativePlatform_TraceDeferredDropped(void);
/* Host lifecycle only, after producers stop: drain, join and close the writer.
 * Bind also performs this before resetting storage. Returns zero if joining
 * failed; a caller must then retain the runtime used by the writer. */
int GENativePlatform_TraceShutdown(void);
int GENativePlatform_TraceReady(void);
const char *GENativePlatform_TracePath(void);
const char *GENativePlatform_TraceError(void);

void *geNativeDramBase(void);
size_t geNativeDramBytes(void);
int geNativeAddressIsRam(uintptr_t address);
/* True only inside a loaded section of this XBE, using its validated
 * section table. This disambiguates compiled data from N64 segment tokens. */
int geNativeAddressIsNativeImage(uintptr_t address);
void *geNativeAddressToPointer(uintptr_t address);
uint32_t geNativePointerToPhysical(const void *pointer);

/* Explicit arena tokens avoid confusing arena offsets with real Xbox
 * executable/stack addresses, which also occupy the low address range. */
#define GE_NATIVE_DRAM_TOKEN 0x70000000u
#define GE_NATIVE_DRAM_SIZE  0x00800000u

/* Required only by hardware code in the bridge. These expose real storage,
 * not a second game heap or a fabricated framebuffer. */
int GENativePlatform_ReadCartridge(uint32_t address, void *out, uint32_t bytes);
/* The ROM stays in the archive; no fixed mapping or full-ROM copy exists. */
int romdataInit(void);
void romdataDestroy(void);
const unsigned char *romdataGetRom(void);
uint32_t romdataGetRomSize(void);
int romdataCartAddrValid(uint32_t address, uint32_t bytes);
const void *romdataMapVa(uint32_t address);

void *geNativeStdStream(int index);
int geNativeFprintf(void *stream, const char *format, ...);
int geNativeFflush(void *stream);
int geNativeSetvbuf(void *stream, char *buffer, int mode, size_t bytes);
int geNativePrintf(const char *format, ...);
int geXboxVsnprintf(char *out, size_t bytes, const char *format, va_list args);
int geXboxSnprintf(char *out, size_t bytes, const char *format, ...);

#ifdef __cplusplus
}
#endif
#endif
