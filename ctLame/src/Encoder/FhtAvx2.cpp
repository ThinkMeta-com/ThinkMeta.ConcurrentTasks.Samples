// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "FftKernels.h"

#include <array>
#include <cstdint>

#include <immintrin.h>

// The Hartley transform with 8 lanes and a 4-lane tail. This file alone is compiled with
// /arch:AVX2 (the project file); it is called only when the CPU has AVX2. /fp:precise without
// /fp:contract keeps the compiler from fusing multiplies and adds, so no FMA is emitted.

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

#define FHT_SIMD_NAME FhtAvx2
#define FHT_SIMD_WIDE Avx2
#define FHT_SIMD_AVX2
#include "FhtSimd.inc"
#undef FHT_SIMD_AVX2
#undef FHT_SIMD_WIDE
#undef FHT_SIMD_NAME

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
