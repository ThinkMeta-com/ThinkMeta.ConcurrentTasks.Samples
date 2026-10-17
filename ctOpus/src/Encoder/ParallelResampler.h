// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

#include "HeapArray.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    class Resampler;

    /// <summary>
    /// The Speex resampler of libopusenc as a function of the output index: output j of a channel
    /// is the SSE-ordered inner product of sinc phase (j * step) mod den with the input window
    /// starting at TapStart(j) of the padded input (filter length - 1 zeros, the samples, then the
    /// drain extension). Any range of outputs can therefore be computed on its own and in any order,
    /// with exactly the floats of the streaming resampler. With AVX2 eight consecutive outputs share
    /// one vector, each lane summing its taps in the order of the SSE kernel.
    /// </summary>
    class ParallelResampler
    {
    public:
        /// <summary>Takes the filter of an initialized Resampler; false if out of memory.</summary>
        bool Initialize(const Resampler& resampler) noexcept;

        std::int32_t FilterLength() const noexcept { return n_; }

        /// <summary>Index into the padded input of the first tap of output j.</summary>
        std::int64_t TapStart(std::int64_t j) const noexcept { return n_ / 2 + (j * step_) / den_; }

        /// <summary>Number of outputs whose taps start before inputFrames padded input frames end the samples.</summary>
        std::int64_t OutputsFor(std::int64_t inputFrames) const noexcept;

        /// <summary>Highest padded input index (exclusive) the outputs [0, j) read.</summary>
        std::int64_t InputNeeded(std::int64_t j) const noexcept { return j == 0 ? 0 : TapStart(j - 1) + n_; }

        /// <summary>
        /// Outputs [j0, j1) of every channel into interleaved out (out[0] = output j0 of channel 0).
        /// padded[c] points at index 0 of channel c's padded input.
        /// </summary>
        void Compute(const float* const* padded, std::int32_t channels, std::int64_t j0, std::int64_t j1, float* out) const noexcept;

        bool UsesAvx2() const noexcept { return avx2_; }

    private:
        void ComputeScalar(const float* padded, std::int64_t j0, std::int64_t j1, float* out, std::int32_t stride) const noexcept;
        void ComputeAvx2(const float* padded, std::int64_t j0, std::int64_t j1, float* out, std::int32_t stride) const noexcept;

        const float*            table_  = nullptr; // the Resampler's sinc table, [phase][tap]
        std::int32_t            n_      = 0;
        std::int64_t            den_    = 0;
        std::int64_t            step_   = 0; // int_advance * den + frac_advance = num
        std::int64_t            period_ = 0; // lcm(den, 8): the pattern of 8-output groups repeats
        bool                    avx2_   = false;
        HeapArray<float>        groupCoef_; // [group][tap][lane]
        HeapArray<std::int32_t> groupPerm_; // [group][lane]: offset of the lane's window
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
