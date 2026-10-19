// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <cstdint>

#include "Types.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    struct SessionConfig;

    // Port of the helpers of util.c the psychoacoustic model and the quantizer use.

    // LAME's constants as its build defines them (no _USE_MATH_DEFINES, so no M_LN2 etc.)
    inline constexpr double LOG2  = 0.69314718055994530942;
    inline constexpr double LOG10 = 2.30258509299404568402;

    /// <summary>The size of fast_log2's table: log2 of 1 + j / 2^FastLog2SizeL2, j = 0..2^FastLog2SizeL2.</summary>
    inline constexpr std::int32_t FastLog2SizeL2 = 9;

    /// <summary>fast_log2: log2 by table lookup with linear interpolation (USE_FAST_LOG).</summary>
    float FastLog2(float x) noexcept;

    /// <summary>The table of FastLog2 (2^FastLog2SizeL2 + 1 values), for its SIMD versions.</summary>
    const float* FastLog2Table() noexcept;

    // FAST_LOG10 and friends are double expressions in LAME: fast_log2(x) * (a double constant).
    inline double FastLog10(float x) noexcept
    {
        return FastLog2(x) * (LOG2 / LOG10);
    }
    inline double FastLog(float x) noexcept
    {
        return FastLog2(x) * LOG2;
    }
    inline double FastLog10X(float x, double y) noexcept
    {
        return FastLog2(x) * (LOG2 / LOG10 * y);
    }

    /// <summary>freq2bark: frequency in Hz to bark.</summary>
    float FreqToBark(float freq) noexcept;

    /// <summary>ATHformula: absolute threshold of hearing in dB at a frequency in Hz.</summary>
    float AthFormula(const SessionConfig& config, float freq) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
