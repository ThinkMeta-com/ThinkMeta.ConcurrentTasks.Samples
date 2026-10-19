// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <xmmintrin.h>

#include <cstdint>

// The SSE kernels that libopus 1.6.1 uses unconditionally on x64 (OPUS_X86_PRESUME_SSE,
// celt/x86/pitch_sse.c): their summation order differs from the C versions, so the port uses
// exactly these.

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    /// <summary>celt_inner_prod_sse.</summary>
    inline float InnerProd(const float* x, const float* y, std::int32_t n) noexcept
    {
        __m128       sum = _mm_setzero_ps();
        std::int32_t i   = 0;
        for (; i < n - 3; i += 4)
            sum = _mm_add_ps(sum, _mm_mul_ps(_mm_loadu_ps(x + i), _mm_loadu_ps(y + i)));
        // horizontal sum
        sum      = _mm_add_ps(sum, _mm_movehl_ps(sum, sum));
        sum      = _mm_add_ss(sum, _mm_shuffle_ps(sum, sum, 0x55));
        float xy = 0;
        _mm_store_ss(&xy, sum);
        for (; i < n; ++i)
            xy = xy + x[i] * y[i];
        return xy;
    }

    /// <summary>dual_inner_prod_sse: x·y01 and x·y02.</summary>
    inline void DualInnerProd(const float* x, const float* y01, const float* y02, std::int32_t n, float* xy1, float* xy2) noexcept
    {
        __m128       xsum1 = _mm_setzero_ps();
        __m128       xsum2 = _mm_setzero_ps();
        std::int32_t i     = 0;
        for (; i < n - 3; i += 4) {
            const __m128 xi = _mm_loadu_ps(x + i);
            xsum1           = _mm_add_ps(xsum1, _mm_mul_ps(xi, _mm_loadu_ps(y01 + i)));
            xsum2           = _mm_add_ps(xsum2, _mm_mul_ps(xi, _mm_loadu_ps(y02 + i)));
        }
        // horizontal sums
        xsum1 = _mm_add_ps(xsum1, _mm_movehl_ps(xsum1, xsum1));
        xsum1 = _mm_add_ss(xsum1, _mm_shuffle_ps(xsum1, xsum1, 0x55));
        _mm_store_ss(xy1, xsum1);
        xsum2 = _mm_add_ps(xsum2, _mm_movehl_ps(xsum2, xsum2));
        xsum2 = _mm_add_ss(xsum2, _mm_shuffle_ps(xsum2, xsum2, 0x55));
        _mm_store_ss(xy2, xsum2);
        for (; i < n; ++i) {
            *xy1 = *xy1 + x[i] * y01[i];
            *xy2 = *xy2 + x[i] * y02[i];
        }
    }

    /// <summary>xcorr_kernel_sse: sum[k] += x·y[k..] for k = 0..3.</summary>
    inline void XcorrKernel(const float* x, const float* y, float sum[4], std::int32_t len) noexcept
    {
        __m128       xsum1 = _mm_loadu_ps(sum);
        __m128       xsum2 = _mm_setzero_ps();
        std::int32_t j     = 0;
        for (; j < len - 3; j += 4) {
            const __m128 x0 = _mm_loadu_ps(x + j);
            const __m128 yj = _mm_loadu_ps(y + j);
            const __m128 y3 = _mm_loadu_ps(y + j + 3);
            xsum1           = _mm_add_ps(xsum1, _mm_mul_ps(_mm_shuffle_ps(x0, x0, 0x00), yj));
            xsum2           = _mm_add_ps(xsum2, _mm_mul_ps(_mm_shuffle_ps(x0, x0, 0x55), _mm_shuffle_ps(yj, y3, 0x49)));
            xsum1           = _mm_add_ps(xsum1, _mm_mul_ps(_mm_shuffle_ps(x0, x0, 0xaa), _mm_shuffle_ps(yj, y3, 0x9e)));
            xsum2           = _mm_add_ps(xsum2, _mm_mul_ps(_mm_shuffle_ps(x0, x0, 0xff), y3));
        }
        if (j < len) {
            xsum1 = _mm_add_ps(xsum1, _mm_mul_ps(_mm_load1_ps(x + j), _mm_loadu_ps(y + j)));
            if (++j < len) {
                xsum2 = _mm_add_ps(xsum2, _mm_mul_ps(_mm_load1_ps(x + j), _mm_loadu_ps(y + j)));
                if (++j < len)
                    xsum1 = _mm_add_ps(xsum1, _mm_mul_ps(_mm_load1_ps(x + j), _mm_loadu_ps(y + j)));
            }
        }
        _mm_storeu_ps(sum, _mm_add_ps(xsum1, xsum2));
    }

    /// <summary>comb_filter_const_sse: n a multiple of 4 (always so in the static modes).</summary>
    inline void CombFilterConst(float* y, const float* x, std::int32_t t, std::int32_t n, float g10, float g11, float g12) noexcept
    {
        const __m128 g10v = _mm_load1_ps(&g10);
        const __m128 g11v = _mm_load1_ps(&g11);
        const __m128 g12v = _mm_load1_ps(&g12);
        __m128       x0v  = _mm_loadu_ps(&x[-t - 2]);
        for (std::int32_t i = 0; i < n - 3; i += 4) {
            const float* xp  = &x[i - t - 2];
            __m128       yi  = _mm_loadu_ps(x + i);
            const __m128 x4v = _mm_loadu_ps(xp + 4);
            const __m128 x2v = _mm_shuffle_ps(x0v, x4v, 0x4e);
            const __m128 x1v = _mm_shuffle_ps(x0v, x2v, 0x99);
            const __m128 x3v = _mm_shuffle_ps(x2v, x4v, 0x99);
            yi               = _mm_add_ps(yi, _mm_mul_ps(g10v, x2v));
            // partial sums (not bit-exact with the C version)
            const __m128 yi2 = _mm_add_ps(_mm_mul_ps(g11v, _mm_add_ps(x3v, x1v)), _mm_mul_ps(g12v, _mm_add_ps(x4v, x0v)));
            yi               = _mm_add_ps(yi, yi2);
            x0v              = x4v;
            _mm_storeu_ps(y + i, yi);
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
