// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "FftKernels.h"

#include <array>
#include <cstdint>

#include <immintrin.h>

// The Hartley transform with 4 lanes (SSE, compiled for the default architecture).

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

#define FHT_SIMD_NAME FhtSse41
#define FHT_SIMD_WIDE Sse
#include "FhtSimd.inc"
#undef FHT_SIMD_WIDE
#undef FHT_SIMD_NAME

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
