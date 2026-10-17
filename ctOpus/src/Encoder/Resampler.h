// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

#include "HeapArray.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    /// <summary>
    /// Port of the Speex resampler in libopusenc 0.2.1 (resample.c) as libopusenc uses it:
    /// float samples, quality 5, RESAMPLE_FULL_SINC_TABLE (always the direct sinc table) and the
    /// SSE inner product (resample_sse.h) that GCC/Clang x64 builds of opusenc use. The stream
    /// semantics are those of speex_resampler_process_interleaved_float, including how much input
    /// a call consumes when the output space runs out.
    /// </summary>
    class Resampler
    {
    public:
        static constexpr std::int32_t Quality = 5;

        /// <summary>speex_resampler_init + speex_resampler_skip_zeros; false on invalid rates or out of memory.</summary>
        bool Initialize(std::int32_t channels, std::uint32_t inRate, std::uint32_t outRate) noexcept;

        /// <summary>
        /// speex_resampler_process_interleaved_float: resamples up to inLen frames into at most
        /// outLen frames; on return both hold the frames actually consumed and produced.
        /// </summary>
        void ProcessInterleaved(const float* in, std::uint32_t& inLen, float* out, std::uint32_t& outLen) noexcept;

        /// <summary>speex_resampler_get_output_latency.</summary>
        std::int32_t OutputLatency() const noexcept;

        std::int32_t  FilterLength() const noexcept { return static_cast<std::int32_t>(filtLen_); }
        std::uint32_t NumRate() const noexcept { return numRate_; }
        std::uint32_t DenRate() const noexcept { return denRate_; }
        const float*  SincTable() const noexcept { return sincTable_.Data(); }

    private:
        bool          BuildFilter() noexcept;
        void          ProcessChannel(std::int32_t channel, const float* in, std::uint32_t& inLen, float* out, std::uint32_t& outLen) noexcept;
        std::uint32_t Kernel(std::int32_t channel, const float* mem, std::uint32_t inLen, float* out, std::uint32_t outLen) noexcept;

        std::int32_t  channels_     = 0;
        std::uint32_t numRate_      = 0;
        std::uint32_t denRate_      = 0;
        std::uint32_t filtLen_      = 0;
        std::uint32_t memAllocSize_ = 0;
        std::int32_t  intAdvance_   = 0;
        std::int32_t  fracAdvance_  = 0;
        float         cutoff_       = 1.f;
        std::int32_t  stride_       = 1; // in_stride = out_stride = channels while processing interleaved

        HeapArray<float>         sincTable_;
        HeapArray<float>         mem_;
        HeapArray<std::int32_t>  lastSample_;
        HeapArray<std::uint32_t> sampFracNum_;
    };

    /// <summary>
    /// The Speex resampler for live audio: quality 5 with the interpolated sinc table
    /// (resampler_basic_interpolate_single), so the ratio can change while running without a
    /// new table (speex_resampler_set_rate_frac). Filter length and cutoff stay those of the
    /// nominal rates; small ratio changes (clock drift, well below 1 %) do not need others. Not
    /// bit-identical to Resampler, which keeps the direct table of opusenc.
    /// </summary>
    class LiveResampler
    {
    public:
        /// <summary>The ratio's denominator once SetRatio was called (2^24: samp_frac_num * 32 fits 32 bits).</summary>
        static constexpr std::uint32_t RatioDen = 1u << 24;

        /// <summary>speex_resampler_init + skip_zeros for the nominal rates; false on invalid rates or out of memory.</summary>
        bool Initialize(std::int32_t channels, std::uint32_t inRate, std::uint32_t outRate) noexcept;

        /// <summary>
        /// Input frames per output frame: NominalRatio() for the nominal rates, a little more to
        /// consume the input faster (produce less output). Takes effect with the next output frame.
        /// </summary>
        void SetRatio(double ratio) noexcept;

        double NominalRatio() const noexcept { return nominalRatio_; }
        double Ratio() const noexcept { return static_cast<double>(numRate_) / denRate_; }

        /// <summary>As Resampler::ProcessInterleaved.</summary>
        void ProcessInterleaved(const float* in, std::uint32_t& inLen, float* out, std::uint32_t& outLen) noexcept;

        /// <summary>The delay in output frames (speex_resampler_get_output_latency).</summary>
        std::int32_t OutputLatency() const noexcept;

    private:
        void          ProcessChannel(std::int32_t channel, const float* in, std::uint32_t& inLen, float* out, std::uint32_t& outLen) noexcept;
        std::uint32_t Kernel(std::int32_t channel, const float* mem, std::uint32_t inLen, float* out, std::uint32_t outLen) noexcept;

        std::int32_t  channels_     = 0;
        std::uint32_t numRate_      = 0;
        std::uint32_t denRate_      = 0;
        std::uint32_t filtLen_      = 0;
        std::uint32_t oversample_   = 0;
        std::uint32_t memAllocSize_ = 0;
        std::int32_t  intAdvance_   = 0;
        std::int32_t  fracAdvance_  = 0;
        std::int32_t  stride_       = 1;
        double        nominalRatio_ = 1;

        HeapArray<float>         sincTable_; // oversample * filtLen + 8 taps
        HeapArray<float>         mem_;
        HeapArray<std::int32_t>  lastSample_;
        HeapArray<std::uint32_t> sampFracNum_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
