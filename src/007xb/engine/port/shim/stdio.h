#ifndef GE_NATIVE_STDIO_H
#define GE_NATIVE_STDIO_H
/* Declaration-only classic CRT ABI: no modern UCRT inline entrypoints. */
#include <stddef.h>
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct _iobuf FILE;
#ifndef EOF
#define EOF (-1)
#endif
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define _IOFBF 0
#define _IOLBF 0x40
#define _IONBF 0x04
#define BUFSIZ 512
void *geNativeStdStream(int stream);
int geNativeFprintf(void *stream, const char *format, ...);
int geNativeFflush(void *stream);
int geNativeSetvbuf(void *stream, char *buffer, int mode, size_t bytes);
int geNativePrintf(const char *format, ...);
int geXboxSnprintf(char *dest, size_t capacity, const char *format, ...);
int geXboxVsnprintf(char *dest, size_t capacity, const char *format, va_list args);
#define stdin ((FILE *)geNativeStdStream(0))
#define stdout ((FILE *)geNativeStdStream(1))
#define stderr ((FILE *)geNativeStdStream(2))
#define fprintf geNativeFprintf
#define fflush geNativeFflush
#define setvbuf geNativeSetvbuf
#define printf geNativePrintf
#define snprintf geXboxSnprintf
#define vsnprintf geXboxVsnprintf
FILE *fopen(const char *name, const char *mode);
int fclose(FILE *stream);
size_t fread(void *dest, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *src, size_t size, size_t count, FILE *stream);
int fseek(FILE *stream, long offset, int origin);
long ftell(FILE *stream);
int ferror(FILE *stream);
int feof(FILE *stream);
int fgetc(FILE *stream);
int fputc(int c, FILE *stream);
char *fgets(char *text, int count, FILE *stream);
int fputs(const char *text, FILE *stream);
int puts(const char *text);
int remove(const char *path);
int rename(const char *oldpath, const char *newpath);
int sscanf(const char *text, const char *format, ...);
int ge_sprintf(char *dest, const char *format, ...);
#ifdef __cplusplus
}
#endif
#endif
