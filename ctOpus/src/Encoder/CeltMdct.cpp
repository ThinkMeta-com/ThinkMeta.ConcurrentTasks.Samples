// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "CeltMdct.h"

#include "CeltMode.h"
#include "KissFft.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    // libopus: clt_mdct_forward_c() (celt/mdct.c)
    void MdctForward(const float* in, float* out, std::int32_t shift, std::int32_t stride) noexcept
    {
        constexpr std::int32_t overlap = CeltMode::Overlap;
        const float* const     window  = CeltMode::Window;
        const FftState&        st      = Fft48000(shift);
        const float            scale   = st.scale;

        std::int32_t n    = CeltMode::MdctSize;
        const float* trig = CeltMode::MdctTrig;
        for (std::int32_t i = 0; i < shift; ++i) {
            n >>= 1;
            trig += n;
        }
        const std::int32_t n2 = n >> 1;
        const std::int32_t n4 = n >> 2;

        float      f[CeltMode::MdctSize / 2];
        FftComplex f2[CeltMode::MdctSize / 4];

        // the input as four blocks [a, b, c, d]: window, shuffle, fold
        {
            const float* xp1 = in + (overlap >> 1);
            const float* xp2 = in + n2 - 1 + (overlap >> 1);
            float*       yp  = f;
            const float* wp1 = window + (overlap >> 1);
            const float* wp2 = window + (overlap >> 1) - 1;
            std::int32_t i   = 0;
            for (; i < ((overlap + 3) >> 2); ++i) {
                // real part arranged as -d-cR, imaginary part as -b+aR
                *yp++ = xp1[n2] * *wp2 + *xp2 * *wp1;
                *yp++ = *xp1 * *wp1 - xp2[-n2] * *wp2;
                xp1 += 2;
                xp2 -= 2;
                wp1 += 2;
                wp2 -= 2;
            }
            wp1 = window;
            wp2 = window + overlap - 1;
            for (; i < n4 - ((overlap + 3) >> 2); ++i) {
                // real part arranged as a-bR, imaginary part as -c-dR
                *yp++ = *xp2;
                *yp++ = *xp1;
                xp1 += 2;
                xp2 -= 2;
            }
            for (; i < n4; ++i) {
                // real part arranged as a-bR, imaginary part as -c-dR
                *yp++ = -(xp1[-n2] * *wp1) + *xp2 * *wp2;
                *yp++ = *xp1 * *wp2 + xp2[n2] * *wp1;
                xp1 += 2;
                xp2 -= 2;
                wp1 += 2;
                wp2 -= 2;
            }
        }
        // pre-rotation (scaled before the FFT, which no longer scales)
        {
            const float* yp = f;
            const float* t  = trig;
            for (std::int32_t i = 0; i < n4; ++i) {
                const float t0 = t[i];
                const float t1 = t[n4 + i];
                const float re = *yp++;
                const float im = *yp++;
                const float yr = re * t0 - im * t1;
                const float yi = im * t0 + re * t1;
                FftComplex  yc;
                yc.r             = yr * scale;
                yc.i             = yi * scale;
                f2[st.bitrev[i]] = yc;
            }
        }

        // N/4 complex FFT
        FftImpl(st, f2);

        // post-rotation
        {
            const FftComplex* fp  = f2;
            float*            yp1 = out;
            float*            yp2 = out + stride * (n2 - 1);
            const float*      t   = trig;
            for (std::int32_t i = 0; i < n4; ++i) {
                const float t0 = t[i];
                const float t1 = t[n4 + i];
                const float yr = fp->i * t1 - fp->r * t0;
                const float yi = fp->r * t1 + fp->i * t0;
                *yp1           = yr;
                *yp2           = yi;
                ++fp;
                yp1 += 2 * stride;
                yp2 -= 2 * stride;
            }
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
