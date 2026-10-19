// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "Resampler.h"

#include "IntMath.h"

#include <immintrin.h>

#include <climits>
#include <cmath>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    namespace {

        // Kaiser window for quality 5 (KAISER10, oversampled 32 times): data from resample.c.
        constexpr double Kaiser10Table[36]
            = {0.99537781, 1.00000000, 0.99537781, 0.98162644, 0.95908712, 0.92831446, 0.89005583, 0.84522401, 0.79486424, 0.74011713, 0.68217934, 0.62226347,
               0.56155915, 0.50119680, 0.44221549, 0.38553619, 0.33194107, 0.28205962, 0.23636152, 0.19515633, 0.15859932, 0.12670280, 0.09935205, 0.07632451,
               0.05731132, 0.04193980, 0.02979584, 0.02044510, 0.01345224, 0.00839739, 0.00488951, 0.00257636, 0.00115101, 0.00035515, 0.00000000, 0.00000000};
        constexpr std::int32_t Kaiser10Oversample = 32;

        // quality_map[5]
        constexpr std::uint32_t BaseLength          = 80;
        constexpr float         DownsampleBandwidth = 0.922f;
        constexpr float         UpsampleBandwidth   = 0.940f;
        constexpr std::uint32_t BufferSize          = 160; // st->buffer_size

        constexpr double Pi = 3.14159265358979323846;

        // libopusenc: compute_func() (src/resample.c): cubic interpolation of the window table at x in [0, 1].
        double WindowAt(float x) noexcept
        {
            const float        y    = x * Kaiser10Oversample;
            const std::int32_t ind  = static_cast<std::int32_t>(std::floor(y));
            const float        frac = y - ind;
            double             interp[4];
            interp[3] = -0.1666666667 * frac + 0.1666666667 * (frac * frac * frac);
            interp[2] = frac + 0.5 * (frac * frac) - 0.5 * (frac * frac * frac);
            interp[0] = -0.3333333333 * frac + 0.5 * (frac * frac) - 0.1666666667 * (frac * frac * frac);
            interp[1] = 1.f - interp[3] - interp[2] - interp[0];
            return interp[0] * Kaiser10Table[ind] + interp[1] * Kaiser10Table[ind + 1] + interp[2] * Kaiser10Table[ind + 2]
                + interp[3] * Kaiser10Table[ind + 3];
        }

        // libopusenc: sinc() (src/resample.c) (floating point build): the windowed sinc tap at offset x of an n-tap filter.
        float Sinc(float cutoff, float x, std::int32_t n) noexcept
        {
            const float xx = x * cutoff;
            if (std::fabs(x) < 1e-6)
                return cutoff;
            if (std::fabs(x) > .5 * n)
                return 0;
            return static_cast<float>(cutoff * std::sin(Pi * xx) / (Pi * xx) * WindowAt(static_cast<float>(std::fabs(2. * x / n))));
        }

        // libopusenc: multiply_frac() (src/resample.c): value * num / den without 32-bit overflow; false on overflow.
        bool MultiplyFrac(std::uint32_t& result, std::uint32_t value, std::uint32_t num, std::uint32_t den) noexcept
        {
            const std::uint32_t major  = value / den;
            const std::uint32_t remain = value % den;
            if (remain > UINT32_MAX / num || major > UINT32_MAX / num || major * num > UINT32_MAX - remain * num / den)
                return false;
            result = remain * num / den + major * num;
            return true;
        }

        // libopusenc: inner_product_single() (src/resample_sse.h): four lanes over the taps, then (s0 + s2) + (s1 + s3).
        float InnerProduct(const float* a, const float* b, std::uint32_t len) noexcept
        {
            __m128 sum = _mm_setzero_ps();
            for (std::uint32_t i = 0; i < len; i += 8) {
                sum = _mm_add_ps(sum, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
                sum = _mm_add_ps(sum, _mm_mul_ps(_mm_loadu_ps(a + i + 4), _mm_loadu_ps(b + i + 4)));
            }
            sum = _mm_add_ps(sum, _mm_movehl_ps(sum, sum));
            sum = _mm_add_ss(sum, _mm_shuffle_ps(sum, sum, 0x55));
            return _mm_cvtss_f32(sum);
        }

    } // namespace

    // libopusenc: speex_resampler_init() (src/resample.c) with speex_resampler_skip_zeros()
    bool Resampler::Initialize(std::int32_t channels, std::uint32_t inRate, std::uint32_t outRate) noexcept
    {
        if (channels <= 0 || inRate == 0 || outRate == 0)
            return false;
        channels_                = channels;
        const std::uint32_t fact = Gcd(inRate, outRate);
        numRate_                 = inRate / fact;
        denRate_                 = outRate / fact;
        if (!BuildFilter())
            return false;
        memAllocSize_ = filtLen_ - 1 + BufferSize;
        if (!mem_.Allocate(static_cast<std::size_t>(channels) * memAllocSize_) || !lastSample_.Allocate(static_cast<std::size_t>(channels))
            || !sampFracNum_.Allocate(static_cast<std::size_t>(channels)))
            return false;
        for (std::int32_t c = 0; c < channels; ++c)
            lastSample_[c] = static_cast<std::int32_t>(filtLen_ / 2); // speex_resampler_skip_zeros
        return true;
    }

    // libopusenc: update_filter() (src/resample.c) for a fresh resampler.
    bool Resampler::BuildFilter() noexcept
    {
        intAdvance_  = static_cast<std::int32_t>(numRate_ / denRate_);
        fracAdvance_ = static_cast<std::int32_t>(numRate_ % denRate_);
        filtLen_     = BaseLength;
        if (numRate_ > denRate_) {
            // down-sampling
            cutoff_ = DownsampleBandwidth * denRate_ / numRate_;
            if (!MultiplyFrac(filtLen_, filtLen_, numRate_, denRate_))
                return false;
            filtLen_ = ((filtLen_ - 1) & ~0x7u) + 8; // a multiple of 8 for SSE
        } else {
            cutoff_ = UpsampleBandwidth;
        }
        if (INT_MAX / sizeof(float) / denRate_ < filtLen_)
            return false;
        if (!sincTable_.Allocate(static_cast<std::size_t>(filtLen_) * denRate_))
            return false;
        const std::int32_t n = static_cast<std::int32_t>(filtLen_);
        for (std::uint32_t i = 0; i < denRate_; ++i) {
            for (std::int32_t j = 0; j < n; ++j) {
                const float x                                          = (j - n / 2 + 1) - static_cast<float>(i) / denRate_;
                sincTable_[static_cast<std::size_t>(i) * filtLen_ + j] = Sinc(cutoff_, x, n);
            }
        }
        return true;
    }

    // libopusenc: speex_resampler_get_output_latency() (src/resample.c)
    std::int32_t Resampler::OutputLatency() const noexcept
    {
        return static_cast<std::int32_t>(((filtLen_ / 2) * denRate_ + (numRate_ >> 1)) / numRate_);
    }

    // libopusenc: speex_resampler_process_interleaved_float() (src/resample.c)
    void Resampler::ProcessInterleaved(const float* in, std::uint32_t& inLen, float* out, std::uint32_t& outLen) noexcept
    {
        const std::uint32_t inBackup  = inLen;
        const std::uint32_t outBackup = outLen;
        stride_                       = channels_;
        for (std::int32_t c = 0; c < channels_; ++c) {
            inLen  = inBackup;
            outLen = outBackup;
            ProcessChannel(c, in + c, inLen, out + c, outLen);
        }
        stride_ = 1;
    }

    // libopusenc: speex_resampler_process_float() (src/resample.c) with speex_resampler_process_native (no magic samples).
    void Resampler::ProcessChannel(std::int32_t channel, const float* in, std::uint32_t& inLen, float* out, std::uint32_t& outLen) noexcept
    {
        std::uint32_t       ilen     = inLen;
        std::uint32_t       olen     = outLen;
        float*              x        = mem_.Data() + static_cast<std::size_t>(channel) * memAllocSize_;
        const std::uint32_t filtOffs = filtLen_ - 1;
        const std::uint32_t xlen     = memAllocSize_ - filtOffs;
        while (ilen != 0 && olen != 0) {
            std::uint32_t ichunk = ilen > xlen ? xlen : ilen;
            for (std::uint32_t j = 0; j < ichunk; ++j)
                x[j + filtOffs] = in[static_cast<std::size_t>(j) * stride_];

            const std::uint32_t produced = Kernel(channel, x, ichunk, out, olen);
            if (lastSample_[channel] < static_cast<std::int32_t>(ichunk))
                ichunk = static_cast<std::uint32_t>(lastSample_[channel]);
            lastSample_[channel] -= static_cast<std::int32_t>(ichunk);
            for (std::uint32_t j = 0; j < filtOffs; ++j)
                x[j] = x[j + ichunk];

            ilen -= ichunk;
            olen -= produced;
            out += static_cast<std::size_t>(produced) * stride_;
            in += static_cast<std::size_t>(ichunk) * stride_;
        }
        inLen -= ilen;
        outLen -= olen;
    }

    // libopusenc: resampler_basic_direct_single() (src/resample.c) with the SSE inner product.
    std::uint32_t Resampler::Kernel(std::int32_t channel, const float* mem, std::uint32_t inLen, float* out, std::uint32_t outLen) noexcept
    {
        std::int32_t  lastSample  = lastSample_[channel];
        std::uint32_t sampFracNum = sampFracNum_[channel];
        std::uint32_t produced    = 0;
        while (!(lastSample >= static_cast<std::int32_t>(inLen) || produced >= outLen)) {
            const float* sinct                                  = sincTable_.Data() + static_cast<std::size_t>(sampFracNum) * filtLen_;
            const float* iptr                                   = mem + lastSample;
            out[static_cast<std::size_t>(stride_) * produced++] = InnerProduct(sinct, iptr, filtLen_);
            lastSample += intAdvance_;
            sampFracNum += static_cast<std::uint32_t>(fracAdvance_);
            if (sampFracNum >= denRate_) {
                sampFracNum -= denRate_;
                ++lastSample;
            }
        }
        lastSample_[channel]  = lastSample;
        sampFracNum_[channel] = sampFracNum;
        return produced;
    }

    // --- LiveResampler ---------------------------------------------------------------------------

    // libopusenc: speex_resampler_init() (src/resample.c) + skip_zeros, update_filter with the interpolated table.
    bool LiveResampler::Initialize(std::int32_t channels, std::uint32_t inRate, std::uint32_t outRate) noexcept
    {
        if (channels <= 0 || inRate == 0 || outRate == 0)
            return false;
        channels_                = channels;
        const std::uint32_t fact = Gcd(inRate, outRate);
        numRate_                 = inRate / fact;
        denRate_                 = outRate / fact;
        nominalRatio_            = static_cast<double>(inRate) / outRate;
        intAdvance_              = static_cast<std::int32_t>(numRate_ / denRate_);
        fracAdvance_             = static_cast<std::int32_t>(numRate_ % denRate_);
        oversample_              = Kaiser10Oversample;
        filtLen_                 = BaseLength;
        float cutoff             = UpsampleBandwidth;
        if (numRate_ > denRate_) {
            // down-sampling
            cutoff = DownsampleBandwidth * denRate_ / numRate_;
            if (!MultiplyFrac(filtLen_, filtLen_, numRate_, denRate_))
                return false;
            filtLen_ = ((filtLen_ - 1) & ~0x7u) + 8;
            for (std::uint32_t f = 2; f <= 16; f *= 2) {
                if (f * denRate_ < numRate_)
                    oversample_ >>= 1;
            }
            if (oversample_ < 1)
                oversample_ = 1;
        }
        if ((INT_MAX / sizeof(float) - 8) / oversample_ < filtLen_)
            return false;
        if (!sincTable_.Allocate(static_cast<std::size_t>(oversample_) * filtLen_ + 8))
            return false;
        const std::int32_t n = static_cast<std::int32_t>(filtLen_);
        for (std::int32_t i = -4; i < static_cast<std::int32_t>(oversample_ * filtLen_ + 4); ++i)
            sincTable_[static_cast<std::size_t>(i + 4)] = Sinc(cutoff, i / static_cast<float>(oversample_) - static_cast<float>(filtLen_ / 2), n);

        memAllocSize_ = filtLen_ - 1 + BufferSize;
        if (!mem_.Allocate(static_cast<std::size_t>(channels) * memAllocSize_) || !lastSample_.Allocate(static_cast<std::size_t>(channels))
            || !sampFracNum_.Allocate(static_cast<std::size_t>(channels)))
            return false;
        for (std::int32_t c = 0; c < channels; ++c)
            lastSample_[c] = static_cast<std::int32_t>(filtLen_ / 2); // speex_resampler_skip_zeros
        return true;
    }

    // libopusenc: speex_resampler_set_rate_frac() (src/resample.c) with the ratio in units of 1 / RatioDen; the table stays.
    void LiveResampler::SetRatio(double ratio) noexcept
    {
        if (!(ratio > 0) || ratio > 256)
            return;
        const auto num = static_cast<std::uint32_t>(ratio * RatioDen + .5);
        if (num == 0 || (num == numRate_ && denRate_ == RatioDen))
            return;
        for (std::int32_t c = 0; c < channels_; ++c) {
            // the position between two input frames, carried over to the new denominator
            std::uint64_t frac = static_cast<std::uint64_t>(sampFracNum_[c]) * RatioDen / denRate_;
            sampFracNum_[c]    = static_cast<std::uint32_t>(frac < RatioDen ? frac : RatioDen - 1);
        }
        numRate_     = num;
        denRate_     = RatioDen;
        intAdvance_  = static_cast<std::int32_t>(numRate_ / denRate_);
        fracAdvance_ = static_cast<std::int32_t>(numRate_ % denRate_);
    }

    // libopusenc: speex_resampler_get_output_latency() (src/resample.c)
    std::int32_t LiveResampler::OutputLatency() const noexcept
    {
        return static_cast<std::int32_t>(((filtLen_ / 2) / nominalRatio_) + .5);
    }

    // libopusenc: speex_resampler_process_interleaved_float() (src/resample.c)
    void LiveResampler::ProcessInterleaved(const float* in, std::uint32_t& inLen, float* out, std::uint32_t& outLen) noexcept
    {
        const std::uint32_t inBackup  = inLen;
        const std::uint32_t outBackup = outLen;
        stride_                       = channels_;
        for (std::int32_t c = 0; c < channels_; ++c) {
            inLen  = inBackup;
            outLen = outBackup;
            ProcessChannel(c, in + c, inLen, out + c, outLen);
        }
        stride_ = 1;
    }

    // libopusenc: speex_resampler_process_float() (src/resample.c) with speex_resampler_process_native (no magic samples).
    void LiveResampler::ProcessChannel(std::int32_t channel, const float* in, std::uint32_t& inLen, float* out, std::uint32_t& outLen) noexcept
    {
        std::uint32_t       ilen     = inLen;
        std::uint32_t       olen     = outLen;
        float*              x        = mem_.Data() + static_cast<std::size_t>(channel) * memAllocSize_;
        const std::uint32_t filtOffs = filtLen_ - 1;
        const std::uint32_t xlen     = memAllocSize_ - filtOffs;
        while (ilen != 0 && olen != 0) {
            std::uint32_t ichunk = ilen > xlen ? xlen : ilen;
            for (std::uint32_t j = 0; j < ichunk; ++j)
                x[j + filtOffs] = in[static_cast<std::size_t>(j) * stride_];

            const std::uint32_t produced = Kernel(channel, x, ichunk, out, olen);
            if (lastSample_[channel] < static_cast<std::int32_t>(ichunk))
                ichunk = static_cast<std::uint32_t>(lastSample_[channel]);
            lastSample_[channel] -= static_cast<std::int32_t>(ichunk);
            for (std::uint32_t j = 0; j < filtOffs; ++j)
                x[j] = x[j + ichunk];

            ilen -= ichunk;
            olen -= produced;
            out += static_cast<std::size_t>(produced) * stride_;
            in += static_cast<std::size_t>(ichunk) * stride_;
        }
        inLen -= ilen;
        outLen -= olen;
    }

    // libopusenc: resampler_basic_interpolate_single() (src/resample.c) (floating point build, generic inner loop).
    std::uint32_t LiveResampler::Kernel(std::int32_t channel, const float* mem, std::uint32_t inLen, float* out, std::uint32_t outLen) noexcept
    {
        std::int32_t       lastSample  = lastSample_[channel];
        std::uint32_t      sampFracNum = sampFracNum_[channel];
        std::uint32_t      produced    = 0;
        const std::int32_t n           = static_cast<std::int32_t>(filtLen_);
        const std::int32_t over        = static_cast<std::int32_t>(oversample_);
        while (!(lastSample >= static_cast<std::int32_t>(inLen) || produced >= outLen)) {
            const float*        iptr   = mem + lastSample;
            const std::uint64_t pos    = static_cast<std::uint64_t>(sampFracNum) * oversample_;
            const auto          offset = static_cast<std::int32_t>(pos / denRate_);
            const float         frac   = static_cast<float>(pos % denRate_) / denRate_;

            // the four neighbouring phases of the oversampled table, interpolated cubically
            const float* table    = sincTable_.Data() + 4 + over - offset - 2;
            float        accum[4] = {0, 0, 0, 0};
            for (std::int32_t j = 0; j < n; ++j) {
                const float  currIn = iptr[j];
                const float* t      = table + static_cast<std::ptrdiff_t>(j) * over;
                accum[0] += currIn * t[0];
                accum[1] += currIn * t[1];
                accum[2] += currIn * t[2];
                accum[3] += currIn * t[3];
            }
            float interp[4]; // cubic_coef
            interp[0] = -0.16667f * frac + 0.16667f * frac * frac * frac;
            interp[1] = frac + 0.5f * frac * frac - 0.5f * frac * frac * frac;
            interp[3] = -0.33333f * frac + 0.5f * frac * frac - 0.16667f * frac * frac * frac;
            interp[2] = static_cast<float>(1. - interp[0] - interp[1] - interp[3]);

            out[static_cast<std::size_t>(stride_) * produced++] = interp[0] * accum[0] + interp[1] * accum[1] + interp[2] * accum[2] + interp[3] * accum[3];
            lastSample += intAdvance_;
            sampFracNum += static_cast<std::uint32_t>(fracAdvance_);
            if (sampFracNum >= denRate_) {
                sampFracNum -= denRate_;
                ++lastSample;
            }
        }
        lastSample_[channel]  = lastSample;
        sampFracNum_[channel] = sampFracNum;
        return produced;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
