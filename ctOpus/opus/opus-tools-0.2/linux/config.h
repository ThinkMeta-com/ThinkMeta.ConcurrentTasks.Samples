/* Linux x64 counterpart of msvc/config.h for the ThinkMeta.ConcurrentTasks
 * builds (clang, CMakeLists.txt): the same switches, without the MSVC parts.
 * __SSE__ (the SSE path of the Speex resampler) is predefined by clang on x64. */
#ifndef CONFIG_H
#define CONFIG_H

#define RESAMPLE_FULL_SINC_TABLE
#define OUTSIDE_SPEEX         1
#define OPUSTOOLS             1

#define RANDOM_PREFIX opustools
#define PACKAGE_NAME "opus-tools"
#include "version.h"

#endif /* CONFIG_H */
