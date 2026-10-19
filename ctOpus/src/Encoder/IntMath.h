// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

// Integer helpers of libopus 1.6.1 (IMIN/IMAX of celt/arch.h) and of the resamplers, shared by
// the ports: the operand order of the min/max macros is kept.

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    /// <summary>IMIN.</summary>
    constexpr std::int32_t Imin(std::int32_t a, std::int32_t b) noexcept
    {
        return a < b ? a : b;
    }

    /// <summary>IMAX.</summary>
    constexpr std::int32_t Imax(std::int32_t a, std::int32_t b) noexcept
    {
        return a > b ? a : b;
    }

    /// <summary>The greatest common divisor (Euclid), as compute_gcd of the Speex resampler.</summary>
    template <typename T>
    constexpr T Gcd(T a, T b) noexcept
    {
        while (b != 0) {
            const T t = a % b;
            a         = b;
            b         = t;
        }
        return a;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
