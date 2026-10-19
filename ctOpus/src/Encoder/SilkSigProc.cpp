// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "SilkSigProc.h"

#include <cstring>

#include "SilkDefines.h"
#include "SilkMacros.h"
#include "SilkTables.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk {

    std::int32_t Lin2Log(std::int32_t inLin) noexcept
    {
        std::int32_t lz     = 0;
        std::int32_t fracQ7 = 0;
        ClzFrac(inLin, &lz, &fracQ7);
        // piece-wise parabolic approximation
        return AddLshift32(Smlawb(fracQ7, fracQ7 * (128 - fracQ7), 179), 31 - lz, 7);
    }

    std::int32_t Log2Lin(std::int32_t inLogQ7) noexcept
    {
        if (inLogQ7 < 0)
            return 0;
        if (inLogQ7 >= 3967)
            return Int32Max;
        std::int32_t       out    = Lshift(1, inLogQ7 >> 7);
        const std::int32_t fracQ7 = inLogQ7 & 0x7F;
        // piece-wise parabolic approximation
        if (inLogQ7 < 2048)
            out = AddRshift32(out, out * Smlawb(fracQ7, Smulbb(fracQ7, 128 - fracQ7), -174), 7);
        else
            out = Mla(out, out >> 7, Smlawb(fracQ7, Smulbb(fracQ7, 128 - fracQ7), -174));
        return out;
    }

    void SumSqrShift(std::int32_t* energy, std::int32_t* shift, const std::int16_t* x, std::int32_t len) noexcept
    {
        // a first run with the maximum shift possible, conservatively starting with nrg = len
        std::int32_t shft = 31 - Clz32(len);
        std::int32_t nrg  = len;
        std::int32_t i    = 0;
        for (i = 0; i < len - 1; i += 2) {
            std::uint32_t nrgTmp = static_cast<std::uint32_t>(Smulbb(x[i], x[i]));
            nrgTmp               = static_cast<std::uint32_t>(SmlabbOvflw(static_cast<std::int32_t>(nrgTmp), x[i + 1], x[i + 1]));
            nrg                  = static_cast<std::int32_t>(static_cast<std::uint32_t>(nrg) + (nrgTmp >> shft));
        }
        if (i < len) {
            // one sample left
            const auto nrgTmp = static_cast<std::uint32_t>(Smulbb(x[i], x[i]));
            nrg               = static_cast<std::int32_t>(static_cast<std::uint32_t>(nrg) + (nrgTmp >> shft));
        }
        // the result fits in a signed 32-bit integer with two bits of headroom
        shft = Max32(0, shft + 3 - Clz32(nrg));
        nrg  = 0;
        for (i = 0; i < len - 1; i += 2) {
            std::uint32_t nrgTmp = static_cast<std::uint32_t>(Smulbb(x[i], x[i]));
            nrgTmp               = static_cast<std::uint32_t>(SmlabbOvflw(static_cast<std::int32_t>(nrgTmp), x[i + 1], x[i + 1]));
            nrg                  = static_cast<std::int32_t>(static_cast<std::uint32_t>(nrg) + (nrgTmp >> shft));
        }
        if (i < len) {
            const auto nrgTmp = static_cast<std::uint32_t>(Smulbb(x[i], x[i]));
            nrg               = static_cast<std::int32_t>(static_cast<std::uint32_t>(nrg) + (nrgTmp >> shft));
        }
        *shift  = shft;
        *energy = nrg;
    }

    std::int32_t InnerProdAlignedScale(const std::int16_t* inVec1, const std::int16_t* inVec2, std::int32_t scale, std::int32_t len) noexcept
    {
        std::int32_t sum = 0;
        for (std::int32_t i = 0; i < len; ++i)
            sum = AddRshift32(sum, Smulbb(inVec1[i], inVec2[i]), scale);
        return sum;
    }

    std::int32_t SigmQ15(std::int32_t inQ5) noexcept
    {
        static constexpr std::int32_t slopeQ10[6] = {237, 153, 73, 30, 12, 7};
        static constexpr std::int32_t posQ15[6]   = {16384, 23955, 28861, 31213, 32178, 32548};
        static constexpr std::int32_t negQ15[6]   = {16384, 8812, 3906, 1554, 589, 219};
        if (inQ5 < 0) {
            inQ5 = -inQ5;
            if (inQ5 >= 6 * 32)
                return 0; // clip
            // linear interpolation of the table
            const std::int32_t ind = inQ5 >> 5;
            return negQ15[ind] - Smulbb(slopeQ10[ind], inQ5 & 0x1F);
        }
        if (inQ5 >= 6 * 32)
            return 32767; // clip
        const std::int32_t ind = inQ5 >> 5;
        return posQ15[ind] + Smulbb(slopeQ10[ind], inQ5 & 0x1F);
    }

    void AnaFiltBank1(const std::int16_t* in, std::int32_t* s, std::int16_t* outL, std::int16_t* outH, std::int32_t n) noexcept
    {
        // the coefficients of the first-order allpass filters
        constexpr std::int16_t aFb1_20 = 5394 << 1;
        constexpr std::int16_t aFb1_21 = -24290; // (int16)(20623 << 1)
        const std::int32_t     n2      = n >> 1;
        // the variables and the state are in Q10
        for (std::int32_t k = 0; k < n2; ++k) {
            // the all-pass section for the even input sample
            std::int32_t       in32 = Lshift(static_cast<std::int32_t>(in[2 * k]), 10);
            std::int32_t       y    = in32 - s[0];
            std::int32_t       x    = Smlawb(y, y, aFb1_21);
            const std::int32_t out1 = s[0] + x;
            s[0]                    = in32 + x;
            // the all-pass section for the odd input sample, added to the output of the previous one
            in32                    = Lshift(static_cast<std::int32_t>(in[2 * k + 1]), 10);
            y                       = in32 - s[1];
            x                       = Smulwb(y, aFb1_20);
            const std::int32_t out2 = s[1] + x;
            s[1]                    = in32 + x;
            // add/subtract, back to int16
            outL[k] = static_cast<std::int16_t>(Sat16(RshiftRound(out2 + out1, 11)));
            outH[k] = static_cast<std::int16_t>(Sat16(RshiftRound(out2 - out1, 11)));
        }
    }

    void BiquadAltStride1(const std::int16_t* in, const std::int32_t* bQ28, const std::int32_t* aQ28, std::int32_t* s, std::int16_t* out,
                          std::int32_t len) noexcept
    {
        // direct form II transposed; the A values negated and split in two parts
        const std::int32_t a0LQ28 = (-aQ28[0]) & 0x00003FFF;
        const std::int32_t a0UQ28 = (-aQ28[0]) >> 14;
        const std::int32_t a1LQ28 = (-aQ28[1]) & 0x00003FFF;
        const std::int32_t a1UQ28 = (-aQ28[1]) >> 14;
        for (std::int32_t k = 0; k < len; ++k) {
            // s[0], s[1]: Q12
            const std::int32_t inval    = in[k];
            const std::int32_t out32Q14 = Lshift(Smlawb(s[0], bQ28[0], inval), 2);
            s[0]                        = s[1] + RshiftRound(Smulwb(out32Q14, a0LQ28), 14);
            s[0]                        = Smlawb(s[0], out32Q14, a0UQ28);
            s[0]                        = Smlawb(s[0], bQ28[1], inval);
            s[1]                        = RshiftRound(Smulwb(out32Q14, a1LQ28), 14);
            s[1]                        = Smlawb(s[1], out32Q14, a1UQ28);
            s[1]                        = Smlawb(s[1], bQ28[2], inval);
            // back to Q0, saturated
            out[k] = static_cast<std::int16_t>(Sat16((out32Q14 + (1 << 14) - 1) >> 14));
        }
    }

    void LpVariableCutoff(SilkLpState* lp, std::int16_t* frame, std::int32_t frameLength) noexcept
    {
        if (lp->mode == 0)
            return;
        // the index and the interpolation factor (TRANSITION_INT_STEPS == 64)
        std::int32_t       facQ16 = Lshift(TransitionFrames - lp->transition_frame_no, 16 - 6);
        const std::int32_t ind    = facQ16 >> 16;
        facQ16 -= Lshift(ind, 16);

        // the interpolated filter taps
        std::int32_t bQ28[TransitionNb];
        std::int32_t aQ28[TransitionNa];
        if (ind < TransitionIntNum - 1) {
            if (facQ16 > 0) {
                if (facQ16 < 32768) {
                    // facQ16 fits in 16 bits: piece-wise linear interpolation
                    for (std::int32_t nb = 0; nb < TransitionNb; ++nb)
                        bQ28[nb] = Smlawb(Transition_LP_B_Q28[ind][nb], Transition_LP_B_Q28[ind + 1][nb] - Transition_LP_B_Q28[ind][nb], facQ16);
                    for (std::int32_t na = 0; na < TransitionNa; ++na)
                        aQ28[na] = Smlawb(Transition_LP_A_Q28[ind][na], Transition_LP_A_Q28[ind + 1][na] - Transition_LP_A_Q28[ind][na], facQ16);
                } else {
                    // facQ16 - (1 << 16) fits in 16 bits
                    for (std::int32_t nb = 0; nb < TransitionNb; ++nb)
                        bQ28[nb]
                            = Smlawb(Transition_LP_B_Q28[ind + 1][nb], Transition_LP_B_Q28[ind + 1][nb] - Transition_LP_B_Q28[ind][nb], facQ16 - (1 << 16));
                    for (std::int32_t na = 0; na < TransitionNa; ++na)
                        aQ28[na]
                            = Smlawb(Transition_LP_A_Q28[ind + 1][na], Transition_LP_A_Q28[ind + 1][na] - Transition_LP_A_Q28[ind][na], facQ16 - (1 << 16));
                }
            } else {
                std::memcpy(bQ28, Transition_LP_B_Q28[ind], sizeof(bQ28));
                std::memcpy(aQ28, Transition_LP_A_Q28[ind], sizeof(aQ28));
            }
        } else {
            std::memcpy(bQ28, Transition_LP_B_Q28[TransitionIntNum - 1], sizeof(bQ28));
            std::memcpy(aQ28, Transition_LP_A_Q28[TransitionIntNum - 1], sizeof(aQ28));
        }

        // the transition frame number of the next frame
        lp->transition_frame_no = Limit(lp->transition_frame_no + lp->mode, 0, TransitionFrames);
        // ARMA low-pass filtering
        BiquadAltStride1(frame, bQ28, aQ28, lp->In_LP_State, frame, frameLength);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk
