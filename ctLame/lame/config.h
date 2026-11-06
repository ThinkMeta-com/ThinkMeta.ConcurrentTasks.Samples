/* Generated for the ThinkMeta.ConcurrentTasks builds: forwards to the
 * Windows-specific config header that ships with the LAME sources (MSVC) or
 * to configLinux.h (clang on Linux, CMakeLists.txt) instead of one generated
 * by autoconf/configure. */
#if defined(_WIN32)
#include "configMS.h"
#else
#include "configLinux.h"
#endif
