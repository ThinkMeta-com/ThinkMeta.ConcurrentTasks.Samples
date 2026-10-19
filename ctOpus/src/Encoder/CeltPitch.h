// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    // Port of the pitch analysis and comb filter of libopus 1.6.1 (celt/pitch.c, celt/celt_lpc.c,
    // comb_filter of celt/celt.c, celt/x86/pitch_avx.c), float build. arch is the libopus x86
    // level (0 C, 1 SSE, 2 SSE2, 3 SSE4.1, 4 AVX2): only celt_pitch_xcorr depends on it.

    constexpr std::int32_t ArchAvx2 = 4;

    /// <summary>opus_select_arch (celt/x86/x86cpu.c): the x86 level of this CPU from CPUID.</summary>
    std::int32_t SelectArch() noexcept;

    /// <summary>celt_pitch_xcorr: xcorr[i] = x·y[i..] for i &lt; maxPitch (AVX2/FMA or C with the SSE kernel).</summary>
    void PitchXcorr(const float* x, const float* y, float* xcorr, std::int32_t len, std::int32_t maxPitch, std::int32_t arch) noexcept;

    /// <summary>pitch_downsample: the half-rate, whitened sum of the channels.</summary>
    void PitchDownsample(float* const x[], float* xLp, std::int32_t len, std::int32_t c, std::int32_t factor, std::int32_t arch) noexcept;

    /// <summary>pitch_search: the best pitch lag of xLp in y.</summary>
    void PitchSearch(const float* xLp, const float* y, std::int32_t len, std::int32_t maxPitch, std::int32_t* pitch, std::int32_t arch) noexcept;

    /// <summary>remove_doubling: checks the sub-multiples of the period; returns the pitch gain.</summary>
    float RemoveDoubling(float* x, std::int32_t maxPeriod, std::int32_t minPeriod, std::int32_t n, std::int32_t* t0, std::int32_t prevPeriod,
                         float prevGain) noexcept;

    /// <summary>comb_filter: y = x filtered by the pitch comb, cross-faded over the overlap from (t0, g0, tapset0) to (t1, g1, tapset1).</summary>
    void CombFilter(float* y, float* x, std::int32_t t0, std::int32_t t1, std::int32_t n, float g0, float g1, std::int32_t tapset0, std::int32_t tapset1,
                    const float* window, std::int32_t overlap) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
