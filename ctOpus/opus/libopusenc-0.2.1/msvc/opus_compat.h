/* libopus 1.6 renamed the type-check macros used by libopusenc 0.2.1; pure type checks, no effect on encoding. */
#include <opus_defines.h>
#ifndef __opus_check_int
#define __opus_check_int(x) opus_check_int(x)
#define __opus_check_int_ptr(ptr) opus_check_int_ptr(ptr)
#endif
