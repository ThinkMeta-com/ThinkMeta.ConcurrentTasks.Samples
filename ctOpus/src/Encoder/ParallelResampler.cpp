// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "ParallelResampler.h"

#include <immintrin.h>

#include <cstdint>

#include "IntMath.h"
#include "Resampler.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    bool ParallelResampler::Initialize(const Resampler& resampler) noexcept
    {
        table_  = resampler.SincTable();
        n_      = resampler.FilterLength();
        den_    = resampler.DenRate();
        step_   = resampler.NumRate(); // int_advance * den + frac_advance
        period_ = den_ / Gcd<std::int64_t>(den_, 8) * 8;
        avx2_   = false;
        // no check of the CPU: the programs require AVX2 (CpuCheckEntry), and the encoder library
        // is built for it anyway
        if (n_ % 4 != 0)
            return true;

        const std::int64_t groups = period_ / 8;
        if (!groupCoef_.Allocate(static_cast<std::size_t>(groups) * n_ * 8) || !groupPerm_.Allocate(static_cast<std::size_t>(groups) * 8))
            return false;
        for (std::int64_t g = 0; g < groups; ++g) {
            const std::int64_t r     = g * 8;
            const std::int64_t first = (r * step_) / den_;
            for (std::int32_t k = 0; k < 8; ++k) {
                const std::int64_t offset = ((r + k) * step_) / den_ - first;
                if (offset > 7)
                    return true; // down-sampling: the lanes' windows do not fit one vector, stay scalar
                groupPerm_[static_cast<std::size_t>(g * 8 + k)] = static_cast<std::int32_t>(offset);
                const std::int64_t phase                        = ((r + k) * step_) % den_;
                for (std::int32_t t = 0; t < n_; ++t)
                    groupCoef_[static_cast<std::size_t>((g * n_ + t) * 8 + k)] = table_[phase * n_ + t];
            }
        }
        avx2_ = true;
        return true;
    }

    void ParallelResampler::InitializeIdentity() noexcept
    {
        table_    = nullptr;
        n_        = 1;
        den_      = 1;
        step_     = 1;
        period_   = 8;
        avx2_     = false;
        identity_ = true;
    }

    std::int64_t ParallelResampler::OutputsFor(std::int64_t inputFrames) const noexcept
    {
        // outputs j with n/2 + floor(j * step / den) < inputFrames
        const std::int64_t m = inputFrames - n_ / 2 - 1;
        if (m < 0)
            return 0;
        return ((m + 1) * den_ - 1) / step_ + 1;
    }

    void ParallelResampler::Compute(const float* const* padded, std::int32_t channels, std::int64_t j0, std::int64_t j1, float* out) const noexcept
    {
        if (identity_) { // 48 kHz: interleave the planar input
            for (std::int32_t c = 0; c < channels; ++c)
                for (std::int64_t j = j0; j < j1; ++j)
                    out[(j - j0) * channels + c] = padded[c][j];
            return;
        }
        for (std::int32_t c = 0; c < channels; ++c) {
            if (avx2_)
                ComputeAvx2(padded[c], j0, j1, out + c, channels);
            else
                ComputeScalar(padded[c], j0, j1, out + c, channels);
        }
    }

    // The SSE inner product of resample_sse.h per output: lane m sums taps m, m+4, ...; then (s0 + s2) + (s1 + s3).
    void ParallelResampler::ComputeScalar(const float* padded, std::int64_t j0, std::int64_t j1, float* out, std::int32_t stride) const noexcept
    {
        for (std::int64_t j = j0; j < j1; ++j) {
            const float* sinct = table_ + ((j * step_) % den_) * n_;
            const float* iptr  = padded + TapStart(j);
            float        s[4]  = {0, 0, 0, 0};
            for (std::int32_t t = 0; t < n_; t += 4) {
                s[0] = s[0] + sinct[t] * iptr[t];
                s[1] = s[1] + sinct[t + 1] * iptr[t + 1];
                s[2] = s[2] + sinct[t + 2] * iptr[t + 2];
                s[3] = s[3] + sinct[t + 3] * iptr[t + 3];
            }
            out[(j - j0) * stride] = (s[0] + s[2]) + (s[1] + s[3]);
        }
    }

    // Eight outputs per vector, two groups at once: per group four accumulators (tap mod 4), so
    // every lane adds its products in the order of the SSE kernel; separate mul and add, no FMA.
    void ParallelResampler::ComputeAvx2(const float* padded, std::int64_t j0, std::int64_t j1, float* out, std::int32_t stride) const noexcept
    {
        std::int64_t       j            = j0;
        const std::int64_t alignedStart = (j0 + 7) & ~static_cast<std::int64_t>(7);
        if (alignedStart > j1) {
            ComputeScalar(padded, j0, j1, out, stride);
            return;
        }
        ComputeScalar(padded, j0, alignedStart, out, stride);
        j = alignedStart;

        alignas(32) float lanes[16];
        for (; j + 16 <= j1; j += 16) {
            const std::int64_t g0    = (j % period_) / 8;
            const std::int64_t g1    = ((j + 8) % period_) / 8;
            const float*       c0    = groupCoef_.Data() + g0 * n_ * 8;
            const float*       c1    = groupCoef_.Data() + g1 * n_ * 8;
            const __m256i      perm0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(groupPerm_.Data() + g0 * 8));
            const __m256i      perm1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(groupPerm_.Data() + g1 * 8));
            const float*       b0    = padded + TapStart(j);
            const float*       b1    = padded + TapStart(j + 8);
            __m256             a0 = _mm256_setzero_ps(), a1 = _mm256_setzero_ps(), a2 = _mm256_setzero_ps(), a3 = _mm256_setzero_ps();
            __m256             e0 = _mm256_setzero_ps(), e1 = _mm256_setzero_ps(), e2 = _mm256_setzero_ps(), e3 = _mm256_setzero_ps();
            for (std::int32_t t = 0; t < n_; t += 4) {
                const float* k0 = c0 + static_cast<std::size_t>(t) * 8;
                const float* k1 = c1 + static_cast<std::size_t>(t) * 8;
                a0              = _mm256_add_ps(a0, _mm256_mul_ps(_mm256_loadu_ps(k0), _mm256_permutevar8x32_ps(_mm256_loadu_ps(b0 + t), perm0)));
                e0              = _mm256_add_ps(e0, _mm256_mul_ps(_mm256_loadu_ps(k1), _mm256_permutevar8x32_ps(_mm256_loadu_ps(b1 + t), perm1)));
                a1              = _mm256_add_ps(a1, _mm256_mul_ps(_mm256_loadu_ps(k0 + 8), _mm256_permutevar8x32_ps(_mm256_loadu_ps(b0 + t + 1), perm0)));
                e1              = _mm256_add_ps(e1, _mm256_mul_ps(_mm256_loadu_ps(k1 + 8), _mm256_permutevar8x32_ps(_mm256_loadu_ps(b1 + t + 1), perm1)));
                a2              = _mm256_add_ps(a2, _mm256_mul_ps(_mm256_loadu_ps(k0 + 16), _mm256_permutevar8x32_ps(_mm256_loadu_ps(b0 + t + 2), perm0)));
                e2              = _mm256_add_ps(e2, _mm256_mul_ps(_mm256_loadu_ps(k1 + 16), _mm256_permutevar8x32_ps(_mm256_loadu_ps(b1 + t + 2), perm1)));
                a3              = _mm256_add_ps(a3, _mm256_mul_ps(_mm256_loadu_ps(k0 + 24), _mm256_permutevar8x32_ps(_mm256_loadu_ps(b0 + t + 3), perm0)));
                e3              = _mm256_add_ps(e3, _mm256_mul_ps(_mm256_loadu_ps(k1 + 24), _mm256_permutevar8x32_ps(_mm256_loadu_ps(b1 + t + 3), perm1)));
            }
            _mm256_store_ps(lanes, _mm256_add_ps(_mm256_add_ps(a0, a2), _mm256_add_ps(a1, a3)));
            _mm256_store_ps(lanes + 8, _mm256_add_ps(_mm256_add_ps(e0, e2), _mm256_add_ps(e1, e3)));
            float* o = out + (j - j0) * stride;
            for (std::int32_t k = 0; k < 16; ++k)
                o[k * stride] = lanes[k];
        }
        ComputeScalar(padded, j, j1, out + (j - j0) * stride, stride);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
