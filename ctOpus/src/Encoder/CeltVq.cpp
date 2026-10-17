// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "CeltVq.h"

#include <emmintrin.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "CeltBands.h"
#include "CeltSimd.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    namespace {

#include "CeltCwrsTable.inc"

        constexpr const std::uint32_t* PvqURow[15]
            = {PvqUData + 0,    PvqUData + 176,  PvqUData + 351,  PvqUData + 525,  PvqUData + 698,  PvqUData + 870,  PvqUData + 1041, PvqUData + 1131,
               PvqUData + 1178, PvqUData + 1207, PvqUData + 1226, PvqUData + 1240, PvqUData + 1248, PvqUData + 1254, PvqUData + 1257};

        constexpr std::int32_t Imin(std::int32_t a, std::int32_t b) noexcept
        {
            return a < b ? a : b;
        }
        constexpr std::int32_t Imax(std::int32_t a, std::int32_t b) noexcept
        {
            return a > b ? a : b;
        }

        // CELT_PVQ_U and CELT_PVQ_V of cwrs.c
        inline std::uint32_t PvqU(std::int32_t n, std::int32_t k) noexcept
        {
            return PvqURow[Imin(n, k)][Imax(n, k)];
        }
        inline std::uint32_t PvqV(std::int32_t n, std::int32_t k) noexcept
        {
            return PvqU(n, k) + PvqU(n, k + 1);
        }

        // icwrs: the index of the pulse vector y
        std::uint32_t Icwrs(std::int32_t n, const std::int32_t* y) noexcept
        {
            std::int32_t  j = n - 1;
            std::uint32_t i = y[j] < 0 ? 1 : 0;
            std::int32_t  k = std::abs(y[j]);
            do {
                --j;
                i += PvqU(n - j, k);
                k += std::abs(y[j]);
                if (y[j] < 0)
                    i += PvqU(n - j, k + 1);
            } while (j > 0);
            return i;
        }

        void EncodePulses(const std::int32_t* y, std::int32_t n, std::int32_t k, RangeEncoder* enc) noexcept
        {
            enc->EncodeUint(Icwrs(n, y), PvqV(n, k));
        }

        // celt_cos_norm of the float build: cos(pi/2 * x) in double
        inline float CosNorm(float x) noexcept
        {
            return static_cast<float>(std::cos((.5f * 3.1415926535897931) * x));
        }

        // celt_atan_norm / celt_atan2p_norm (mathops.h, float)
        inline float AtanNorm(float x) noexcept
        {
            constexpr float twoOverPi = 0.636619772367581f;
            constexpr float a03       = -3.3331659436225891113281250000e-01f;
            constexpr float a05       = 1.99627041816711425781250000000e-01f;
            constexpr float a07       = -1.3976582884788513183593750000e-01f;
            constexpr float a09       = 9.79423448443412780761718750000e-02f;
            constexpr float a11       = -5.7773590087890625000000000000e-02f;
            constexpr float a13       = 2.30401363223791122436523437500e-02f;
            constexpr float a15       = -4.3554059229791164398193359375e-03f;
            const float     xSq       = x * x;
            return twoOverPi * (x + x * xSq * (a03 + xSq * (a05 + xSq * (a07 + xSq * (a09 + xSq * (a11 + xSq * (a13 + xSq * (a15))))))));
        }

        inline float Atan2pNorm(float y, float x) noexcept
        {
            // for very small values the answer does not matter
            if ((x * x + y * y) < 1e-18f)
                return 0;
            if (y < x)
                return AtanNorm(y / x);
            return 1.f - AtanNorm(x / y);
        }

        void ExpRotation1(float* x, std::int32_t len, std::int32_t stride, float c, float s) noexcept
        {
            const float ms   = -s;
            float*      xptr = x;
            for (std::int32_t i = 0; i < len - stride; ++i) {
                const float x1 = xptr[0];
                const float x2 = xptr[stride];
                xptr[stride]   = c * x2 + s * x1;
                *xptr++        = c * x1 + ms * x2;
            }
            xptr = &x[len - 2 * stride - 1];
            for (std::int32_t i = len - 2 * stride - 1; i >= 0; --i) {
                const float x1 = xptr[0];
                const float x2 = xptr[stride];
                xptr[stride]   = c * x2 + s * x1;
                *xptr--        = c * x1 + ms * x2;
            }
        }

        void ExpRotation(float* x, std::int32_t len, std::int32_t dir, std::int32_t stride, std::int32_t k, std::int32_t spread) noexcept
        {
            static constexpr std::int32_t spreadFactor[3] = {15, 10, 5};
            if (2 * k >= len || spread == SpreadNone)
                return;
            const std::int32_t factor = spreadFactor[spread - 1];

            const float gain  = (1.f * static_cast<float>(len)) / static_cast<float>(len + factor * k);
            const float theta = .5f * (gain * gain);

            const float c = CosNorm(theta);
            const float s = CosNorm(1.f - theta); // sin(theta)

            std::int32_t stride2 = 0;
            if (len >= 8 * stride) {
                stride2 = 1;
                // sqrt(len/stride) with rounding: increment while (stride2+0.5)^2 < len/stride
                while ((stride2 * stride2 + stride2) * stride + (stride >> 2) < len)
                    ++stride2;
            }
            len = static_cast<std::int32_t>(static_cast<std::uint32_t>(len) / static_cast<std::uint32_t>(stride));
            for (std::int32_t i = 0; i < stride; ++i) {
                if (dir < 0) {
                    if (stride2 != 0)
                        ExpRotation1(x + i * len, len, stride2, s, c);
                    ExpRotation1(x + i * len, len, 1, c, s);
                } else {
                    ExpRotation1(x + i * len, len, 1, c, -s);
                    if (stride2 != 0)
                        ExpRotation1(x + i * len, len, stride2, s, -c);
                }
            }
        }

        // the integer codeword to unit norm (times gain)
        void NormaliseResidual(const std::int32_t* iy, float* x, std::int32_t n, float ryy, float gain) noexcept
        {
            const float g = (1.f / static_cast<float>(std::sqrt(static_cast<double>(ryy)))) * gain;
            for (std::int32_t i = 0; i < n; ++i)
                x[i] = iy[i] * g;
        }

        std::uint32_t ExtractCollapseMask(const std::int32_t* iy, std::int32_t n, std::int32_t b) noexcept
        {
            if (b <= 1)
                return 1;
            const std::int32_t n0           = static_cast<std::int32_t>(static_cast<std::uint32_t>(n) / static_cast<std::uint32_t>(b));
            std::uint32_t      collapseMask = 0;
            for (std::int32_t i = 0; i < b; ++i) {
                std::uint32_t tmp = 0;
                for (std::int32_t j = 0; j < n0; ++j)
                    tmp |= static_cast<std::uint32_t>(iy[i * n0 + j]);
                collapseMask |= static_cast<std::uint32_t>(tmp != 0) << i;
            }
            return collapseMask;
        }

        // op_pvq_search_sse2 (libopus uses it unconditionally on x64); iy has room for n + 3
        float PvqSearch(const float* xIn, std::int32_t* iy, std::int32_t k, std::int32_t n) noexcept
        {
            float         y[176 + 3];
            float         x[176 + 3];
            float         signy[176 + 3];
            const __m128  signmask = _mm_set_ps1(-0.f); // only the sign bit set
            const __m128i fours    = _mm_set_epi32(4, 4, 4, 4);

            std::memcpy(x, xIn, static_cast<std::size_t>(n) * sizeof(float));
            x[n] = x[n + 1] = x[n + 2] = 0;
            __m128 sums                = _mm_setzero_ps();
            for (std::int32_t j = 0; j < n; j += 4) {
                __m128       x4 = _mm_loadu_ps(&x[j]);
                const __m128 s4 = _mm_cmplt_ps(x4, _mm_setzero_ps());
                // no sign
                x4   = _mm_andnot_ps(signmask, x4);
                sums = _mm_add_ps(sums, x4);
                // y and iy cleared in case there is no projection
                _mm_storeu_ps(&y[j], _mm_setzero_ps());
                _mm_storeu_si128(reinterpret_cast<__m128i*>(&iy[j]), _mm_setzero_si128());
                _mm_storeu_ps(&x[j], x4);
                _mm_storeu_ps(&signy[j], s4);
            }
            sums = _mm_add_ps(sums, _mm_shuffle_ps(sums, sums, _MM_SHUFFLE(1, 0, 3, 2)));
            sums = _mm_add_ps(sums, _mm_shuffle_ps(sums, sums, _MM_SHUFFLE(2, 3, 0, 1)));

            float        xy         = 0;
            float        yy         = 0;
            std::int32_t pulsesLeft = k;

            // a pre-search by projecting on the pyramid
            if (k > (n >> 1)) {
                const float sum = _mm_cvtss_f32(sums);
                // X too small: a pulse at 0 instead (against infinities and NaNs allocating too
                // many pulses; 64 approximates infinity here)
                if (!(sum > 1e-15f && sum < 64)) {
                    x[0] = 1.f;
                    for (std::int32_t j = 1; j < n; ++j)
                        x[j] = 0;
                    sums = _mm_set_ps1(1.f);
                }
                // K+e with e < 1 cannot give more than K pulses
                const __m128 rcp4      = _mm_mul_ps(_mm_set_ps1(static_cast<float>(k + .8)), _mm_rcp_ps(sums));
                __m128       xy4       = _mm_setzero_ps();
                __m128       yy4       = _mm_setzero_ps();
                __m128i      pulsesSum = _mm_setzero_si128();
                for (std::int32_t j = 0; j < n; j += 4) {
                    const __m128  x4  = _mm_loadu_ps(&x[j]);
                    const __m128  rx4 = _mm_mul_ps(x4, rcp4);
                    const __m128i iy4 = _mm_cvttps_epi32(rx4);
                    pulsesSum         = _mm_add_epi32(pulsesSum, iy4);
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(&iy[j]), iy4);
                    const __m128 y4 = _mm_cvtepi32_ps(iy4);
                    xy4             = _mm_add_ps(xy4, _mm_mul_ps(x4, y4));
                    yy4             = _mm_add_ps(yy4, _mm_mul_ps(y4, y4));
                    // y doubled, so the search loop need not
                    _mm_storeu_ps(&y[j], _mm_add_ps(y4, y4));
                }
                pulsesSum = _mm_add_epi32(pulsesSum, _mm_shuffle_epi32(pulsesSum, _MM_SHUFFLE(1, 0, 3, 2)));
                pulsesSum = _mm_add_epi32(pulsesSum, _mm_shuffle_epi32(pulsesSum, _MM_SHUFFLE(2, 3, 0, 1)));
                pulsesLeft -= _mm_cvtsi128_si32(pulsesSum);
                xy4 = _mm_add_ps(xy4, _mm_shuffle_ps(xy4, xy4, _MM_SHUFFLE(1, 0, 3, 2)));
                xy4 = _mm_add_ps(xy4, _mm_shuffle_ps(xy4, xy4, _MM_SHUFFLE(2, 3, 0, 1)));
                xy  = _mm_cvtss_f32(xy4);
                yy4 = _mm_add_ps(yy4, _mm_shuffle_ps(yy4, yy4, _MM_SHUFFLE(1, 0, 3, 2)));
                yy4 = _mm_add_ps(yy4, _mm_shuffle_ps(yy4, yy4, _MM_SHUFFLE(2, 3, 0, 1)));
                yy  = _mm_cvtss_f32(yy4);
            }
            x[n] = x[n + 1] = x[n + 2] = -100;
            y[n] = y[n + 1] = y[n + 2] = 100;

            // should never happen (e.g. on silence): the first bin gets the pulses
            if (pulsesLeft > n + 3) {
                const auto tmp = static_cast<float>(pulsesLeft);
                yy             = yy + tmp * tmp;
                yy             = yy + tmp * y[0];
                iy[0] += pulsesLeft;
                pulsesLeft = 0;
            }

            for (std::int32_t i = 0; i < pulsesLeft; ++i) {
                // the squared magnitude term is added anyway, so outside the loop
                yy                 = yy + 1;
                const __m128 xy4   = _mm_load1_ps(&xy);
                const __m128 yy4   = _mm_load1_ps(&yy);
                __m128       max   = _mm_setzero_ps();
                __m128i      pos   = _mm_setzero_si128();
                __m128i      count = _mm_set_epi32(3, 2, 1, 0);
                for (std::int32_t j = 0; j < n; j += 4) {
                    __m128 x4       = _mm_loadu_ps(&x[j]);
                    __m128 y4       = _mm_loadu_ps(&y[j]);
                    x4              = _mm_add_ps(x4, xy4);
                    y4              = _mm_add_ps(y4, yy4);
                    y4              = _mm_rsqrt_ps(y4);
                    const __m128 r4 = _mm_mul_ps(x4, y4);
                    // the index of the max, the max, the indices (+4)
                    pos   = _mm_max_epi16(pos, _mm_and_si128(count, _mm_castps_si128(_mm_cmpgt_ps(r4, max))));
                    max   = _mm_max_ps(max, r4);
                    count = _mm_add_epi32(count, fours);
                }
                // horizontal max, then which partial max equals the global one
                __m128 max2               = _mm_max_ps(max, _mm_shuffle_ps(max, max, _MM_SHUFFLE(1, 0, 3, 2)));
                max2                      = _mm_max_ps(max2, _mm_shuffle_ps(max2, max2, _MM_SHUFFLE(2, 3, 0, 1)));
                pos                       = _mm_and_si128(pos, _mm_castps_si128(_mm_cmpeq_ps(max, max2)));
                pos                       = _mm_max_epi16(pos, _mm_unpackhi_epi64(pos, pos));
                pos                       = _mm_max_epi16(pos, _mm_shufflelo_epi16(pos, _MM_SHUFFLE(1, 0, 3, 2)));
                const std::int32_t bestId = _mm_cvtsi128_si32(pos);

                // the sums with the new pulse
                xy = xy + x[bestId];
                // y is doubled, so no factor here
                yy = yy + y[bestId];
                // only now update y/iy (y doubled)
                y[bestId] += 2;
                ++iy[bestId];
            }

            // the original sign back
            for (std::int32_t j = 0; j < n; j += 4) {
                __m128i       y4 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&iy[j]));
                const __m128i s4 = _mm_castps_si128(_mm_loadu_ps(&signy[j]));
                y4               = _mm_xor_si128(_mm_add_epi32(y4, s4), s4);
                _mm_storeu_si128(reinterpret_cast<__m128i*>(&iy[j]), y4);
            }
            return yy;
        }

    } // namespace

    std::uint32_t AlgQuant(float* x, std::int32_t n, std::int32_t k, std::int32_t spread, std::int32_t b, RangeEncoder* enc, float gain, bool resynth) noexcept
    {
        // up to 4-way vectorisation
        std::int32_t iy[176 + 3];

        ExpRotation(x, n, 1, b, k, spread);

        const float         yy           = PvqSearch(x, iy, k, n);
        const std::uint32_t collapseMask = ExtractCollapseMask(iy, n, b);
        EncodePulses(iy, n, k, enc);
        if (resynth) {
            NormaliseResidual(iy, x, n, yy, gain);
            ExpRotation(x, n, -1, b, k, spread);
        }
        return collapseMask;
    }

    void RenormaliseVector(float* x, std::int32_t n, float gain) noexcept
    {
        const float e = 1e-15f + InnerProd(x, x, n);
        const float g = (1.f / static_cast<float>(std::sqrt(static_cast<double>(e)))) * gain;
        for (std::int32_t i = 0; i < n; ++i)
            x[i] = g * x[i];
    }

    std::int32_t StereoItheta(const float* x, const float* y, bool stereo, std::int32_t n) noexcept
    {
        float emid  = 0;
        float eside = 0;
        if (stereo) {
            for (std::int32_t i = 0; i < n; ++i) {
                const float m = x[i] + y[i];
                const float s = x[i] - y[i];
                emid          = emid + m * m;
                eside         = eside + s * s;
            }
        } else {
            emid += InnerProd(x, x, n);
            eside += InnerProd(y, y, n);
        }
        const auto mid  = static_cast<float>(std::sqrt(static_cast<double>(emid)));
        const auto side = static_cast<float>(std::sqrt(static_cast<double>(eside)));
        return static_cast<std::int32_t>(std::floor(.5f + 65536.f * 16384 * Atan2pNorm(side, mid)));
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
