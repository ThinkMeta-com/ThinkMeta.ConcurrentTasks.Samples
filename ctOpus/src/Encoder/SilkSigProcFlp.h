// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

// Port of the float signal processing functions of SILK (libopus 1.6.1 silk/float/*_FLP.c,
// float/x86/inner_product_FLP_avx2.c).

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk {

    /// <summary>silk_inner_product_FLP: in double; the AVX2/FMA variant for arch >= ArchAvx2, as the libopus RTCD.</summary>
    double InnerProductFlp(const float* data1, const float* data2, std::int32_t dataSize, std::int32_t arch) noexcept;

    /// <summary>silk_energy_FLP: the sum of squares, in double.</summary>
    double EnergyFlp(const float* data, std::int32_t dataSize) noexcept;

    /// <summary>silk_autocorrelation_FLP.</summary>
    void AutocorrelationFlp(float* results, const float* inputData, std::int32_t inputDataSize, std::int32_t correlationCount, std::int32_t arch) noexcept;

    /// <summary>silk_warped_autocorrelation_FLP: autocorrelations on a warped frequency axis (order even).</summary>
    void WarpedAutocorrelationFlp(float* corr, const float* input, float warping, std::int32_t length, std::int32_t order) noexcept;

    /// <summary>silk_schur_FLP: the reflection coefficients; returns the residual energy.</summary>
    float SchurFlp(float reflCoef[], const float autoCorr[], std::int32_t order) noexcept;

    /// <summary>silk_k2a_FLP: reflection coefficients to prediction coefficients.</summary>
    void K2aFlp(float* a, const float* rc, std::int32_t order) noexcept;

    /// <summary>silk_bwexpander_FLP: chirps an AR filter (without the leading 1).</summary>
    void BwexpanderFlp(float* ar, std::int32_t d, float chirp) noexcept;

    /// <summary>silk_apply_sine_window_FLP: winType 1 is a sine window from 0 to pi/2, 2 from pi/2 to pi (length a multiple of 4).</summary>
    void ApplySineWindowFlp(float pxWin[], const float px[], std::int32_t winType, std::int32_t length) noexcept;

    /// <summary>silk_insertion_sort_decreasing_FLP: the k largest of the l values of a sorted first, with their indices.</summary>
    void InsertionSortDecreasingFlp(float* a, std::int32_t* idx, std::int32_t l, std::int32_t k) noexcept;

    /// <summary>silk_LPC_analysis_filter_FLP: the LPC residual with zero start state (order 6, 8, 10, 12 or 16; the first order samples are zero).</summary>
    void LpcAnalysisFilterFlp(float rLpc[], const float predCoef[], const float s[], std::int32_t length, std::int32_t order) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk
