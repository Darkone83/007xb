#ifndef GE_NATIVE_STDDEF_H
#define GE_NATIVE_STDDEF_H
#if defined(_MSC_VER)
#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef unsigned int size_t;
#endif
#ifndef _PTRDIFF_T_DEFINED
#define _PTRDIFF_T_DEFINED
typedef int ptrdiff_t;
#endif
#if !defined(__cplusplus) && !defined(_WCHAR_T_DEFINED)
#define _WCHAR_T_DEFINED
typedef unsigned short wchar_t;
#endif
#define offsetof(type, member) ((size_t)&(((type *)0)->member))
#else
typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
#ifndef __cplusplus
typedef __WCHAR_TYPE__ wchar_t;
#endif
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif
#ifndef NULL
#ifdef __cplusplus
#define NULL 0
#else
#define NULL ((void *)0)
#endif
#endif
#endif
