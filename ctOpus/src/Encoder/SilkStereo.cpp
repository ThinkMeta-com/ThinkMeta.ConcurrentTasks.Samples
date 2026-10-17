// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "SilkStereo.h"

#include <cstring>

#include "SilkMacros.h"
#include "SilkSigProc.h"
#include "SilkTables.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk {

    namespace {

        constexpr std::int32_t StereoQuantTabSize    = 16;
        constexpr std::int32_t StereoQuantSubSteps   = 5;
        constexpr std::int32_t StereoInterpLenMs     = 8;
        constexpr double       StereoRatioSmoothCoef = 0.01;
        constexpr std::int32_t LaShapeMs             = 5;

        // the least-squares prediction gain of y from x, quantised
        std::int32_t StereoFindPredictor(std::int32_t* ratioQ14, const std::int16_t x[], const std::int16_t y[], std::int32_t midResAmpQ0[],
                                         std::int32_t length, std::int32_t smoothCoefQ16) noexcept
        {
            std::int32_t nrgx   = 0;
            std::int32_t nrgy   = 0;
            std::int32_t scale1 = 0;
            std::int32_t scale2 = 0;
            SumSqrShift(&nrgx, &scale1, x, length);
            SumSqrShift(&nrgy, &scale2, y, length);
            std::int32_t scale          = MaxInt(scale1, scale2);
            scale                       = scale + (scale & 1); // even
            nrgy                        = nrgy >> (scale - scale2);
            nrgx                        = nrgx >> (scale - scale1);
            nrgx                        = MaxInt(nrgx, 1);
            const std::int32_t corr     = InnerProdAlignedScale(x, y, scale, length);
            std::int32_t       predQ13  = Div32VarQ(corr, nrgx, 13);
            predQ13                     = Limit(predQ13, -(1 << 14), 1 << 14);
            const std::int32_t pred2Q10 = Smulwb(predQ13, predQ13);

            // a faster update for signals with large prediction parameters
            smoothCoefQ16 = MaxInt(smoothCoefQ16, Abs(pred2Q10));

            // the smoothed mid and residual norms
            scale          = scale >> 1;
            midResAmpQ0[0] = Smlawb(midResAmpQ0[0], Lshift(SqrtApprox(nrgx), scale) - midResAmpQ0[0], smoothCoefQ16);
            // residual energy = nrgy - 2 * pred * corr + pred^2 * nrgx
            nrgy           = SubLshift32(nrgy, Smulwb(corr, predQ13), 3 + 1);
            nrgy           = AddLshift32(nrgy, Smulwb(nrgx, pred2Q10), 6);
            midResAmpQ0[1] = Smlawb(midResAmpQ0[1], Lshift(SqrtApprox(nrgy), scale) - midResAmpQ0[1], smoothCoefQ16);

            // the ratio of the smoothed residual and mid norms
            *ratioQ14 = Div32VarQ(midResAmpQ0[1], Max(midResAmpQ0[0], 1), 14);
            *ratioQ14 = Limit(*ratioQ14, 0, 32767);
            return predQ13;
        }

        // quantises the mid/side predictors
        void StereoQuantPred(std::int32_t predQ13[], std::int8_t ix[2][3]) noexcept
        {
            std::int32_t quantPredQ13 = 0;
            for (std::int32_t n = 0; n < 2; ++n) {
                // brute-force search over the quantisation levels
                std::int32_t errMinQ13 = Int32Max;
                bool         done      = false;
                for (std::int32_t i = 0; i < StereoQuantTabSize - 1 && !done; ++i) {
                    const std::int32_t lowQ13  = stereo_pred_quant_Q13[i];
                    const std::int32_t stepQ13 = Smulwb(stereo_pred_quant_Q13[i + 1] - lowQ13, FixConst(0.5 / StereoQuantSubSteps, 16));
                    for (std::int32_t j = 0; j < StereoQuantSubSteps; ++j) {
                        const std::int32_t lvlQ13 = Smlabb(lowQ13, stepQ13, 2 * j + 1);
                        const std::int32_t errQ13 = Abs(predQ13[n] - lvlQ13);
                        if (errQ13 < errMinQ13) {
                            errMinQ13    = errQ13;
                            quantPredQ13 = lvlQ13;
                            ix[n][0]     = static_cast<std::int8_t>(i);
                            ix[n][1]     = static_cast<std::int8_t>(j);
                        } else {
                            // the error grows: past the optimum
                            done = true;
                            break;
                        }
                    }
                }
                ix[n][2]   = static_cast<std::int8_t>(Div32_16(ix[n][0], 3));
                ix[n][0]   = static_cast<std::int8_t>(ix[n][0] - ix[n][2] * 3);
                predQ13[n] = quantPredQ13;
            }
            // the second subtracted from the first predictor (helps applying them)
            predQ13[0] -= predQ13[1];
        }

    } // namespace

    void StereoLrToMs(SilkStereoEncState* state, std::int16_t x1[], std::int16_t x2[], std::int8_t ix[2][3], std::int8_t* midOnlyFlag,
                      std::int32_t midSideRatesBps[], std::int32_t totalRateBps, std::int32_t prevSpeechActQ8, bool toMono, std::int32_t fsKhz,
                      std::int32_t frameLength) noexcept
    {
        std::int16_t* mid = &x1[-2];
        std::int16_t  side[SilkMaxFrameLength + 2];
        std::int16_t  lpMid[SilkMaxFrameLength];
        std::int16_t  hpMid[SilkMaxFrameLength];
        std::int16_t  lpSide[SilkMaxFrameLength];
        std::int16_t  hpSide[SilkMaxFrameLength];

        // basic mid/side signals
        for (std::int32_t n = 0; n < frameLength + 2; ++n) {
            const std::int32_t sum  = x1[n - 2] + static_cast<std::int32_t>(x2[n - 2]);
            const std::int32_t diff = x1[n - 2] - static_cast<std::int32_t>(x2[n - 2]);
            mid[n]                  = static_cast<std::int16_t>(RshiftRound(sum, 1));
            side[n]                 = static_cast<std::int16_t>(Sat16(RshiftRound(diff, 1)));
        }
        // buffering
        std::memcpy(mid, state->sMid, 2 * sizeof(std::int16_t));
        std::memcpy(side, state->sSide, 2 * sizeof(std::int16_t));
        std::memcpy(state->sMid, &mid[frameLength], 2 * sizeof(std::int16_t));
        std::memcpy(state->sSide, &side[frameLength], 2 * sizeof(std::int16_t));

        // LP and HP filtered mid and side
        for (std::int32_t n = 0; n < frameLength; ++n) {
            const std::int32_t sum = RshiftRound(AddLshift32(mid[n] + static_cast<std::int32_t>(mid[n + 2]), mid[n + 1], 1), 2);
            lpMid[n]               = static_cast<std::int16_t>(sum);
            hpMid[n]               = static_cast<std::int16_t>(mid[n + 1] - sum);
        }
        for (std::int32_t n = 0; n < frameLength; ++n) {
            const std::int32_t sum = RshiftRound(AddLshift32(side[n] + static_cast<std::int32_t>(side[n + 2]), side[n + 1], 1), 2);
            lpSide[n]              = static_cast<std::int16_t>(sum);
            hpSide[n]              = static_cast<std::int16_t>(side[n + 1] - sum);
        }

        // energies and predictors
        const bool   is10msFrame   = frameLength == 10 * fsKhz;
        std::int32_t smoothCoefQ16 = is10msFrame ? FixConst(StereoRatioSmoothCoef / 2, 16) : FixConst(StereoRatioSmoothCoef, 16);
        smoothCoefQ16              = Smulwb(Smulbb(prevSpeechActQ8, prevSpeechActQ8), smoothCoefQ16);

        std::int32_t lpRatioQ14 = 0;
        std::int32_t hpRatioQ14 = 0;
        std::int32_t predQ13[2];
        predQ13[0] = StereoFindPredictor(&lpRatioQ14, lpMid, lpSide, &state->mid_side_amp_Q0[0], frameLength, smoothCoefQ16);
        predQ13[1] = StereoFindPredictor(&hpRatioQ14, hpMid, hpSide, &state->mid_side_amp_Q0[2], frameLength, smoothCoefQ16);

        // the ratio of the norms of residual and mid
        std::int32_t fracQ16 = Smlabb(hpRatioQ14, lpRatioQ14, 3);
        fracQ16              = Min(fracQ16, FixConst(1, 16));

        // the rate split between mid and side, possibly with a reduced stereo width
        totalRateBps -= is10msFrame ? 1200 : 600; // about the rate of the stereo parameters
        if (totalRateBps < 1)
            totalRateBps = 1;
        const std::int32_t minMidRateBps = Smlabb(2000, fsKhz, 600);

        // by default 8 parts for mid and (5+3*frac) parts for side
        const std::int32_t frac3Q16 = 3 * fracQ16;
        std::int32_t       widthQ14 = 0;
        midSideRatesBps[0]          = Div32VarQ(totalRateBps, FixConst(8 + 5, 16) + frac3Q16, 16 + 3);
        // a mid rate below the minimum reduces the stereo width
        if (midSideRatesBps[0] < minMidRateBps) {
            midSideRatesBps[0] = minMidRateBps;
            midSideRatesBps[1] = totalRateBps - midSideRatesBps[0];
            // width = 4 * ( 2 * side_rate - min_rate ) / ( ( 1 + 3 * frac ) * min_rate )
            widthQ14 = Div32VarQ(Lshift(midSideRatesBps[1], 1) - minMidRateBps, Smulwb(FixConst(1, 16) + frac3Q16, minMidRateBps), 14 + 2);
            widthQ14 = Limit(widthQ14, 0, FixConst(1, 14));
        } else {
            midSideRatesBps[1] = totalRateBps - midSideRatesBps[0];
            widthQ14           = FixConst(1, 14);
        }

        // smoother
        state->smth_width_Q14 = static_cast<std::int16_t>(Smlawb(state->smth_width_Q14, widthQ14 - state->smth_width_Q14, smoothCoefQ16));

        // very low rates or nearly amplitude panned inputs: panned-mono coding
        *midOnlyFlag = 0;
        if (toMono) {
            // the last frame before a stereo->mono transition: no stereo width
            widthQ14   = 0;
            predQ13[0] = 0;
            predQ13[1] = 0;
            StereoQuantPred(predQ13, ix);
        } else if (state->width_prev_Q14 == 0 && (8 * totalRateBps < 13 * minMidRateBps || Smulwb(fracQ16, state->smth_width_Q14) < FixConst(0.05, 14))) {
            // panned-mono; the previous frame had zero width already: scaled down, quantised predictors
            predQ13[0] = Smulbb(state->smth_width_Q14, predQ13[0]) >> 14;
            predQ13[1] = Smulbb(state->smth_width_Q14, predQ13[1]) >> 14;
            StereoQuantPred(predQ13, ix);
            // no stereo width
            widthQ14           = 0;
            predQ13[0]         = 0;
            predQ13[1]         = 0;
            midSideRatesBps[0] = totalRateBps;
            midSideRatesBps[1] = 0;
            *midOnlyFlag       = 1;
        } else if (state->width_prev_Q14 != 0 && (8 * totalRateBps < 11 * minMidRateBps || Smulwb(fracQ16, state->smth_width_Q14) < FixConst(0.02, 14))) {
            // transition to zero-width stereo
            predQ13[0] = Smulbb(state->smth_width_Q14, predQ13[0]) >> 14;
            predQ13[1] = Smulbb(state->smth_width_Q14, predQ13[1]) >> 14;
            StereoQuantPred(predQ13, ix);
            widthQ14   = 0;
            predQ13[0] = 0;
            predQ13[1] = 0;
        } else if (state->smth_width_Q14 > FixConst(0.95, 14)) {
            // full-width stereo coding
            StereoQuantPred(predQ13, ix);
            widthQ14 = FixConst(1, 14);
        } else {
            // reduced-width stereo coding; scaled down, quantised predictors
            predQ13[0] = Smulbb(state->smth_width_Q14, predQ13[0]) >> 14;
            predQ13[1] = Smulbb(state->smth_width_Q14, predQ13[1]) >> 14;
            StereoQuantPred(predQ13, ix);
            widthQ14 = state->smth_width_Q14;
        }

        // keep encoding until the tapered output has been transmitted
        if (*midOnlyFlag == 1) {
            state->silent_side_len = static_cast<std::int16_t>(state->silent_side_len + frameLength - StereoInterpLenMs * fsKhz);
            if (state->silent_side_len < LaShapeMs * fsKhz)
                *midOnlyFlag = 0;
            else
                state->silent_side_len = 10000; // limited against wrapping around
        } else {
            state->silent_side_len = 0;
        }

        if (*midOnlyFlag == 0 && midSideRatesBps[1] < 1) {
            midSideRatesBps[1] = 1;
            midSideRatesBps[0] = MaxInt(1, totalRateBps - midSideRatesBps[1]);
        }

        // interpolated predictors, the prediction subtracted from the side channel
        std::int32_t       pred0Q13  = -state->pred_prev_Q13[0];
        std::int32_t       pred1Q13  = -state->pred_prev_Q13[1];
        std::int32_t       wQ24      = Lshift(state->width_prev_Q14, 10);
        const std::int32_t denomQ16  = Div32_16(static_cast<std::int32_t>(1) << 16, StereoInterpLenMs * fsKhz);
        const std::int32_t delta0Q13 = -RshiftRound(Smulbb(predQ13[0] - state->pred_prev_Q13[0], denomQ16), 16);
        const std::int32_t delta1Q13 = -RshiftRound(Smulbb(predQ13[1] - state->pred_prev_Q13[1], denomQ16), 16);
        const std::int32_t deltawQ24 = Lshift(Smulwb(widthQ14 - state->width_prev_Q14, denomQ16), 10);
        for (std::int32_t n = 0; n < StereoInterpLenMs * fsKhz; ++n) {
            pred0Q13 += delta0Q13;
            pred1Q13 += delta1Q13;
            wQ24 += deltawQ24;
            std::int32_t sum = Lshift(AddLshift32(mid[n] + static_cast<std::int32_t>(mid[n + 2]), mid[n + 1], 1), 9); // Q11
            sum              = Smlawb(Smulwb(wQ24, side[n + 1]), sum, pred0Q13);                                      // Q8
            sum              = Smlawb(sum, Lshift(static_cast<std::int32_t>(mid[n + 1]), 11), pred1Q13);              // Q8
            x2[n - 1]        = static_cast<std::int16_t>(Sat16(RshiftRound(sum, 8)));
        }
        pred0Q13 = -predQ13[0];
        pred1Q13 = -predQ13[1];
        wQ24     = Lshift(widthQ14, 10);
        for (std::int32_t n = StereoInterpLenMs * fsKhz; n < frameLength; ++n) {
            std::int32_t sum = Lshift(AddLshift32(mid[n] + static_cast<std::int32_t>(mid[n + 2]), mid[n + 1], 1), 9); // Q11
            sum              = Smlawb(Smulwb(wQ24, side[n + 1]), sum, pred0Q13);                                      // Q8
            sum              = Smlawb(sum, Lshift(static_cast<std::int32_t>(mid[n + 1]), 11), pred1Q13);              // Q8
            x2[n - 1]        = static_cast<std::int16_t>(Sat16(RshiftRound(sum, 8)));
        }
        state->pred_prev_Q13[0] = static_cast<std::int16_t>(predQ13[0]);
        state->pred_prev_Q13[1] = static_cast<std::int16_t>(predQ13[1]);
        state->width_prev_Q14   = static_cast<std::int16_t>(widthQ14);
    }

    void StereoEncodePred(RangeEncoder* rangeEnc, const std::int8_t ix[2][3]) noexcept
    {
        const std::int32_t n = 5 * ix[0][2] + ix[1][2];
        rangeEnc->EncodeIcdf(n, stereo_pred_joint_iCDF, 8);
        for (std::int32_t k = 0; k < 2; ++k) {
            rangeEnc->EncodeIcdf(ix[k][0], uniform3_iCDF, 8);
            rangeEnc->EncodeIcdf(ix[k][1], uniform5_iCDF, 8);
        }
    }

    void StereoEncodeMidOnly(RangeEncoder* rangeEnc, std::int8_t midOnlyFlag) noexcept
    {
        rangeEnc->EncodeIcdf(midOnlyFlag, stereo_only_code_mid_iCDF, 8);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk
