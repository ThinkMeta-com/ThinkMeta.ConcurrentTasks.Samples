#ifndef CONFIG_H
#define CONFIG_H

#define PACKAGE_NAME "opus"
#define PACKAGE_VERSION "1.6.1"

#define OPUS_BUILD 1
#define USE_ALLOCA 1
#define ENABLE_HARDENING 1
#define DISABLE_DEBUG_FLOAT 1

/* CPU Features: enable runtime CPU detection */
#define OPUS_HAVE_RTCD 1
#define CPU_INFO_BY_C 1
#define OPUS_X86_MAY_HAVE_SSE 1
#define OPUS_X86_MAY_HAVE_SSE2 1
#define OPUS_X86_MAY_HAVE_SSE4_1 1
#define OPUS_X86_MAY_HAVE_AVX2 1

#define OPUS_X86_PRESUME_SSE 1
#define OPUS_X86_PRESUME_SSE2 1

#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_LRINT 1
#define HAVE_LRINTF 1

#ifdef _MSC_VER
#include <malloc.h>
#define alloca _alloca
#define inline __inline
#pragma warning(disable: 4244 4305 4018 4100 4127 4702 4701 4706 4324 4310)
#endif

#endif /* CONFIG_H */
