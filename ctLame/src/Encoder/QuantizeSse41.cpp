// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "QuantizeKernels.h"

#include <array>
#include <cstdint>

#include <immintrin.h>

// The quantizer's loops with 4 lanes (SSE, compiled for the default architecture).

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

#define QUANTIZE_SIMD_SUFFIX Sse41
#include "QuantizeSimd.inc"
#undef QUANTIZE_SIMD_SUFFIX

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
