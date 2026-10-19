// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

#include "RangeEncoder.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    // Port of the energy quantisation of libopus 1.6.1 (celt/quant_bands.c, celt/laplace.c) for
    // the static 48 kHz mode (encoder side).

    constexpr std::int32_t MaxFineBits = 8;

    /// <summary>eMeans: the mean energy per band in Q4, as float.</summary>
    extern const float EMeans[25];

    /// <summary>amp2Log2: band amplitudes to log2 energies relative to eMeans.</summary>
    void Amp2Log2(std::int32_t effEnd, std::int32_t end, const float* bandE, float* bandLogE, std::int32_t c) noexcept;

    /// <summary>quant_coarse_energy: intra or inter-predicted coarse energies (two-pass at complexity 4 and up).</summary>
    void QuantCoarseEnergy(std::int32_t start, std::int32_t end, std::int32_t effEnd, const float* eBands, float* oldEBands, std::uint32_t budget, float* error,
                           RangeEncoder* enc, std::int32_t c, std::int32_t lm, std::int32_t nbAvailableBytes, std::int32_t forceIntra, float* delayedIntra,
                           bool twoPass, std::int32_t lossRate, std::int32_t lfe) noexcept;

    /// <summary>quant_fine_energy with prev_quant NULL: extraQuant[i] more bits per band.</summary>
    void QuantFineEnergy(std::int32_t start, std::int32_t end, float* oldEBands, float* error, const std::int32_t* extraQuant, RangeEncoder* enc,
                         std::int32_t c) noexcept;

    /// <summary>quant_energy_finalise: the remaining bits, one per band and channel by priority.</summary>
    void QuantEnergyFinalise(std::int32_t start, std::int32_t end, float* oldEBands, float* error, const std::int32_t* fineQuant,
                             const std::int32_t* finePriority, std::int32_t bitsLeft, RangeEncoder* enc, std::int32_t c) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
