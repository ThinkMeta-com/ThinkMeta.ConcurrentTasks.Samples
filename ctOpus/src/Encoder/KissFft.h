// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    struct FftComplex {
        float r;
        float i;
    };

    /// <summary>
    /// kiss_fft_state of the static 48 kHz CELT mode: one of the four FFT sizes 480, 240, 120 and
    /// 60 that share the twiddles of the 480-point transform (shift selects every 2^shift-th one).
    /// </summary>
    struct FftState {
        std::int32_t        nfft;
        float               scale;
        std::int32_t        shift;
        std::int16_t        factors[16];
        const std::int16_t* bitrev;
        const FftComplex*   twiddles;
    };

    /// <summary>The FFT of size 480 >> shift (fft_state48000_960_shift), shift 0..3.</summary>
    const FftState& Fft48000(std::int32_t shift) noexcept;

    /// <summary>opus_fft_c: the scaled forward FFT of fin into fout (not in place).</summary>
    void Fft(const FftState& st, const FftComplex* fin, FftComplex* fout) noexcept;

    /// <summary>opus_fft_impl: the butterflies on bit-reversed data in place.</summary>
    void FftImpl(const FftState& st, FftComplex* fout) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
