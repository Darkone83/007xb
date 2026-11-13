/* Missing C99 float entrypoints, using the RXDK's original double CRT.
 * GU __sinf/__cosf and the original game's asin/acos/atan2 remain intact. */
#include <math.h>
float sqrtf(float x) { return (float)sqrt((double)x); }
float floorf(float x) { return (float)floor((double)x); }
float ceilf(float x) { return (float)ceil((double)x); }
float fabsf(float x) { return (float)fabs((double)x); }
float fmodf(float x, float y) { return (float)fmod((double)x, (double)y); }
long lroundf(float x) {
    return (long)(x < 0.0f ? ceil((double)x - 0.5) : floor((double)x + 0.5));
}
float tanf(float x) { return (float)tan((double)x); }

#if defined(_MSC_VER) && defined(_M_IX86)
/* MSVC emits this helper for x86 float-to-integer conversions, but the
 * legacy Xbox CRT does not provide it. This is a compiler entrypoint:
 * input is ST(0), output is EDX:EAX, and exactly one x87 value is popped.
 * Truncate toward zero, then restore the caller's complete control word.
 * Use only x87 instructions; the original Xbox does not support SSE2.
 * C name decoration exports the required symbol __ftol2_sse. */
__declspec(naked) void __cdecl _ftol2_sse(void)
{
    __asm {
        sub esp, 16
        fnstcw WORD PTR [esp+8]
        mov ax, WORD PTR [esp+8]
        or ax, 0C00h
        mov WORD PTR [esp+10], ax
        fldcw WORD PTR [esp+10]
        fistp QWORD PTR [esp]
        fldcw WORD PTR [esp+8]
        mov eax, DWORD PTR [esp]
        mov edx, DWORD PTR [esp+4]
        add esp, 16
        ret
    }
}
#endif
