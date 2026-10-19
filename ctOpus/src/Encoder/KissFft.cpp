// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "KissFft.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    namespace {

#include "KissFftTables.inc"

        constexpr FftState FftStates[4] = {
            {480, 0.0020833334f, -1, {5, 96, 3, 32, 4, 8, 2, 4, 4, 1}, FftBitrev480, FftTwiddles480},
            {240, 0.0041666669f, 1, {5, 48, 3, 16, 4, 4, 4, 1}, FftBitrev240, FftTwiddles480},
            {120, 0.0083333338f, 2, {5, 24, 3, 8, 2, 4, 4, 1}, FftBitrev120, FftTwiddles480},
            {60, 0.016666668f, 3, {5, 12, 3, 4, 4, 1}, FftBitrev60, FftTwiddles480},
        };

        constexpr std::int32_t MaxFactors = 8;

        // the complex helpers of _kiss_fft_guts.h (float build)
        inline FftComplex Mul(const FftComplex& a, const FftComplex& b) noexcept
        {
            return {a.r * b.r - a.i * b.i, a.r * b.i + a.i * b.r};
        }
        inline FftComplex Add(const FftComplex& a, const FftComplex& b) noexcept
        {
            return {a.r + b.r, a.i + b.i};
        }
        inline FftComplex Sub(const FftComplex& a, const FftComplex& b) noexcept
        {
            return {a.r - b.r, a.i - b.i};
        }
        inline void AddTo(FftComplex& res, const FftComplex& a) noexcept
        {
            res.r += a.r;
            res.i += a.i;
        }

        // libopus: kf_bfly2() (celt/kiss_fft.c)
        // radix 2, always after a radix 4 (m == 4) in the static modes
        void Butterfly2(FftComplex* fout, std::int32_t n) noexcept
        {
            constexpr float tw = 0.7071067812f;
            for (std::int32_t i = 0; i < n; ++i) {
                FftComplex* fout2 = fout + 4;
                FftComplex  t     = fout2[0];
                fout2[0]          = Sub(fout[0], t);
                AddTo(fout[0], t);

                t.r      = (fout2[1].r + fout2[1].i) * tw;
                t.i      = (fout2[1].i - fout2[1].r) * tw;
                fout2[1] = Sub(fout[1], t);
                AddTo(fout[1], t);

                t.r      = fout2[2].i;
                t.i      = -fout2[2].r;
                fout2[2] = Sub(fout[2], t);
                AddTo(fout[2], t);

                t.r      = (fout2[3].i - fout2[3].r) * tw;
                t.i      = -(fout2[3].i + fout2[3].r) * tw;
                fout2[3] = Sub(fout[3], t);
                AddTo(fout[3], t);
                fout += 8;
            }
        }

        // libopus: kf_bfly4() (celt/kiss_fft.c)
        void Butterfly4(FftComplex* fout, std::int32_t fstride, const FftState& st, std::int32_t m, std::int32_t n, std::int32_t mm) noexcept
        {
            if (m == 1) {
                // all twiddles are 1
                for (std::int32_t i = 0; i < n; ++i) {
                    const FftComplex scratch0 = Sub(fout[0], fout[2]);
                    AddTo(fout[0], fout[2]);
                    FftComplex scratch1 = Add(fout[1], fout[3]);
                    fout[2]             = Sub(fout[0], scratch1);
                    AddTo(fout[0], scratch1);
                    scratch1  = Sub(fout[1], fout[3]);
                    fout[1].r = scratch0.r + scratch1.i;
                    fout[1].i = scratch0.i - scratch1.r;
                    fout[3].r = scratch0.r - scratch1.i;
                    fout[3].i = scratch0.i + scratch1.r;
                    fout += 4;
                }
                return;
            }
            const std::int32_t m2       = 2 * m;
            const std::int32_t m3       = 3 * m;
            FftComplex* const  foutBase = fout;
            for (std::int32_t i = 0; i < n; ++i) {
                fout                  = foutBase + i * mm;
                const FftComplex* tw1 = st.twiddles;
                const FftComplex* tw2 = st.twiddles;
                const FftComplex* tw3 = st.twiddles;
                for (std::int32_t j = 0; j < m; ++j) {
                    const FftComplex s0 = Mul(fout[m], *tw1);
                    const FftComplex s1 = Mul(fout[m2], *tw2);
                    const FftComplex s2 = Mul(fout[m3], *tw3);
                    const FftComplex s5 = Sub(fout[0], s1);
                    AddTo(fout[0], s1);
                    const FftComplex s3 = Add(s0, s2);
                    const FftComplex s4 = Sub(s0, s2);
                    fout[m2]            = Sub(fout[0], s3);
                    tw1 += fstride;
                    tw2 += fstride * 2;
                    tw3 += fstride * 3;
                    AddTo(fout[0], s3);
                    fout[m].r  = s5.r + s4.i;
                    fout[m].i  = s5.i - s4.r;
                    fout[m3].r = s5.r - s4.i;
                    fout[m3].i = s5.i + s4.r;
                    ++fout;
                }
            }
        }

        // libopus: kf_bfly3() (celt/kiss_fft.c)
        void Butterfly3(FftComplex* fout, std::int32_t fstride, const FftState& st, std::int32_t m, std::int32_t n, std::int32_t mm) noexcept
        {
            const std::int32_t m2       = 2 * m;
            const FftComplex   epi3     = st.twiddles[fstride * m];
            FftComplex* const  foutBase = fout;
            for (std::int32_t i = 0; i < n; ++i) {
                fout                  = foutBase + i * mm;
                const FftComplex* tw1 = st.twiddles;
                const FftComplex* tw2 = st.twiddles;
                for (std::int32_t k = m; k > 0; --k) {
                    const FftComplex s1 = Mul(fout[m], *tw1);
                    const FftComplex s2 = Mul(fout[m2], *tw2);
                    const FftComplex s3 = Add(s1, s2);
                    FftComplex       s0 = Sub(s1, s2);
                    tw1 += fstride;
                    tw2 += fstride * 2;
                    fout[m].r = fout[0].r - s3.r * .5f;
                    fout[m].i = fout[0].i - s3.i * .5f;
                    s0.r *= epi3.i;
                    s0.i *= epi3.i;
                    AddTo(fout[0], s3);
                    fout[m2].r = fout[m].r + s0.i;
                    fout[m2].i = fout[m].i - s0.r;
                    fout[m].r  = fout[m].r - s0.i;
                    fout[m].i  = fout[m].i + s0.r;
                    ++fout;
                }
            }
        }

        // libopus: kf_bfly5() (celt/kiss_fft.c)
        void Butterfly5(FftComplex* fout, std::int32_t fstride, const FftState& st, std::int32_t m, std::int32_t n, std::int32_t mm) noexcept
        {
            const FftComplex  ya       = st.twiddles[fstride * m];
            const FftComplex  yb       = st.twiddles[fstride * 2 * m];
            const FftComplex* tw       = st.twiddles;
            FftComplex* const foutBase = fout;
            for (std::int32_t i = 0; i < n; ++i) {
                fout              = foutBase + i * mm;
                FftComplex* fout0 = fout;
                FftComplex* fout1 = fout0 + m;
                FftComplex* fout2 = fout0 + 2 * m;
                FftComplex* fout3 = fout0 + 3 * m;
                FftComplex* fout4 = fout0 + 4 * m;
                for (std::int32_t u = 0; u < m; ++u) {
                    const FftComplex s0  = *fout0;
                    const FftComplex s1  = Mul(*fout1, tw[u * fstride]);
                    const FftComplex s2  = Mul(*fout2, tw[2 * u * fstride]);
                    const FftComplex s3  = Mul(*fout3, tw[3 * u * fstride]);
                    const FftComplex s4  = Mul(*fout4, tw[4 * u * fstride]);
                    const FftComplex s7  = Add(s1, s4);
                    const FftComplex s10 = Sub(s1, s4);
                    const FftComplex s8  = Add(s2, s3);
                    const FftComplex s9  = Sub(s2, s3);

                    fout0->r = fout0->r + (s7.r + s8.r);
                    fout0->i = fout0->i + (s7.i + s8.i);

                    FftComplex s5;
                    s5.r = s0.r + (s7.r * ya.r + s8.r * yb.r);
                    s5.i = s0.i + (s7.i * ya.r + s8.i * yb.r);
                    FftComplex s6;
                    s6.r   = s10.i * ya.i + s9.i * yb.i;
                    s6.i   = -(s10.r * ya.i + s9.r * yb.i);
                    *fout1 = Sub(s5, s6);
                    *fout4 = Add(s5, s6);

                    FftComplex s11;
                    s11.r = s0.r + (s7.r * yb.r + s8.r * ya.r);
                    s11.i = s0.i + (s7.i * yb.r + s8.i * ya.r);
                    FftComplex s12;
                    s12.r  = s9.i * ya.i - s10.i * yb.i;
                    s12.i  = s10.r * yb.i - s9.r * ya.i;
                    *fout2 = Add(s11, s12);
                    *fout3 = Sub(s11, s12);
                    ++fout0;
                    ++fout1;
                    ++fout2;
                    ++fout3;
                    ++fout4;
                }
            }
        }

    } // namespace

    const FftState& Fft48000(std::int32_t shift) noexcept
    {
        return FftStates[shift];
    }

    // libopus: opus_fft_impl() (celt/kiss_fft.c)
    void FftImpl(const FftState& st, FftComplex* fout) noexcept
    {
        // st.shift can be -1
        const std::int32_t shift = st.shift > 0 ? st.shift : 0;

        std::int32_t fstride[MaxFactors];
        fstride[0]     = 1;
        std::int32_t l = 0;
        std::int32_t m = 0;
        do {
            const std::int32_t p = st.factors[2 * l];
            m                    = st.factors[2 * l + 1];
            fstride[l + 1]       = fstride[l] * p;
            ++l;
        } while (m != 1);
        m = st.factors[2 * l - 1];
        for (std::int32_t i = l - 1; i >= 0; --i) {
            const std::int32_t m2 = i != 0 ? st.factors[2 * i - 1] : 1;
            switch (st.factors[2 * i]) {
                case 2: Butterfly2(fout, fstride[i]); break;
                case 4: Butterfly4(fout, fstride[i] << shift, st, m, fstride[i], m2); break;
                case 3: Butterfly3(fout, fstride[i] << shift, st, m, fstride[i], m2); break;
                case 5: Butterfly5(fout, fstride[i] << shift, st, m, fstride[i], m2); break;
                default: break;
            }
            m = m2;
        }
    }

    // libopus: opus_fft_c() (celt/kiss_fft.c)
    void Fft(const FftState& st, const FftComplex* fin, FftComplex* fout) noexcept
    {
        // bit-reverse the input
        for (std::int32_t i = 0; i < st.nfft; ++i) {
            const FftComplex x   = fin[i];
            fout[st.bitrev[i]].r = x.r * st.scale;
            fout[st.bitrev[i]].i = x.i * st.scale;
        }
        FftImpl(st, fout);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
