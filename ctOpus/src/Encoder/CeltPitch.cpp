// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "CeltPitch.h"

#include <immintrin.h>
#include <intrin.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "CeltMath.h"
#include "CeltSimd.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    namespace {

        constexpr std::int32_t MinPeriod = 15; // COMBFILTER_MINPERIOD

        // xcorr_kernel_avx: like xcorr_kernel, but 8 results at a time (FMA partial sums)
        void XcorrKernelAvx(const float* x, const float* y, float sum[8], std::int32_t len) noexcept
        {
            __m256       xsum0 = _mm256_setzero_ps();
            __m256       xsum1 = xsum0;
            __m256       xsum2 = xsum0;
            __m256       xsum3 = xsum0;
            __m256       xsum4 = xsum0;
            __m256       xsum5 = xsum0;
            __m256       xsum6 = xsum0;
            __m256       xsum7 = xsum0;
            std::int32_t i     = 0;
            for (i = 0; i < len - 7; i += 8) {
                const __m256 x0 = _mm256_loadu_ps(x + i);
                xsum0           = _mm256_fmadd_ps(x0, _mm256_loadu_ps(y + i), xsum0);
                xsum1           = _mm256_fmadd_ps(x0, _mm256_loadu_ps(y + i + 1), xsum1);
                xsum2           = _mm256_fmadd_ps(x0, _mm256_loadu_ps(y + i + 2), xsum2);
                xsum3           = _mm256_fmadd_ps(x0, _mm256_loadu_ps(y + i + 3), xsum3);
                xsum4           = _mm256_fmadd_ps(x0, _mm256_loadu_ps(y + i + 4), xsum4);
                xsum5           = _mm256_fmadd_ps(x0, _mm256_loadu_ps(y + i + 5), xsum5);
                xsum6           = _mm256_fmadd_ps(x0, _mm256_loadu_ps(y + i + 6), xsum6);
                xsum7           = _mm256_fmadd_ps(x0, _mm256_loadu_ps(y + i + 7), xsum7);
            }
            if (i != len) {
                static const std::int32_t mask[15] = {-1, -1, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0};
                const __m256i             m        = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(mask + 7 + i - len));
                const __m256              x0       = _mm256_maskload_ps(x + i, m);
                xsum0                              = _mm256_fmadd_ps(x0, _mm256_maskload_ps(y + i, m), xsum0);
                xsum1                              = _mm256_fmadd_ps(x0, _mm256_maskload_ps(y + i + 1, m), xsum1);
                xsum2                              = _mm256_fmadd_ps(x0, _mm256_maskload_ps(y + i + 2, m), xsum2);
                xsum3                              = _mm256_fmadd_ps(x0, _mm256_maskload_ps(y + i + 3, m), xsum3);
                xsum4                              = _mm256_fmadd_ps(x0, _mm256_maskload_ps(y + i + 4, m), xsum4);
                xsum5                              = _mm256_fmadd_ps(x0, _mm256_maskload_ps(y + i + 5, m), xsum5);
                xsum6                              = _mm256_fmadd_ps(x0, _mm256_maskload_ps(y + i + 6, m), xsum6);
                xsum7                              = _mm256_fmadd_ps(x0, _mm256_maskload_ps(y + i + 7, m), xsum7);
            }
            // 8 horizontal adds: [0 4] [1 5] [2 6] [3 7], then [0 1 4 5] [2 3 6 7], then [0..7]
            xsum0 = _mm256_add_ps(_mm256_permute2f128_ps(xsum0, xsum4, 2 << 4), _mm256_permute2f128_ps(xsum0, xsum4, 1 | (3 << 4)));
            xsum1 = _mm256_add_ps(_mm256_permute2f128_ps(xsum1, xsum5, 2 << 4), _mm256_permute2f128_ps(xsum1, xsum5, 1 | (3 << 4)));
            xsum2 = _mm256_add_ps(_mm256_permute2f128_ps(xsum2, xsum6, 2 << 4), _mm256_permute2f128_ps(xsum2, xsum6, 1 | (3 << 4)));
            xsum3 = _mm256_add_ps(_mm256_permute2f128_ps(xsum3, xsum7, 2 << 4), _mm256_permute2f128_ps(xsum3, xsum7, 1 | (3 << 4)));
            xsum0 = _mm256_hadd_ps(xsum0, xsum1);
            xsum1 = _mm256_hadd_ps(xsum2, xsum3);
            xsum0 = _mm256_hadd_ps(xsum0, xsum1);
            _mm256_storeu_ps(sum, xsum0);
        }

        void FindBestPitch(const float* xcorr, const float* y, std::int32_t len, std::int32_t maxPitch, std::int32_t* bestPitch) noexcept
        {
            float bestNum[2] = {-1, -1};
            float bestDen[2] = {0, 0};
            float syy        = 1;
            bestPitch[0]     = 0;
            bestPitch[1]     = 1;
            for (std::int32_t j = 0; j < len; ++j)
                syy = syy + y[j] * y[j];
            for (std::int32_t i = 0; i < maxPitch; ++i) {
                if (xcorr[i] > 0) {
                    float xcorr16 = xcorr[i];
                    // against underflows and overflows (inf) when squaring
                    xcorr16 *= 1e-12f;
                    const float num = xcorr16 * xcorr16;
                    if (num * bestDen[1] > bestNum[1] * syy) {
                        if (num * bestDen[0] > bestNum[0] * syy) {
                            bestNum[1]   = bestNum[0];
                            bestDen[1]   = bestDen[0];
                            bestPitch[1] = bestPitch[0];
                            bestNum[0]   = num;
                            bestDen[0]   = syy;
                            bestPitch[0] = i;
                        } else {
                            bestNum[1]   = num;
                            bestDen[1]   = syy;
                            bestPitch[1] = i;
                        }
                    }
                }
                syy += y[i + len] * y[i + len] - y[i] * y[i];
                syy = Max32(1, syy);
            }
        }

        void Fir5(float* x, const float* num, std::int32_t n) noexcept
        {
            const float num0 = num[0];
            const float num1 = num[1];
            const float num2 = num[2];
            const float num3 = num[3];
            const float num4 = num[4];
            float       mem0 = 0;
            float       mem1 = 0;
            float       mem2 = 0;
            float       mem3 = 0;
            float       mem4 = 0;
            for (std::int32_t i = 0; i < n; ++i) {
                float sum = x[i];
                sum       = sum + num0 * mem0;
                sum       = sum + num1 * mem1;
                sum       = sum + num2 * mem2;
                sum       = sum + num3 * mem3;
                sum       = sum + num4 * mem4;
                mem4      = mem3;
                mem3      = mem2;
                mem2      = mem1;
                mem1      = mem0;
                mem0      = x[i];
                x[i]      = sum;
            }
        }

        // _celt_autocorr without a window
        void Autocorr(const float* x, float* ac, std::int32_t lag, std::int32_t n, std::int32_t arch) noexcept
        {
            const std::int32_t fastN = n - lag;
            PitchXcorr(x, x, ac, fastN, lag + 1, arch);
            for (std::int32_t k = 0; k <= lag; ++k) {
                float d = 0;
                for (std::int32_t i = k + fastN; i < n; ++i)
                    d = d + x[i] * x[i - k];
                ac[k] += d;
            }
        }

        // _celt_lpc
        void Lpc(float* lpc, const float* ac, std::int32_t p) noexcept
        {
            float error = ac[0];
            std::memset(lpc, 0, static_cast<std::size_t>(p) * sizeof(float));
            if (ac[0] > 1e-10f) {
                for (std::int32_t i = 0; i < p; ++i) {
                    // the reflection coefficient of this iteration
                    float rr = 0;
                    for (std::int32_t j = 0; j < i; ++j)
                        rr += lpc[j] * ac[i - j];
                    rr += ac[i + 1];
                    const float r = -(rr / error);
                    // the LPC coefficients and the total error
                    lpc[i] = r;
                    for (std::int32_t j = 0; j < (i + 1) >> 1; ++j) {
                        const float tmp1 = lpc[j];
                        const float tmp2 = lpc[i - 1 - j];
                        lpc[j]           = tmp1 + r * tmp2;
                        lpc[i - 1 - j]   = tmp2 + r * tmp1;
                    }
                    error = error - (r * r) * error;
                    // bail out at 30 dB gain
                    if (error <= .001f * ac[0])
                        break;
                }
            }
        }

        float ComputePitchGain(float xy, float xx, float yy) noexcept
        {
            return xy / static_cast<float>(std::sqrt(static_cast<double>(1 + xx * yy)));
        }

        constexpr std::int32_t SecondCheck[16] = {0, 0, 3, 2, 3, 2, 5, 2, 3, 2, 3, 2, 5, 2, 3, 2};

        inline std::int32_t Udiv(std::int32_t n, std::int32_t d) noexcept
        {
            return static_cast<std::int32_t>(static_cast<std::uint32_t>(n) / static_cast<std::uint32_t>(d));
        }

    } // namespace

    std::int32_t SelectArch() noexcept
    {
        // the CPU features: SSE, SSE2, SSE4.1, AVX2 (with FMA and the AVX2 bit of leaf 7)
        int info[4] = {};
        __cpuid(info, 0);
        const int nIds = info[0];
        if (nIds < 1)
            return 0;
        __cpuid(info, 1);
        const bool sse   = (info[3] & (1 << 25)) != 0;
        const bool sse2  = (info[3] & (1 << 26)) != 0;
        const bool sse41 = (info[2] & (1 << 19)) != 0;
        bool       avx2  = (info[2] & (1 << 28)) != 0 && (info[2] & (1 << 12)) != 0;
        if (avx2 && nIds >= 7) {
            __cpuid(info, 7);
            avx2 = (info[1] & (1 << 5)) != 0;
        } else {
            avx2 = false;
        }
        // the levels build on each other
        if (!sse)
            return 0;
        if (!sse2)
            return 1;
        if (!sse41)
            return 2;
        return avx2 ? ArchAvx2 : 3;
    }

    void PitchXcorr(const float* x, const float* y, float* xcorr, std::int32_t len, std::int32_t maxPitch, std::int32_t arch) noexcept
    {
        std::int32_t i = 0;
        if (arch >= ArchAvx2) {
            for (i = 0; i < maxPitch - 7; i += 8)
                XcorrKernelAvx(x, y + i, &xcorr[i], len);
        } else {
            for (i = 0; i < maxPitch - 3; i += 4) {
                float sum[4] = {0, 0, 0, 0};
                XcorrKernel(x, y + i, sum, len);
                xcorr[i]     = sum[0];
                xcorr[i + 1] = sum[1];
                xcorr[i + 2] = sum[2];
                xcorr[i + 3] = sum[3];
            }
        }
        // the rest, not unrolled
        for (; i < maxPitch; ++i)
            xcorr[i] = InnerProd(x, y + i, len);
    }

    void PitchDownsample(float* const x[], float* xLp, std::int32_t len, std::int32_t c, std::int32_t factor, std::int32_t arch) noexcept
    {
        float              ac[5];
        float              tmp = 1.0f;
        float              lpc[4];
        float              lpc2[5];
        constexpr float    c1     = .8f;
        const std::int32_t offset = factor / 2;
        for (std::int32_t i = 1; i < len; ++i)
            xLp[i] = .25f * x[0][(factor * i - offset)] + .25f * x[0][(factor * i + offset)] + .5f * x[0][factor * i];
        xLp[0] = .25f * x[0][offset] + .5f * x[0][0];
        if (c == 2) {
            for (std::int32_t i = 1; i < len; ++i)
                xLp[i] += .25f * x[1][(factor * i - offset)] + .25f * x[1][(factor * i + offset)] + .5f * x[1][factor * i];
            xLp[0] += .25f * x[1][offset] + .5f * x[1][0];
        }
        Autocorr(xLp, ac, 4, len, arch);

        // noise floor -40 dB
        ac[0] *= 1.0001f;
        // lag windowing
        for (std::int32_t i = 1; i <= 4; ++i)
            ac[i] -= ac[i] * (.008f * i) * (.008f * i);

        Lpc(lpc, ac, 4);
        for (std::int32_t i = 0; i < 4; ++i) {
            tmp    = .9f * tmp;
            lpc[i] = lpc[i] * tmp;
        }
        // add a zero
        lpc2[0] = lpc[0] + .8f;
        lpc2[1] = lpc[1] + c1 * lpc[0];
        lpc2[2] = lpc[2] + c1 * lpc[1];
        lpc2[3] = lpc[3] + c1 * lpc[2];
        lpc2[4] = c1 * lpc[3];
        Fir5(xLp, lpc2, len);
    }

    void PitchSearch(const float* xLp, const float* y, std::int32_t len, std::int32_t maxPitch, std::int32_t* pitch, std::int32_t arch) noexcept
    {
        std::int32_t       bestPitch[2] = {0, 0};
        const std::int32_t lag          = len + maxPitch;
        float              xLp4[960 >> 2];
        float              yLp4[(960 + 1024) >> 2];
        float              xcorr[1024 >> 1];

        // down-sample by 2 again
        for (std::int32_t j = 0; j < len >> 2; ++j)
            xLp4[j] = xLp[2 * j];
        for (std::int32_t j = 0; j < lag >> 2; ++j)
            yLp4[j] = y[2 * j];

        // coarse search with 4x decimation
        PitchXcorr(xLp4, yLp4, xcorr, len >> 2, maxPitch >> 2, arch);
        FindBestPitch(xcorr, yLp4, len >> 2, maxPitch >> 2, bestPitch);

        // finer search with 2x decimation
        for (std::int32_t i = 0; i < maxPitch >> 1; ++i) {
            xcorr[i] = 0;
            if (std::abs(i - 2 * bestPitch[0]) > 2 && std::abs(i - 2 * bestPitch[1]) > 2)
                continue;
            const float sum = InnerProd(xLp, y + i, len >> 1);
            xcorr[i]        = Max32(-1, sum);
        }
        FindBestPitch(xcorr, y, len >> 1, maxPitch >> 1, bestPitch);

        // refine by pseudo-interpolation
        std::int32_t offset = 0;
        if (bestPitch[0] > 0 && bestPitch[0] < (maxPitch >> 1) - 1) {
            const float a = xcorr[bestPitch[0] - 1];
            const float b = xcorr[bestPitch[0]];
            const float c = xcorr[bestPitch[0] + 1];
            if ((c - a) > .7f * (b - a))
                offset = 1;
            else if ((a - c) > .7f * (b - c))
                offset = -1;
        }
        *pitch = 2 * bestPitch[0] - offset;
    }

    float RemoveDoubling(float* x, std::int32_t maxPeriod, std::int32_t minPeriod, std::int32_t n, std::int32_t* t0Out, std::int32_t prevPeriod,
                         float prevGain) noexcept
    {
        const std::int32_t minPeriod0 = minPeriod;
        maxPeriod /= 2;
        minPeriod /= 2;
        *t0Out /= 2;
        prevPeriod /= 2;
        n /= 2;
        x += maxPeriod;
        if (*t0Out >= maxPeriod)
            *t0Out = maxPeriod - 1;
        const std::int32_t t0 = *t0Out;
        std::int32_t       t  = t0;

        float yyLookup[(1024 / 2) + 1];
        float xx = 0;
        float xy = 0;
        DualInnerProd(x, x, x - t0, n, &xx, &xy);
        yyLookup[0] = xx;
        float yy    = xx;
        for (std::int32_t i = 1; i <= maxPeriod; ++i) {
            yy          = yy + x[-i] * x[-i] - x[n - i] * x[n - i];
            yyLookup[i] = Max32(0, yy);
        }
        yy                 = yyLookup[t0];
        float       bestXy = xy;
        float       bestYy = yy;
        const float g0     = ComputePitchGain(xy, xx, yy);
        float       g      = g0;
        // any pitch at T/k
        for (std::int32_t k = 2; k <= 15; ++k) {
            const std::int32_t t1 = Udiv(2 * t0 + k, 2 * k);
            if (t1 < minPeriod)
                break;
            // another strong correlation at T1b
            std::int32_t t1b = 0;
            if (k == 2)
                t1b = t1 + t0 > maxPeriod ? t0 : t0 + t1;
            else
                t1b = Udiv(2 * SecondCheck[k] * t0 + k, 2 * k);
            float xy2 = 0;
            DualInnerProd(x, &x[-t1], &x[-t1b], n, &xy, &xy2);
            xy               = .5f * (xy + xy2);
            yy               = .5f * (yyLookup[t1] + yyLookup[t1b]);
            const float g1   = ComputePitchGain(xy, xx, yy);
            float       cont = 0;
            if (std::abs(t1 - prevPeriod) <= 1)
                cont = prevGain;
            else if (std::abs(t1 - prevPeriod) <= 2 && 5 * k * k < t0)
                cont = .5f * prevGain;
            else
                cont = 0;
            float thresh = Max32(.3f, .7f * g0 - cont);
            // bias against very high pitch (very short period), against false positives from
            // short-term correlation
            if (t1 < 3 * minPeriod)
                thresh = Max32(.4f, .85f * g0 - cont);
            else if (t1 < 2 * minPeriod)
                thresh = Max32(.5f, .9f * g0 - cont);
            if (g1 > thresh) {
                bestXy = xy;
                bestYy = yy;
                t      = t1;
                g      = g1;
            }
        }
        bestXy   = Max32(0, bestXy);
        float pg = 0;
        if (bestYy <= bestXy)
            pg = 1.0f;
        else
            pg = bestXy / (bestYy + 1);

        float xcorr[3];
        for (std::int32_t k = 0; k < 3; ++k)
            xcorr[k] = InnerProd(x, x - (t + k - 1), n);
        std::int32_t offset = 0;
        if ((xcorr[2] - xcorr[0]) > .7f * (xcorr[1] - xcorr[0]))
            offset = 1;
        else if ((xcorr[0] - xcorr[2]) > .7f * (xcorr[1] - xcorr[2]))
            offset = -1;
        if (pg > g)
            pg = g;
        *t0Out = 2 * t + offset;
        if (*t0Out < minPeriod0)
            *t0Out = minPeriod0;
        return pg;
    }

    void CombFilter(float* y, float* x, std::int32_t t0, std::int32_t t1, std::int32_t n, float g0, float g1, std::int32_t tapset0, std::int32_t tapset1,
                    const float* window, std::int32_t overlap) noexcept
    {
        static constexpr float gains[3][3]
            = {{0.3066406250f, 0.2170410156f, 0.1296386719f}, {0.4638671875f, 0.2680664062f, 0.f}, {0.7998046875f, 0.1000976562f, 0.f}};
        if (g0 == 0 && g1 == 0) {
            if (x != y)
                std::memmove(y, x, static_cast<std::size_t>(n) * sizeof(float));
            return;
        }
        // a gain of zero comes with T0 and/or T1 zero: at least the minimum period, not garbage
        t0              = t0 > MinPeriod ? t0 : MinPeriod;
        t1              = t1 > MinPeriod ? t1 : MinPeriod;
        const float g00 = g0 * gains[tapset0][0];
        const float g01 = g0 * gains[tapset0][1];
        const float g02 = g0 * gains[tapset0][2];
        const float g10 = g1 * gains[tapset1][0];
        const float g11 = g1 * gains[tapset1][1];
        const float g12 = g1 * gains[tapset1][2];
        float       x1  = x[-t1 + 1];
        float       x2  = x[-t1];
        float       x3  = x[-t1 - 1];
        float       x4  = x[-t1 - 2];
        // an unchanged filter needs no overlap
        if (g0 == g1 && t0 == t1 && tapset0 == tapset1)
            overlap = 0;
        std::int32_t i = 0;
        for (i = 0; i < overlap; ++i) {
            const float x0 = x[i - t1 + 2];
            const float f  = window[i] * window[i];
            y[i]           = x[i] + ((1.0f - f) * g00) * x[i - t0] + ((1.0f - f) * g01) * (x[i - t0 + 1] + x[i - t0 - 1])
                + ((1.0f - f) * g02) * (x[i - t0 + 2] + x[i - t0 - 2]) + (f * g10) * x2 + (f * g11) * (x1 + x3) + (f * g12) * (x0 + x4);
            x4 = x3;
            x3 = x2;
            x2 = x1;
            x1 = x0;
        }
        if (g1 == 0) {
            if (x != y)
                std::memmove(y + overlap, x + overlap, static_cast<std::size_t>(n - overlap) * sizeof(float));
            return;
        }
        // the part with the constant filter
        CombFilterConst(y + i, x + i, t1, n - i, g10, g11, g12);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
