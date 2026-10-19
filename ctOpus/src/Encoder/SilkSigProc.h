// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

#include "SilkState.h"

// Port of the small fixed-point signal processing functions of SILK (libopus 1.6.1 silk/*.c)
// that several parts of the encoder share.

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk {

    /// <summary>silk_lin2log: approximately 128 * log2(inLin).</summary>
    std::int32_t Lin2Log(std::int32_t inLin) noexcept;

    /// <summary>silk_log2lin: approximately 2^(inLogQ7 / 128).</summary>
    std::int32_t Log2Lin(std::int32_t inLogQ7) noexcept;

    /// <summary>silk_sum_sqr_shift: the energy of x and the right shift that makes it fit in 31 bits with headroom.</summary>
    void SumSqrShift(std::int32_t* energy, std::int32_t* shift, const std::int16_t* x, std::int32_t len) noexcept;

    /// <summary>silk_inner_prod_aligned_scale.</summary>
    std::int32_t InnerProdAlignedScale(const std::int16_t* inVec1, const std::int16_t* inVec2, std::int32_t scale, std::int32_t len) noexcept;

    /// <summary>silk_sigm_Q15: an approximate sigmoid of inQ5, in Q15.</summary>
    std::int32_t SigmQ15(std::int32_t inQ5) noexcept;

    /// <summary>silk_ana_filt_bank_1: splits n samples into two decimated bands with first-order allpass filters.</summary>
    void AnaFiltBank1(const std::int16_t* in, std::int32_t* s, std::int16_t* outL, std::int16_t* outH, std::int32_t n) noexcept;

    /// <summary>silk_biquad_alt_stride1: a second-order ARMA filter (in and out may be the same).</summary>
    void BiquadAltStride1(const std::int16_t* in, const std::int32_t* bQ28, const std::int32_t* aQ28, std::int32_t* s, std::int16_t* out,
                          std::int32_t len) noexcept;

    /// <summary>silk_LP_variable_cutoff: the low-pass filter of the bandwidth transitions, in place.</summary>
    void LpVariableCutoff(SilkLpState* lp, std::int16_t* frame, std::int32_t frameLength) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk
