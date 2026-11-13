#ifndef GE_NATIVE_STDLIB_H
#define GE_NATIVE_STDLIB_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#include "include/stdlib.h"
void *malloc(size_t bytes);
void *calloc(size_t count, size_t bytes);
void *realloc(void *allocation, size_t bytes);
void free(void *allocation);
void abort(void);
void exit(int code);
int atexit(void (*callback)(void));
char *getenv(const char *name);
int atoi(const char *text);
long atol(const char *text);
double atof(const char *text);
long strtol(const char *text, char **end, int base);
unsigned long strtoul(const char *text, char **end, int base);
double strtod(const char *text, char **end);
int abs(int value);
long labs(long value);
void qsort(void *base, size_t count, size_t bytes, int (*compare)(const void *, const void *));
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 32767
#ifdef __cplusplus
}
#endif
#endif
