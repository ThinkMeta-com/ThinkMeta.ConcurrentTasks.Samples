// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "MdctKernels.h"

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Lame::MdctKernels {

    namespace {

        // The helpers below are the recurring steps of the filterbank's butterfly network. Each
        // keeps the exact order of LAME's statements; float locals round like LAME's float xr.

        // xr = hi - lo; lo += hi; hi = xr * f
        inline void Rotate(float& lo, float& hi, float f) noexcept
        {
            float const d = hi - lo;
            lo += hi;
            hi = d * f;
        }

        // xr = lo - hi; lo += hi; hi = xr * f
        inline void RotateReversed(float& lo, float& hi, float f) noexcept
        {
            float const d = lo - hi;
            lo += hi;
            hi = d * f;
        }

        // xr = SQRT2 * (lo - hi) (double, rounded); lo += hi; hi = xr
        inline void ScaledDifference(float& lo, float& hi) noexcept
        {
            float const d = static_cast<float>(SQRT2 * (lo - hi));
            lo += hi;
            hi = d;
        }

        // xr = SQRT2 * (hi - lo) (double, rounded); lo += hi; hi = xr - lo
        inline void ScaledDifferenceMinusSum(float& lo, float& hi) noexcept
        {
            float const d = static_cast<float>(SQRT2 * (-lo + hi));
            lo += hi;
            hi = d - lo;
        }

        // xr = -SQRT2 * (lo - hi) (double, rounded); lo += hi; hi = xr - lo
        inline void NegScaledDifferenceMinusSum(float& lo, float& hi) noexcept
        {
            float const d = static_cast<float>(-SQRT2 * (lo - hi));
            lo += hi;
            hi = d - lo;
        }

        // xr = x; x = y - xr; y = y + xr
        inline void Cross(float& x, float& y) noexcept
        {
            float const old = x;
            x               = y - old;
            y               = y + old;
        }

        // xr = lo; lo += hi; hi -= xr
        inline void SumAndDifference(float& lo, float& hi) noexcept
        {
            float const old = lo;
            lo += hi;
            hi -= old;
        }

        // xr = a[first] - a[second]; a[first] = xr; then for each index: xr = a[index] - xr; a[index] = xr
        inline void DifferenceChain(float* a, std::int32_t first, std::int32_t second, std::initializer_list<std::int32_t> chain) noexcept
        {
            float d  = a[first] - a[second];
            a[first] = d;
            for (std::int32_t index : chain) {
                d        = a[index] - d;
                a[index] = d;
            }
        }

    } // namespace

    // LAME: window_subband() (newmdct.c)
    void WindowSubband(const float* enwindow, const sample_t* x1, float* a) noexcept
    {
        float const*    wp = enwindow + 10;
        const sample_t* x2 = &x1[238 - 14 - 286];

        // 15 pairs of subbands: two windowed sums s and t over 16 taps each
        for (std::int32_t i = -15; i < 0; i++) {
            float w = wp[-10];
            float s = x2[-224] * w;
            float t = x1[224] * w;
            for (std::int32_t k = 1; k < 8; ++k) {
                w = wp[-10 + k];
                s += x2[-224 + 64 * k] * w;
                t += x1[224 - 64 * k] * w;
            }
            for (std::int32_t k = 0; k < 8; ++k) {
                w = wp[-2 + k];
                s += x1[-256 + 64 * k] * w;
                t -= x2[256 - 64 * k] * w;
            }

            s *= wp[6];
            w             = t - s;
            a[30 + i * 2] = t + s;
            a[31 + i * 2] = wp[7] * w;
            wp += 18;
            x1--;
            x2++;
        }

        WindowSubbandTail(enwindow, x1 + 15, a);
    }

    WindowSubbandFunction WindowSubbandFor(SimdLevel level) noexcept
    {
        switch (level) {
            case SimdLevel::Sse41: return WindowSubbandSse41;
            case SimdLevel::Avx2: return WindowSubbandAvx2;
            default: return WindowSubband;
        }
    }

    void WindowSubbandTail(const float* enwindow, const sample_t* x1, float* a) noexcept
    {
        // where the loop over the 15 pairs leaves the window and the input
        float const* const wp = enwindow + 10 + 15 * 18;
        x1 -= 15;

        // the middle subbands 14, 15, 30 and 31
        {
            float t = x1[-16] * wp[-10];
            for (std::int32_t k = 1; k < 8; ++k) {
                if (k % 2 != 0)
                    t += (x1[-16 - 32 * k] - x1[-16 + 32 * k]) * wp[-10 + k];
                else
                    t += (x1[-16 - 32 * k] + x1[-16 + 32 * k]) * wp[-10 + k];
            }

            float s = x1[-32] * wp[-2];
            for (std::int32_t k = 1; k < 4; ++k)
                s += x1[-32 - 64 * k] * wp[-2 + k];
            for (std::int32_t k = 0; k < 3; ++k)
                s -= x1[32 + 64 * k] * wp[2 + k];
            s -= x1[224];

            float const u = s - t;
            float const v = s + t;

            t = a[14];
            s = a[15] - t;

            a[31] = v + t; // A0
            a[30] = u + s; // A1
            a[15] = u - s; // A2
            a[14] = v - t; // A3
        }

        // the butterfly network of the 32 point DCT
        float const f2  = wp[-2 * 18 + 7];
        float const f4  = wp[-4 * 18 + 7];
        float const f6  = wp[-6 * 18 + 7];
        float const f10 = wp[-10 * 18 + 7];
        float const f12 = wp[-12 * 18 + 7];
        float const f14 = wp[-14 * 18 + 7];

        Rotate(a[0], a[28], f2);
        Rotate(a[1], a[29], f2);
        Rotate(a[2], a[26], f4);
        Rotate(a[3], a[27], f4);
        Rotate(a[4], a[24], f6);
        Rotate(a[5], a[25], f6);

        {
            float d = a[22] - a[6];
            a[6] += a[22];
            a[22] = static_cast<float>(d * SQRT2);
            d     = a[23] - a[7];
            a[7] += a[23];
            a[23] = static_cast<float>(d * SQRT2 - a[7]); // double subtraction, rounded once
            a[7] -= a[6];
            a[22] -= a[7];
            a[23] -= a[22];
        }
        Cross(a[6], a[31]);
        Cross(a[7], a[30]);
        Cross(a[22], a[15]);
        Cross(a[23], a[14]);

        Rotate(a[8], a[20], f10);
        Rotate(a[9], a[21], f10);
        Rotate(a[10], a[18], f12);
        Rotate(a[11], a[19], f12);
        Rotate(a[12], a[16], f14);
        Rotate(a[13], a[17], f14);

        Rotate(a[20], a[24], f12);
        Rotate(a[21], a[25], f12);
        RotateReversed(a[4], a[8], f12);
        RotateReversed(a[5], a[9], f12);
        RotateReversed(a[0], a[12], f4);
        RotateReversed(a[1], a[13], f4);
        RotateReversed(a[16], a[28], f4);
        Rotate(a[17], a[29], f4);

        ScaledDifference(a[2], a[10]);
        ScaledDifference(a[3], a[11]);
        ScaledDifferenceMinusSum(a[18], a[26]);
        ScaledDifferenceMinusSum(a[19], a[27]);

        {
            float d = a[2];
            a[19] -= a[3];
            a[3] -= d;
            a[2] = a[31] - d;
            a[31] += d;
            d = a[3];
            a[11] -= a[19];
            a[18] -= d;
            a[3] = a[30] - d;
            a[30] += d;
            d = a[18];
            a[27] -= a[11];
            a[19] -= d;
            a[18] = a[15] - d;
            a[15] += d;

            d = a[19];
            a[10] -= d;
            a[19] = a[14] - d;
            a[14] += d;
            d = a[10];
            a[11] -= d;
            a[10] = a[23] - d;
            a[23] += d;
            d = a[11];
            a[26] -= d;
            a[11] = a[22] - d;
            a[22] += d;
            d = a[26];
            a[27] -= d;
            a[26] = a[7] - d;
            a[7] += d;

            d     = a[27];
            a[27] = a[6] - d;
            a[6] += d;
        }

        ScaledDifference(a[0], a[4]);
        ScaledDifference(a[1], a[5]);
        ScaledDifference(a[16], a[20]);
        ScaledDifference(a[17], a[21]);

        NegScaledDifferenceMinusSum(a[8], a[12]);
        NegScaledDifferenceMinusSum(a[9], a[13]);
        NegScaledDifferenceMinusSum(a[25], a[29]);
        {
            // the only step with a sum inside: xr = -SQRT2 * (a24 + a28); a24 -= a28; a28 = xr - a24
            float const d = static_cast<float>(-SQRT2 * (a[24] + a[28]));
            a[24] -= a[28];
            a[28] = d - a[24];
        }

        DifferenceChain(a, 24, 16, {20, 28});
        DifferenceChain(a, 25, 17, {21, 29});
        DifferenceChain(a, 17, 1, {9, 25, 5, 21, 13, 29});
        DifferenceChain(a, 1, 0, {16, 17, 8, 9, 24, 25, 4, 5, 20, 21, 12, 13, 28, 29});

        SumAndDifference(a[0], a[31]);
        SumAndDifference(a[1], a[30]);
        SumAndDifference(a[16], a[15]);
        SumAndDifference(a[17], a[14]);
        SumAndDifference(a[8], a[23]);
        SumAndDifference(a[9], a[22]);
        SumAndDifference(a[24], a[7]);
        SumAndDifference(a[25], a[6]);
        SumAndDifference(a[4], a[27]);
        SumAndDifference(a[5], a[26]);
        SumAndDifference(a[20], a[11]);
        SumAndDifference(a[21], a[10]);
        SumAndDifference(a[12], a[19]);
        SumAndDifference(a[13], a[18]);
        SumAndDifference(a[28], a[3]);
        SumAndDifference(a[29], a[2]);
    }

    // LAME: mdct_short() (newmdct.c)
    void MdctShort(const float* shortWindow, float* inout) noexcept
    {
        // the scale factors of the three-point transforms (LAME's tritab_s); note the two
        // slightly different values of the last one, as in LAME
        constexpr double TriTab1  = 2.069978111953089e-11;
        constexpr double TriTab2a = 1.907525191737280e-11;
        constexpr double TriTab2b = 1.907525191737281e-11;
        constexpr double Cos30    = 0.86602540378443870761;

        float const w0 = shortWindow[0];
        float const w1 = shortWindow[1];
        float const w2 = shortWindow[2];

        for (std::int32_t l = 0; l < 3; l++, inout++) {
            float ts0 = inout[2 * 3] * w0 - inout[5 * 3];
            float tc0 = inout[0 * 3] * w2 - inout[3 * 3];
            float tc1 = ts0 + tc0;
            float tc2 = ts0 - tc0;

            ts0       = inout[5 * 3] * w0 + inout[2 * 3];
            tc0       = inout[3 * 3] * w2 + inout[0 * 3];
            float ts1 = ts0 + tc0;
            float ts2 = -ts0 + tc0;

            tc0 = static_cast<float>((inout[1 * 3] * w1 - inout[4 * 3]) * TriTab1);
            ts0 = static_cast<float>((inout[4 * 3] * w1 + inout[1 * 3]) * TriTab1);

            inout[3 * 0] = static_cast<float>(tc1 * TriTab2a + tc0);
            inout[3 * 5] = static_cast<float>(-ts1 * TriTab2a + ts0);

            tc2          = static_cast<float>(tc2 * Cos30 * TriTab2b);
            ts1          = static_cast<float>(ts1 * 0.5 * TriTab2b + ts0);
            inout[3 * 1] = tc2 - ts1;
            inout[3 * 2] = tc2 + ts1;

            tc1          = static_cast<float>(tc1 * 0.5 * TriTab2b - tc0);
            ts2          = static_cast<float>(ts2 * Cos30 * TriTab2b);
            inout[3 * 3] = tc1 + ts2;
            inout[3 * 4] = tc1 - ts2;
        }
    }

    // LAME: mdct_long() (newmdct.c)
    void MdctLong(const float* cx, float* out, const float* in) noexcept
    {
        // odd output lines from the differences, first half
        {
            float const tc1 = in[17] - in[9];
            float const tc3 = in[15] - in[11];
            float const tc4 = in[14] - in[12];
            float const ts5 = in[0] + in[8];
            float       ts6 = in[1] + in[7];
            float const ts7 = in[2] + in[6];
            float const ts8 = in[3] + in[5];

            out[17]  = (ts5 + ts7 - ts8) - (ts6 - in[4]);
            float st = (ts5 + ts7 - ts8) * cx[7] + (ts6 - in[4]);
            float ct = (tc1 - tc3 - tc4) * cx[6];
            out[5]   = ct + st;
            out[6]   = ct - st;

            float const tc2 = (in[16] - in[10]) * cx[6];
            ts6             = ts6 * cx[7] + in[4];
            ct              = tc1 * cx[0] + tc2 + tc3 * cx[1] + tc4 * cx[2];
            st              = -ts5 * cx[4] + ts6 - ts7 * cx[5] + ts8 * cx[3];
            out[1]          = ct + st;
            out[2]          = ct - st;

            ct      = tc1 * cx[1] - tc2 - tc3 * cx[2] + tc4 * cx[0];
            st      = -ts5 * cx[5] + ts6 - ts7 * cx[3] + ts8 * cx[4];
            out[9]  = ct + st;
            out[10] = ct - st;

            ct      = tc1 * cx[2] - tc2 + tc3 * cx[0] - tc4 * cx[1];
            st      = ts5 * cx[3] - ts6 + ts7 * cx[4] - ts8 * cx[5];
            out[13] = ct + st;
            out[14] = ct - st;
        }
        // and the second half
        {
            float const ts1 = in[8] - in[0];
            float const ts3 = in[6] - in[2];
            float const ts4 = in[5] - in[3];
            float const tc5 = in[17] + in[9];
            float       tc6 = in[16] + in[10];
            float const tc7 = in[15] + in[11];
            float const tc8 = in[14] + in[12];

            out[0]   = (tc5 + tc7 + tc8) + (tc6 + in[13]);
            float ct = (tc5 + tc7 + tc8) * cx[7] - (tc6 + in[13]);
            float st = (ts1 - ts3 + ts4) * cx[6];
            out[11]  = ct + st;
            out[12]  = ct - st;

            float const ts2 = (in[7] - in[1]) * cx[6];
            tc6             = in[13] - tc6 * cx[7];
            ct              = tc5 * cx[3] - tc6 + tc7 * cx[4] + tc8 * cx[5];
            st              = ts1 * cx[2] + ts2 + ts3 * cx[0] + ts4 * cx[1];
            out[3]          = ct + st;
            out[4]          = ct - st;

            ct     = -tc5 * cx[5] + tc6 - tc7 * cx[3] - tc8 * cx[4];
            st     = ts1 * cx[1] + ts2 - ts3 * cx[2] - ts4 * cx[0];
            out[7] = ct + st;
            out[8] = ct - st;

            ct      = -tc5 * cx[4] + tc6 - tc7 * cx[5] - tc8 * cx[3];
            st      = ts1 * cx[0] - ts2 + ts3 * cx[1] - ts4 * cx[2];
            out[15] = ct + st;
            out[16] = ct - st;
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame::MdctKernels
