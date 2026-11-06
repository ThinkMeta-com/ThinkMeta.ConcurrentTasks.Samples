/* Linux x64 counterpart of configMS.h for the ThinkMeta.ConcurrentTasks
 * builds (clang, CMakeLists.txt): the same feature switches as the MSVC
 * build, so that both builds compile the same code paths. Only the type
 * definitions come from the C library instead of MSVC's __intN types. */
#ifndef CONFIGLINUX_H_INCLUDED
#define CONFIGLINUX_H_INCLUDED

#define STDC_HEADERS
#define HAVE_ERRNO_H
#define HAVE_FCNTL_H
#define HAVE_LIMITS_H
#define HAVE_STDINT_H
#define PACKAGE "lame"
#define PROTOTYPES 1
#define USE_FAST_LOG 1
#define HAVE_STRCHR
#define HAVE_MEMCPY

#include <stdint.h>
typedef float  float32_t;
typedef double float64_t;

typedef long double ieee854_float80_t;
typedef double      ieee754_float64_t;
typedef float       ieee754_float32_t;

#define LAME_LIBRARY_BUILD

#if !defined(__x86_64__)
#    error This LAME build supports x64 only
#endif

#endif /* CONFIGLINUX_H_INCLUDED */
