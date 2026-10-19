// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "SilkSigProcFlp.h"

#include <cstring>
#include <immintrin.h>

#include "CeltPitch.h"
#include "SilkMacros.h"
#include "SilkState.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk {

    namespace {

        constexpr std::int32_t MaxOrderLpc = 24; // SILK_MAX_ORDER_LPC

        double InnerProductFlpC(const float* data1, const float* data2, std::int32_t dataSize) noexcept
        {
            // 4x unrolled
            double       result = 0.0;
            std::int32_t i      = 0;
            for (; i < dataSize - 3; i += 4) {
                result += data1[i + 0] * static_cast<double>(data2[i + 0]) + data1[i + 1] * static_cast<double>(data2[i + 1])
                    + data1[i + 2] * static_cast<double>(data2[i + 2]) + data1[i + 3] * static_cast<double>(data2[i + 3]);
            }
            // the remaining products
            for (; i < dataSize; ++i)
                result += data1[i] * static_cast<double>(data2[i]);
            return result;
        }

        double InnerProductFlpAvx2(const float* data1, const float* data2, std::int32_t dataSize) noexcept
        {
            __m256d      accum1 = _mm256_setzero_pd();
            __m256d      accum2 = accum1;
            std::int32_t i      = 0;
            for (; i < dataSize - 7; i += 8) {
                accum1 = _mm256_fmadd_pd(_mm256_cvtps_pd(_mm_loadu_ps(&data1[i])), _mm256_cvtps_pd(_mm_loadu_ps(&data2[i])), accum1);
                accum2 = _mm256_fmadd_pd(_mm256_cvtps_pd(_mm_loadu_ps(&data1[i + 4])), _mm256_cvtps_pd(_mm_loadu_ps(&data2[i + 4])), accum2);
            }
            for (; i < dataSize - 3; i += 4)
                accum1 = _mm256_fmadd_pd(_mm256_cvtps_pd(_mm_loadu_ps(&data1[i])), _mm256_cvtps_pd(_mm_loadu_ps(&data2[i])), accum1);
            accum1        = _mm256_add_pd(accum1, accum2);
            accum1        = _mm256_add_pd(accum1, _mm256_permute2f128_pd(accum1, accum1, 1));
            accum1        = _mm256_hadd_pd(accum1, accum1);
            double result = _mm256_cvtsd_f64(accum1);
            // the remaining products
            for (; i < dataSize; ++i)
                result += data1[i] * static_cast<double>(data2[i]);
            return result;
        }

    } // namespace

    double InnerProductFlp(const float* data1, const float* data2, std::int32_t dataSize, std::int32_t arch) noexcept
    {
        return arch >= ArchAvx2 ? InnerProductFlpAvx2(data1, data2, dataSize) : InnerProductFlpC(data1, data2, dataSize);
    }

    double EnergyFlp(const float* data, std::int32_t dataSize) noexcept
    {
        // 4x unrolled
        double       result = 0.0;
        std::int32_t i      = 0;
        for (; i < dataSize - 3; i += 4) {
            result += data[i + 0] * static_cast<double>(data[i + 0]) + data[i + 1] * static_cast<double>(data[i + 1])
                + data[i + 2] * static_cast<double>(data[i + 2]) + data[i + 3] * static_cast<double>(data[i + 3]);
        }
        // the remaining products
        for (; i < dataSize; ++i)
            result += data[i] * static_cast<double>(data[i]);
        return result;
    }

    void AutocorrelationFlp(float* results, const float* inputData, std::int32_t inputDataSize, std::int32_t correlationCount, std::int32_t arch) noexcept
    {
        if (correlationCount > inputDataSize)
            correlationCount = inputDataSize;
        for (std::int32_t i = 0; i < correlationCount; ++i)
            results[i] = static_cast<float>(InnerProductFlp(inputData, inputData + i, inputDataSize - i, arch));
    }

    void WarpedAutocorrelationFlp(float* corr, const float* input, float warping, std::int32_t length, std::int32_t order) noexcept
    {
        double state[SilkMaxShapeLpcOrder + 1] = {};
        double c[SilkMaxShapeLpcOrder + 1]     = {};
        // over the samples
        for (std::int32_t n = 0; n < length; ++n) {
            double tmp1 = input[n];
            // over the allpass sections; two multiplies instead of a factored expression for a
            // shorter dependency chain
            for (std::int32_t i = 0; i < order; i += 2) {
                const double tmp2 = state[i] + warping * state[i + 1] - warping * tmp1;
                state[i]          = tmp1;
                c[i] += state[0] * tmp1;
                tmp1         = state[i + 1] + warping * state[i + 2] - warping * tmp2;
                state[i + 1] = tmp2;
                c[i + 1] += state[0] * tmp2;
            }
            state[order] = tmp1;
            c[order] += state[0] * tmp1;
        }
        for (std::int32_t i = 0; i < order + 1; ++i)
            corr[i] = static_cast<float>(c[i]);
    }

    float SchurFlp(float reflCoef[], const float autoCorr[], std::int32_t order) noexcept
    {
        double       c[MaxOrderLpc + 1][2];
        std::int32_t k = 0;
        do {
            c[k][0] = c[k][1] = autoCorr[k];
        } while (++k <= order);

        for (k = 0; k < order; ++k) {
            // the reflection coefficient
            const double rcTmp = -c[k + 1][0] / MaxFloat(c[0][1], 1e-9f);
            reflCoef[k]        = static_cast<float>(rcTmp);
            // the correlations updated
            for (std::int32_t n = 0; n < order - k; ++n) {
                const double ctmp1 = c[n + k + 1][0];
                const double ctmp2 = c[n][1];
                c[n + k + 1][0]    = ctmp1 + ctmp2 * rcTmp;
                c[n][1]            = ctmp2 + ctmp1 * rcTmp;
            }
        }
        // the residual energy
        return static_cast<float>(c[0][1]);
    }

    void K2aFlp(float* a, const float* rc, std::int32_t order) noexcept
    {
        for (std::int32_t k = 0; k < order; ++k) {
            const float rck = rc[k];
            for (std::int32_t n = 0; n < (k + 1) >> 1; ++n) {
                const float tmp1 = a[n];
                const float tmp2 = a[k - n - 1];
                a[n]             = tmp1 + tmp2 * rck;
                a[k - n - 1]     = tmp2 + tmp1 * rck;
            }
            a[k] = -rck;
        }
    }

    void BwexpanderFlp(float* ar, std::int32_t d, float chirp) noexcept
    {
        float cfac = chirp;
        for (std::int32_t i = 0; i < d - 1; ++i) {
            ar[i] *= cfac;
            cfac *= chirp;
        }
        ar[d - 1] *= cfac;
    }

    void ApplySineWindowFlp(float pxWin[], const float px[], std::int32_t winType, std::int32_t length) noexcept
    {
        const float freq = Pi / (length + 1);
        // an approximation of 2 * cos(f)
        const float c = 2.0f - freq * freq;
        // from 0 with sin(f), or from 1 with cos(f)
        float s0 = winType < 2 ? 0.0f : 1.0f;
        float s1 = winType < 2 ? freq : 0.5f * c;
        // sin(n * f) = 2 * cos(f) * sin((n - 1) * f) - sin((n - 2) * f), 4 samples at a time
        for (std::int32_t k = 0; k < length; k += 4) {
            pxWin[k + 0] = px[k + 0] * 0.5f * (s0 + s1);
            pxWin[k + 1] = px[k + 1] * s1;
            s0           = c * s1 - s0;
            pxWin[k + 2] = px[k + 2] * 0.5f * (s1 + s0);
            pxWin[k + 3] = px[k + 3] * s0;
            s1           = c * s0 - s1;
        }
    }

    void InsertionSortDecreasingFlp(float* a, std::int32_t* idx, std::int32_t l, std::int32_t k) noexcept
    {
        // the start indices
        for (std::int32_t i = 0; i < k; ++i)
            idx[i] = i;
        // sorted by value, decreasing
        for (std::int32_t i = 1; i < k; ++i) {
            const float  value = a[i];
            std::int32_t j     = i - 1;
            for (; j >= 0 && value > a[j]; --j) {
                a[j + 1]   = a[j];
                idx[j + 1] = idx[j];
            }
            a[j + 1]   = value;
            idx[j + 1] = i;
        }
        // the remaining values: only the first k positions are kept correct
        for (std::int32_t i = k; i < l; ++i) {
            const float value = a[i];
            if (value > a[k - 1]) {
                std::int32_t j = k - 2;
                for (; j >= 0 && value > a[j]; --j) {
                    a[j + 1]   = a[j];
                    idx[j + 1] = idx[j];
                }
                a[j + 1]   = value;
                idx[j + 1] = i;
            }
        }
    }

    void LpcAnalysisFilterFlp(float rLpc[], const float predCoef[], const float s[], std::int32_t length, std::int32_t order) noexcept
    {
        for (std::int32_t ix = order; ix < length; ++ix) {
            const float* sPtr = &s[ix - 1];
            // the short-term prediction, summed in the order of the unrolled libopus filters
            float lpcPred = sPtr[0] * predCoef[0];
            for (std::int32_t j = 1; j < order; ++j)
                lpcPred += sPtr[-j] * predCoef[j];
            // the prediction error
            rLpc[ix] = sPtr[1] - lpcPred;
        }
        // the first order output samples are zero
        std::memset(rLpc, 0, order * sizeof(float));
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk
