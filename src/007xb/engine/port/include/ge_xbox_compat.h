#ifndef GE_XBOX_COMPAT_H
#define GE_XBOX_COMPAT_H
/* Compiler/CRT boundary for the original 32-bit C engine. */
#if defined(GE_XBOX_NATIVE32)
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
#error GoldenEye Xbox native engine requires a 32-bit compiler target
#endif
#define sprintf ge_sprintf
/* Keep the original engine helpers outside the MSVC intrinsic and CRT
 * namespaces. This forced header remaps declarations, definitions and callers. */
#define ldiv ge_ldiv
#define lldiv ge_lldiv
#define strcpy ge_strcpy
#define strncpy ge_strncpy
#define strcat ge_strcat
#define strcmp ge_strcmp
#define strncmp ge_strncmp
#define strtol ge_strtol
/* Desktop environment overrides have a native configuration boundary. */
#define getenv geNativeGetenv
#if defined(_MSC_VER)
#ifndef __cplusplus
#define inline __inline
#endif
#ifndef __ORDER_LITTLE_ENDIAN__
#define __ORDER_LITTLE_ENDIAN__ 1234
#define __ORDER_BIG_ENDIAN__ 4321
#define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
#endif
#define GE_NOINLINE __declspec(noinline)
#define GE_NORETURN __declspec(noreturn)
#define GE_PRINTF_FORMAT(a,b)
void *_ReturnAddress(void);
#pragma intrinsic(_ReturnAddress)
#define __builtin_return_address(n) _ReturnAddress()
#define __builtin_memset memset
#define __builtin_alignof __alignof
static __inline unsigned short geByteSwap16(unsigned short x) {
    return (unsigned short)((x << 8) | (x >> 8));
}
static __inline unsigned int geByteSwap32(unsigned int x) {
    return ((x & 255u) << 24) | ((x & 65280u) << 8) |
           ((x >> 8) & 65280u) | ((x >> 24) & 255u);
}
static __inline unsigned __int64 geByteSwap64(unsigned __int64 x) {
    return ((unsigned __int64)geByteSwap32((unsigned int)x) << 32) |
           geByteSwap32((unsigned int)(x >> 32));
}
#define __builtin_bswap16 geByteSwap16
#define __builtin_bswap32 geByteSwap32
#define __builtin_bswap64 geByteSwap64
#else
#define GE_NOINLINE __attribute__((noinline))
#define GE_NORETURN __attribute__((noreturn))
#define GE_PRINTF_FORMAT(a,b) __attribute__((format(printf,a,b)))
#endif
#endif
#endif
