// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <array>
#include <cstdint>

#include "SimdFeatures.h"
#include "Types.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    // The fast Hartley transform of fft.c in one scalar and two SIMD versions. All three give
    // the same bits: each SIMD lane computes LAME's scalar butterfly for one twiddle index, and
    // the twiddles come from LAME's recursion (FhtSimd.inc).

    /// <summary>A Hartley transform of 2n values in place (fht in fft.c).</summary>
    using FhtFunction = void (*)(float* fz, std::int32_t n) noexcept;

    /// <summary>The number of stages of the largest transform (1024 = 4^5) minus one, as fft.c's TRI_SIZE.</summary>
    inline constexpr std::int32_t FhtTriSize = 5 - 1;

    /// <summary>cos and sin of the first twiddle of each stage (costab in fft.c).</summary>
    extern const std::array<float, FhtTriSize * 2> FhtCostab;

    void FhtScalar(float* fz, std::int32_t n) noexcept;
    void FhtSse41(float* fz, std::int32_t n) noexcept;
    void FhtAvx2(float* fz, std::int32_t n) noexcept;

    /// <summary>The transform of a SIMD level.</summary>
    FhtFunction FhtFor(SimdLevel level) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
