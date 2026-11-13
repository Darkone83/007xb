#ifndef GE_NATIVE_STDARG_H
#define GE_NATIVE_STDARG_H
#if defined(_MSC_VER)
/* The Xbox target is x86 cdecl: every stack argument is rounded to 4 bytes. */
#ifndef _VA_LIST_DEFINED
#define _VA_LIST_DEFINED
typedef char *va_list;
#endif
#define GE_VA_BYTES(type) ((sizeof(type) + sizeof(int) - 1) & ~(sizeof(int) - 1))
#define va_start(args, last) ((args) = (char *)&(last) + GE_VA_BYTES(last))
#define va_arg(args, type) (*(type *)(((args) += GE_VA_BYTES(type)) - GE_VA_BYTES(type)))
#define va_end(args) ((args) = (va_list)0)
#define va_copy(dest, src) ((dest) = (src))
#else
typedef __builtin_va_list va_list;
typedef __builtin_va_list __gnuc_va_list;
#define va_start(args, last) __builtin_va_start(args, last)
#define va_arg(args, type) __builtin_va_arg(args, type)
#define va_end(args) __builtin_va_end(args)
#define va_copy(dest, src) __builtin_va_copy(dest, src)
#endif
#endif
