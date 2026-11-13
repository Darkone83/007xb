#ifndef GE_NATIVE_MATH_H
#define GE_NATIVE_MATH_H
/* Original constants plus classic CRT declarations; never include_next. */
#ifdef __cplusplus
extern "C" {
#endif
#include "include/math.h"
double sqrt(double x);
double floor(double x);
double ceil(double x);
double fabs(double x);
double fmod(double x, double y);
double acos(double x);
double asin(double x);
double atan(double x);
double atan2(double y, double x);
double tan(double x);
double pow(double x, double y);
double exp(double x);
double log(double x);
double log10(double x);
float tanf(float x);
float floorf(float x);
float ceilf(float x);
float fabsf(float x);
float fmodf(float x, float y);
long lroundf(float x);
float acosf(float x);
float asinf(float x);
float atan2f(float y, float x);
#ifdef __cplusplus
}
#endif
#endif
