// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <xmmintrin.h>

#include <cstdint>
#include <cstring>

// Float helpers of libopus 1.6.1 (celt/mathops.h, celt/float_cast.h, celt/arch.h) with the exact
// semantics of the float build: the operand order of the min/max macros and the rounding of the
// conversions matter for bit-identical output.

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    constexpr float Min32(float a, float b) noexcept
    {
        return a < b ? a : b;
    } // MIN16/MIN32
    constexpr float Max32(float a, float b) noexcept
    {
        return a > b ? a : b;
    } // MAX16/MAX32

    /// <summary>float2int of MSVC x64: cvtss2si, rounding to nearest even.</summary>
    inline std::int32_t Float2Int(float value) noexcept
    {
        return _mm_cvtss_si32(_mm_load_ss(&value));
    }

    /// <summary>celt_isnan of the float build: the exponent all ones and a non-zero mantissa.</summary>
    inline bool IsNan(float x) noexcept
    {
        std::uint32_t i = 0;
        std::memcpy(&i, &x, sizeof(i));
        return ((i >> 23) & 0xFF) == 0xFF && (i & 0x007FFFFF) != 0;
    }

    /// <summary>celt_maxabs16.</summary>
    inline float MaxAbs(const float* x, std::int32_t len) noexcept
    {
        float maxval = 0;
        float minval = 0;
        for (std::int32_t i = 0; i < len; ++i) {
            maxval = maxval > x[i] ? maxval : x[i];
            minval = minval < x[i] ? minval : x[i];
        }
        return maxval > -minval ? maxval : -minval;
    }

    /// <summary>is_digital_silence of opus_encoder.c.</summary>
    inline bool IsDigitalSilence(const float* pcm, std::int32_t frameSize, std::int32_t channels, std::int32_t lsbDepth) noexcept
    {
        const float sampleMax = MaxAbs(pcm, frameSize * channels);
        return sampleMax <= static_cast<float>(1) / (1 << lsbDepth);
    }

    /// <summary>fast_atan2f (mathops.h): a rational approximation, 0 for tiny inputs.</summary>
    inline float FastAtan2f(float y, float x) noexcept
    {
        constexpr float a  = 0.43157974f;
        constexpr float b  = 0.67848403f;
        constexpr float c  = 0.08595542f;
        constexpr float e  = static_cast<float>(3.1415926535897931) / 2;
        const float     x2 = x * x;
        const float     y2 = y * y;
        if (x2 + y2 < 1e-18f)
            return 0;
        if (x2 < y2) {
            const float den = (y2 + b * x2) * (y2 + c * x2);
            return -x * y * (y2 + a * x2) / den + (y < 0 ? -e : e);
        }
        const float den = (x2 + b * y2) * (x2 + c * y2);
        return x * y * (x2 + a * y2) / den + (y < 0 ? -e : e) - (x * y < 0 ? -e : e);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
