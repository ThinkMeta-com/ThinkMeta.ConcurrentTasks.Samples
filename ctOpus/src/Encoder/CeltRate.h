// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

#include "CeltMode.h"
#include "RangeEncoder.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    // Port of the bit allocation of libopus 1.6.1 (celt/rate.c, celt/rate.h, init_caps of
    // celt/celt.c) for the static 48 kHz mode, encoder side.

    constexpr std::int32_t LogMaxPseudo         = 6;
    constexpr std::int32_t QthetaOffset         = 4;
    constexpr std::int32_t QthetaOffsetTwophase = 16;

    /// <summary>get_pulses: the pulse count of a pseudo-pulse index.</summary>
    constexpr std::int32_t GetPulses(std::int32_t i) noexcept
    {
        return i < 8 ? i : (8 + (i & 7)) << ((i >> 3) - 1);
    }

    /// <summary>bits2pulses: the pseudo-pulse count closest to bits (1/8 bits) for a band.</summary>
    inline std::int32_t Bits2Pulses(std::int32_t band, std::int32_t lm, std::int32_t bits) noexcept
    {
        ++lm;
        const std::uint8_t* cache = CeltMode::CacheBits + CeltMode::CacheIndex[lm * CeltMode::NbEBands + band];
        std::int32_t        lo    = 0;
        std::int32_t        hi    = cache[0];
        --bits;
        for (std::int32_t i = 0; i < LogMaxPseudo; ++i) {
            const std::int32_t mid = (lo + hi + 1) >> 1;
            if (static_cast<std::int32_t>(cache[mid]) >= bits)
                hi = mid;
            else
                lo = mid;
        }
        if (bits - (lo == 0 ? -1 : static_cast<std::int32_t>(cache[lo])) <= static_cast<std::int32_t>(cache[hi]) - bits)
            return lo;
        return hi;
    }

    /// <summary>pulses2bits: the bits (1/8 bits) of a pseudo-pulse count.</summary>
    inline std::int32_t Pulses2Bits(std::int32_t band, std::int32_t lm, std::int32_t pulses) noexcept
    {
        ++lm;
        const std::uint8_t* cache = CeltMode::CacheBits + CeltMode::CacheIndex[lm * CeltMode::NbEBands + band];
        return pulses == 0 ? 0 : cache[pulses] + 1;
    }

    /// <summary>init_caps: the maximum bits per band.</summary>
    void InitCaps(std::int32_t* cap, std::int32_t lm, std::int32_t c) noexcept;

    /// <summary>clt_compute_allocation (encoding): pulses, fine energy bits and priorities per band; returns the coded bands.</summary>
    std::int32_t ComputeAllocation(std::int32_t start, std::int32_t end, const std::int32_t* offsets, const std::int32_t* cap, std::int32_t allocTrim,
                                   std::int32_t* intensity, std::int32_t* dualStereo, std::int32_t total, std::int32_t* balance, std::int32_t* pulses,
                                   std::int32_t* ebits, std::int32_t* finePriority, std::int32_t c, std::int32_t lm, RangeEncoder* ec, std::int32_t prev,
                                   std::int32_t signalBandwidth) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
