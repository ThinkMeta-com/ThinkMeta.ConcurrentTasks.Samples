// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "SilkNlsf.h"

#include <cstring>

#include "SilkCoding.h"
#include "SilkDefines.h"
#include "SilkMacros.h"
#include "SilkSigProc.h"
#include "SilkTables.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk {

    namespace {

        constexpr std::int32_t MaxOrderLpc               = 24; // SILK_MAX_ORDER_LPC
        constexpr std::int32_t BinDivStepsA2Nlsf         = 3;
        constexpr std::int32_t MaxIterationsA2Nlsf       = 16;
        constexpr std::int32_t LsfCosTabSzFix            = 128;
        constexpr std::int32_t MaxLpcStabilizeIterations = 16;
        constexpr float        MaxPredictionPowerGain    = 1e4f;
        constexpr std::int32_t NlsfWQ                    = 2;
        constexpr std::int32_t NlsfQuantMaxAmplitudeExt  = 10;
        constexpr double       NlsfQuantLevelAdj         = 0.1;
        constexpr std::int32_t NlsfQuantDelDecStatesLog2 = 2;
        constexpr std::int32_t NlsfQuantDelDecStates     = 1 << NlsfQuantDelDecStatesLog2;
        constexpr std::int32_t NlsfMaxVectors            = 32;
        constexpr std::int32_t NlsfMaxStabilizeLoops     = 20;

        // --- bwexpander_32.c, LPC_fit.c, LPC_inv_pred_gain.c ------------------------------------

        void Bwexpander32(std::int32_t* ar, std::int32_t d, std::int32_t chirpQ16) noexcept
        {
            const std::int32_t chirpMinusOneQ16 = chirpQ16 - 65536;
            for (std::int32_t i = 0; i < d - 1; ++i) {
                ar[i] = Smulww(chirpQ16, ar[i]);
                chirpQ16 += RshiftRound(Mul(chirpQ16, chirpMinusOneQ16), 16);
            }
            ar[d - 1] = Smulww(chirpQ16, ar[d - 1]);
        }

        // int32 coefficients to int16 ones without wrap-around
        void LpcFit(std::int16_t* aQout, std::int32_t* aQin, std::int32_t qout, std::int32_t qin, std::int32_t d) noexcept
        {
            // the maximum absolute value limited so that the coefficients fit in int16
            std::int32_t i   = 0;
            std::int32_t idx = 0;
            for (; i < 10; ++i) {
                std::int32_t maxabs = 0;
                for (std::int32_t k = 0; k < d; ++k) {
                    const std::int32_t absval = Abs(aQin[k]);
                    if (absval > maxabs) {
                        maxabs = absval;
                        idx    = k;
                    }
                }
                maxabs = RshiftRound(maxabs, qin - qout);
                if (maxabs <= Int16Max)
                    break;
                // the magnitude of the prediction coefficients reduced
                maxabs                      = Min(maxabs, 163838); // (int32 max >> 14) + int16 max
                const std::int32_t chirpQ16 = FixConst(0.999, 16) - Div32(Lshift(maxabs - Int16Max, 14), Mul(maxabs, idx + 1) >> 2);
                Bwexpander32(aQin, d, chirpQ16);
            }
            if (i == 10) {
                // the last iteration reached: the coefficients clipped
                for (std::int32_t k = 0; k < d; ++k) {
                    aQout[k] = static_cast<std::int16_t>(Sat16(RshiftRound(aQin[k], qin - qout)));
                    aQin[k]  = Lshift(static_cast<std::int32_t>(aQout[k]), qin - qout);
                }
            } else {
                for (std::int32_t k = 0; k < d; ++k)
                    aQout[k] = static_cast<std::int16_t>(RshiftRound(aQin[k], qin - qout));
            }
        }

        constexpr std::int32_t InvPredQa = 24;

        std::int32_t Mul32FracQ(std::int32_t a32, std::int32_t b32, std::int32_t q) noexcept
        {
            return static_cast<std::int32_t>(RshiftRound64(Smull(a32, b32), q));
        }

        // the inverse prediction gain (Q30, energy domain); 0 if the coefficients are unstable
        std::int32_t LpcInversePredGainQa(std::int32_t aQa[MaxOrderLpc], std::int32_t order) noexcept
        {
            constexpr std::int32_t aLimit     = FixConst(0.99975, InvPredQa);
            const std::int32_t     minInvGain = FixConst(1.0f / MaxPredictionPowerGain, 30);
            std::int32_t           invGainQ30 = FixConst(1, 30);
            std::int32_t           k          = order - 1;
            for (; k > 0; --k) {
                // the stability check
                if (aQa[k] > aLimit || aQa[k] < -aLimit)
                    return 0;
                // the reflection coefficient: the negated AR coefficient
                const std::int32_t rcQ31      = -Lshift(aQa[k], 31 - InvPredQa);
                const std::int32_t rcMult1Q30 = FixConst(1, 30) - Smmul(rcQ31, rcQ31); // [1, 2^30]
                // the inverse gain, [0, 2^30]
                invGainQ30 = Lshift(Smmul(invGainQ30, rcMult1Q30), 2);
                if (invGainQ30 < minInvGain)
                    return 0;
                // rc_mult2 in [2^30, int32 max]
                const std::int32_t mult2Q  = 32 - Clz32(Abs(rcMult1Q30));
                const std::int32_t rcMult2 = Inverse32VarQ(rcMult1Q30, mult2Q + 30);
                // the AR coefficients updated
                for (std::int32_t n = 0; n < (k + 1) >> 1; ++n) {
                    const std::int32_t tmp1  = aQa[n];
                    const std::int32_t tmp2  = aQa[k - n - 1];
                    std::int64_t       tmp64 = RshiftRound64(Smull(SubSat32(tmp1, Mul32FracQ(tmp2, rcQ31, 31)), rcMult2), mult2Q);
                    if (tmp64 > Int32Max || tmp64 < Int32Min)
                        return 0;
                    aQa[n] = static_cast<std::int32_t>(tmp64);
                    tmp64  = RshiftRound64(Smull(SubSat32(tmp2, Mul32FracQ(tmp1, rcQ31, 31)), rcMult2), mult2Q);
                    if (tmp64 > Int32Max || tmp64 < Int32Min)
                        return 0;
                    aQa[k - n - 1] = static_cast<std::int32_t>(tmp64);
                }
            }
            // the stability check
            if (aQa[k] > aLimit || aQa[k] < -aLimit)
                return 0;
            const std::int32_t rcQ31      = -Lshift(aQa[0], 31 - InvPredQa);
            const std::int32_t rcMult1Q30 = FixConst(1, 30) - Smmul(rcQ31, rcQ31);
            invGainQ30                    = Lshift(Smmul(invGainQ30, rcMult1Q30), 2);
            if (invGainQ30 < minInvGain)
                return 0;
            return invGainQ30;
        }

        // silk_LPC_inverse_pred_gain_c: for Q12 coefficients
        std::int32_t LpcInversePredGain(const std::int16_t* aQ12, std::int32_t order) noexcept
        {
            std::int32_t atmpQa[MaxOrderLpc];
            std::int32_t dcResp = 0;
            // to the higher Q domain
            for (std::int32_t k = 0; k < order; ++k) {
                dcResp += static_cast<std::int32_t>(aQ12[k]);
                atmpQa[k] = Lshift32(static_cast<std::int32_t>(aQ12[k]), InvPredQa - 12);
            }
            // an unstable DC needs no full calculation
            if (dcResp >= 4096)
                return 0;
            return LpcInversePredGainQa(atmpQa, order);
        }

        // --- A2NLSF.c ---------------------------------------------------------------------------

        // polynomials from cos(n * f) to cos(f)^n
        void A2NlsfTransPoly(std::int32_t* p, std::int32_t dd) noexcept
        {
            for (std::int32_t k = 2; k <= dd; ++k) {
                for (std::int32_t n = dd; n > k; --n)
                    p[n - 2] -= p[n];
                p[k - 2] -= Lshift(p[k], 1);
            }
        }

        // the polynomial (Q16) evaluated at x (Q12)
        std::int32_t A2NlsfEvalPoly(const std::int32_t* p, std::int32_t x, std::int32_t dd) noexcept
        {
            std::int32_t       y32  = p[dd];
            const std::int32_t xQ16 = Lshift(x, 4);
            for (std::int32_t n = dd - 1; n >= 0; --n)
                y32 = Smlaww(p[n], y32, xQ16);
            return y32;
        }

        void A2NlsfInit(const std::int32_t* aQ16, std::int32_t* p, std::int32_t* q, std::int32_t dd) noexcept
        {
            // the filter coefficients to even and odd polynomials
            p[dd] = Lshift(1, 16);
            q[dd] = Lshift(1, 16);
            for (std::int32_t k = 0; k < dd; ++k) {
                p[k] = -aQ16[dd - k - 1] - aQ16[dd + k];
                q[k] = -aQ16[dd - k - 1] + aQ16[dd + k];
            }
            // the zeros divided out: for even orders z = 1 is always a root of Q, z = -1 of P
            for (std::int32_t k = dd; k > 0; --k) {
                p[k - 1] -= p[k];
                q[k - 1] += q[k];
            }
            A2NlsfTransPoly(p, dd);
            A2NlsfTransPoly(q, dd);
        }

        // --- NLSF2A.c ---------------------------------------------------------------------------

        constexpr std::int32_t Nlsf2AQa = 16;

        void Nlsf2AFindPoly(std::int32_t* out, const std::int32_t* cLsf, std::int32_t dd) noexcept
        {
            out[0] = Lshift(1, Nlsf2AQa);
            out[1] = -cLsf[0];
            for (std::int32_t k = 1; k < dd; ++k) {
                const std::int32_t ftmp = cLsf[2 * k];
                out[k + 1]              = Lshift(out[k - 1], 1) - static_cast<std::int32_t>(RshiftRound64(Smull(ftmp, out[k]), Nlsf2AQa));
                for (std::int32_t n = k; n > 1; --n)
                    out[n] += out[n - 2] - static_cast<std::int32_t>(RshiftRound64(Smull(ftmp, out[n - 1]), Nlsf2AQa));
                out[1] -= ftmp;
            }
        }

        // --- sort.c, NLSF_*.c -------------------------------------------------------------------

        void InsertionSortIncreasing(std::int32_t* a, std::int32_t* idx, std::int32_t l, std::int32_t k) noexcept
        {
            for (std::int32_t i = 0; i < k; ++i)
                idx[i] = i;
            for (std::int32_t i = 1; i < k; ++i) {
                const std::int32_t value = a[i];
                std::int32_t       j     = i - 1;
                for (; j >= 0 && value < a[j]; --j) {
                    a[j + 1]   = a[j];
                    idx[j + 1] = idx[j];
                }
                a[j + 1]   = value;
                idx[j + 1] = i;
            }
            // the remaining values: only the first k positions kept correct
            for (std::int32_t i = k; i < l; ++i) {
                const std::int32_t value = a[i];
                if (value < a[k - 1]) {
                    std::int32_t j = k - 2;
                    for (; j >= 0 && value < a[j]; --j) {
                        a[j + 1]   = a[j];
                        idx[j + 1] = idx[j];
                    }
                    a[j + 1]   = value;
                    idx[j + 1] = i;
                }
            }
        }

        void InsertionSortIncreasingAllValuesInt16(std::int16_t* a, std::int32_t l) noexcept
        {
            for (std::int32_t i = 1; i < l; ++i) {
                const std::int32_t value = a[i];
                std::int32_t       j     = i - 1;
                for (; j >= 0 && value < a[j]; --j)
                    a[j + 1] = a[j];
                a[j + 1] = static_cast<std::int16_t>(value);
            }
        }

        // silk_NLSF_stabilize: the NLSFs moved apart and away from the borders with a minimal
        // Euclidean change, sorted
        void NlsfStabilize(std::int16_t* nlsfQ15, const std::int16_t* nDeltaMinQ15, std::int32_t l) noexcept
        {
            std::int32_t loops = 0;
            for (; loops < NlsfMaxStabilizeLoops; ++loops) {
                // the smallest distance: first, middle, last elements
                std::int32_t minDiffQ15 = nlsfQ15[0] - nDeltaMinQ15[0];
                std::int32_t ix         = 0;
                for (std::int32_t i = 1; i <= l - 1; ++i) {
                    const std::int32_t diffQ15 = nlsfQ15[i] - (nlsfQ15[i - 1] + nDeltaMinQ15[i]);
                    if (diffQ15 < minDiffQ15) {
                        minDiffQ15 = diffQ15;
                        ix         = i;
                    }
                }
                const std::int32_t diffQ15 = (1 << 15) - (nlsfQ15[l - 1] + nDeltaMinQ15[l]);
                if (diffQ15 < minDiffQ15) {
                    minDiffQ15 = diffQ15;
                    ix         = l;
                }

                // a non-negative smallest distance: done
                if (minDiffQ15 >= 0)
                    return;

                if (ix == 0) {
                    // away from the lower limit
                    nlsfQ15[0] = nDeltaMinQ15[0];
                } else if (ix == l) {
                    // away from the higher limit
                    nlsfQ15[l - 1] = static_cast<std::int16_t>((1 << 15) - nDeltaMinQ15[l]);
                } else {
                    // the lower and upper extremes of the current center frequency
                    std::int32_t minCenterQ15 = 0;
                    for (std::int32_t k = 0; k < ix; ++k)
                        minCenterQ15 += nDeltaMinQ15[k];
                    minCenterQ15 += nDeltaMinQ15[ix] >> 1;
                    std::int32_t maxCenterQ15 = 1 << 15;
                    for (std::int32_t k = l; k > ix; --k)
                        maxCenterQ15 -= nDeltaMinQ15[k];
                    maxCenterQ15 -= nDeltaMinQ15[ix] >> 1;

                    // moved apart, sorted, with the same center frequency
                    const std::int16_t centerFreqQ15 = static_cast<std::int16_t>(
                        Limit(RshiftRound(static_cast<std::int32_t>(nlsfQ15[ix - 1]) + static_cast<std::int32_t>(nlsfQ15[ix]), 1), minCenterQ15, maxCenterQ15));
                    nlsfQ15[ix - 1] = static_cast<std::int16_t>(centerFreqQ15 - (nDeltaMinQ15[ix] >> 1));
                    nlsfQ15[ix]     = static_cast<std::int16_t>(nlsfQ15[ix - 1] + nDeltaMinQ15[ix]);
                }
            }

            // the safe and simple fallback, less ideal than the above
            if (loops == NlsfMaxStabilizeLoops) {
                InsertionSortIncreasingAllValuesInt16(&nlsfQ15[0], l);
                // the first NLSF no less than NDeltaMin[0], the delta_min distance kept
                nlsfQ15[0] = static_cast<std::int16_t>(MaxInt(nlsfQ15[0], nDeltaMinQ15[0]));
                for (std::int32_t i = 1; i < l; ++i)
                    nlsfQ15[i] = static_cast<std::int16_t>(MaxInt(nlsfQ15[i], AddSat16(nlsfQ15[i - 1], nDeltaMinQ15[i])));
                // the last NLSF no higher than 1 - NDeltaMin[L], the distance kept
                nlsfQ15[l - 1] = static_cast<std::int16_t>(MinInt(nlsfQ15[l - 1], (1 << 15) - nDeltaMinQ15[l]));
                for (std::int32_t i = l - 2; i >= 0; --i)
                    nlsfQ15[i] = static_cast<std::int16_t>(MinInt(nlsfQ15[i], nlsfQ15[i + 1] - nDeltaMinQ15[i + 1]));
            }
        }

        // silk_NLSF_VQ_weights_laroia: the Laroia low complexity NLSF weights
        void NlsfVqWeightsLaroia(std::int16_t* pNlsfwQOut, const std::int16_t* pNlsfQ15, std::int32_t d) noexcept
        {
            constexpr std::int32_t num = static_cast<std::int32_t>(1) << (15 + NlsfWQ);
            // the first value
            std::int32_t tmp1 = MaxInt(pNlsfQ15[0], 1);
            tmp1              = Div32_16(num, tmp1);
            std::int32_t tmp2 = MaxInt(pNlsfQ15[1] - pNlsfQ15[0], 1);
            tmp2              = Div32_16(num, tmp2);
            pNlsfwQOut[0]     = static_cast<std::int16_t>(MinInt(tmp1 + tmp2, Int16Max));
            // the main loop
            for (std::int32_t k = 1; k < d - 1; k += 2) {
                tmp1              = MaxInt(pNlsfQ15[k + 1] - pNlsfQ15[k], 1);
                tmp1              = Div32_16(num, tmp1);
                pNlsfwQOut[k]     = static_cast<std::int16_t>(MinInt(tmp1 + tmp2, Int16Max));
                tmp2              = MaxInt(pNlsfQ15[k + 2] - pNlsfQ15[k + 1], 1);
                tmp2              = Div32_16(num, tmp2);
                pNlsfwQOut[k + 1] = static_cast<std::int16_t>(MinInt(tmp1 + tmp2, Int16Max));
            }
            // the last value
            tmp1              = MaxInt((1 << 15) - pNlsfQ15[d - 1], 1);
            tmp1              = Div32_16(num, tmp1);
            pNlsfwQOut[d - 1] = static_cast<std::int16_t>(MinInt(tmp1 + tmp2, Int16Max));
        }

        // silk_NLSF_VQ: the weighted predictive quantisation errors of all codebook vectors
        void NlsfVq(std::int32_t errQ24[], const std::int16_t inQ15[], const std::uint8_t pCbQ8[], const std::int16_t pWghtQ9[], std::int32_t k,
                    std::int32_t lpcOrder) noexcept
        {
            const std::uint8_t* cbQ8 = pCbQ8;
            const std::int16_t* wQ9  = pWghtQ9;
            for (std::int32_t i = 0; i < k; ++i) {
                std::int32_t sumErrorQ24 = 0;
                std::int32_t predQ24     = 0;
                for (std::int32_t m = lpcOrder - 2; m >= 0; m -= 2) {
                    // the weighted absolute predictive error of index m + 1, then of m
                    std::int32_t diffQ15  = SubLshift32(inQ15[m + 1], static_cast<std::int32_t>(cbQ8[m + 1]), 7);
                    std::int32_t diffwQ24 = Smulbb(diffQ15, wQ9[m + 1]);
                    sumErrorQ24           = sumErrorQ24 + Abs(SubRshift32(diffwQ24, predQ24, 1));
                    predQ24               = diffwQ24;
                    diffQ15               = SubLshift32(inQ15[m], static_cast<std::int32_t>(cbQ8[m]), 7);
                    diffwQ24              = Smulbb(diffQ15, wQ9[m]);
                    sumErrorQ24           = sumErrorQ24 + Abs(SubRshift32(diffwQ24, predQ24, 1));
                    predQ24               = diffwQ24;
                }
                errQ24[i] = sumErrorQ24;
                cbQ8 += lpcOrder;
                wQ9 += lpcOrder;
            }
        }

        // silk_NLSF_del_dec_quant: the delayed-decision quantiser of the NLSF residuals; returns
        // the RD value in Q25
        std::int32_t NlsfDelDecQuant(std::int8_t indices[], const std::int16_t xQ10[], const std::int16_t wQ5[], const std::uint8_t predCoefQ8[],
                                     const std::int16_t ecIx[], const std::uint8_t ecRatesQ5[], std::int32_t quantStepSizeQ16, std::int16_t invQuantStepSizeQ6,
                                     std::int32_t muQ20, std::int16_t order) noexcept
        {
            constexpr std::int32_t levelAdjQ10 = FixConst(NlsfQuantLevelAdj, 10);
            std::int32_t           out0Q10Table[2 * NlsfQuantMaxAmplitudeExt];
            std::int32_t           out1Q10Table[2 * NlsfQuantMaxAmplitudeExt];
            for (std::int32_t i = -NlsfQuantMaxAmplitudeExt; i <= NlsfQuantMaxAmplitudeExt - 1; ++i) {
                std::int16_t out0Q10 = static_cast<std::int16_t>(Lshift(i, 10));
                std::int16_t out1Q10 = static_cast<std::int16_t>(out0Q10 + 1024);
                if (i > 0) {
                    out0Q10 = static_cast<std::int16_t>(out0Q10 - levelAdjQ10);
                    out1Q10 = static_cast<std::int16_t>(out1Q10 - levelAdjQ10);
                } else if (i == 0) {
                    out1Q10 = static_cast<std::int16_t>(out1Q10 - levelAdjQ10);
                } else if (i == -1) {
                    out0Q10 = static_cast<std::int16_t>(out0Q10 + levelAdjQ10);
                } else {
                    out0Q10 = static_cast<std::int16_t>(out0Q10 + levelAdjQ10);
                    out1Q10 = static_cast<std::int16_t>(out1Q10 + levelAdjQ10);
                }
                out0Q10Table[i + NlsfQuantMaxAmplitudeExt] = Smulbb(out0Q10, quantStepSizeQ16) >> 16;
                out1Q10Table[i + NlsfQuantMaxAmplitudeExt] = Smulbb(out1Q10, quantStepSizeQ16) >> 16;
            }

            std::int32_t indSort[NlsfQuantDelDecStates];
            std::int8_t  ind[NlsfQuantDelDecStates][SilkMaxLpcOrder];
            std::int16_t prevOutQ10[2 * NlsfQuantDelDecStates];
            std::int32_t rdQ25[2 * NlsfQuantDelDecStates];
            std::int32_t rdMinQ25[NlsfQuantDelDecStates];
            std::int32_t rdMaxQ25[NlsfQuantDelDecStates];
            std::int32_t nStates = 1;
            rdQ25[0]             = 0;
            prevOutQ10[0]        = 0;
            for (std::int32_t i = order - 1; i >= 0; --i) {
                const std::uint8_t* ratesQ5 = &ecRatesQ5[ecIx[i]];
                const std::int32_t  inQ10   = xQ10[i];
                for (std::int32_t j = 0; j < nStates; ++j) {
                    const std::int32_t predQ10 = Smulbb(static_cast<std::int16_t>(predCoefQ8[i]), prevOutQ10[j]) >> 8;
                    const std::int32_t resQ10  = inQ10 - predQ10;
                    std::int32_t       indTmp  = Smulbb(invQuantStepSizeQ6, resQ10) >> 16;
                    indTmp                     = Limit(indTmp, -NlsfQuantMaxAmplitudeExt, NlsfQuantMaxAmplitudeExt - 1);
                    ind[j][i]                  = static_cast<std::int8_t>(indTmp);

                    // the outputs of indTmp and indTmp + 1
                    std::int16_t out0Q10    = static_cast<std::int16_t>(out0Q10Table[indTmp + NlsfQuantMaxAmplitudeExt]);
                    std::int16_t out1Q10    = static_cast<std::int16_t>(out1Q10Table[indTmp + NlsfQuantMaxAmplitudeExt]);
                    out0Q10                 = static_cast<std::int16_t>(out0Q10 + predQ10);
                    out1Q10                 = static_cast<std::int16_t>(out1Q10 + predQ10);
                    prevOutQ10[j]           = out0Q10;
                    prevOutQ10[j + nStates] = out1Q10;

                    // the RD of indTmp and indTmp + 1
                    std::int32_t rate0Q5 = 0;
                    std::int32_t rate1Q5 = 0;
                    if (indTmp + 1 >= NlsfQuantMaxAmplitude) {
                        if (indTmp + 1 == NlsfQuantMaxAmplitude) {
                            rate0Q5 = ratesQ5[indTmp + NlsfQuantMaxAmplitude];
                            rate1Q5 = 280;
                        } else {
                            rate0Q5 = Smlabb(280 - 43 * NlsfQuantMaxAmplitude, 43, indTmp);
                            rate1Q5 = rate0Q5 + 43;
                        }
                    } else if (indTmp <= -NlsfQuantMaxAmplitude) {
                        if (indTmp == -NlsfQuantMaxAmplitude) {
                            rate0Q5 = 280;
                            rate1Q5 = ratesQ5[indTmp + 1 + NlsfQuantMaxAmplitude];
                        } else {
                            rate0Q5 = Smlabb(280 - 43 * NlsfQuantMaxAmplitude, -43, indTmp);
                            rate1Q5 = rate0Q5 - 43;
                        }
                    } else {
                        rate0Q5 = ratesQ5[indTmp + NlsfQuantMaxAmplitude];
                        rate1Q5 = ratesQ5[indTmp + 1 + NlsfQuantMaxAmplitude];
                    }
                    const std::int32_t rdTmpQ25 = rdQ25[j];
                    std::int32_t       diffQ10  = inQ10 - out0Q10;
                    rdQ25[j]                    = Smlabb(Mla(rdTmpQ25, Smulbb(diffQ10, diffQ10), wQ5[i]), muQ20, rate0Q5);
                    diffQ10                     = inQ10 - out1Q10;
                    rdQ25[j + nStates]          = Smlabb(Mla(rdTmpQ25, Smulbb(diffQ10, diffQ10), wQ5[i]), muQ20, rate1Q5);
                }

                if (nStates <= NlsfQuantDelDecStates / 2) {
                    // the number of states doubled, copied
                    for (std::int32_t j = 0; j < nStates; ++j)
                        ind[j + nStates][i] = static_cast<std::int8_t>(ind[j][i] + 1);
                    nStates = Lshift(nStates, 1);
                    for (std::int32_t j = nStates; j < NlsfQuantDelDecStates; ++j)
                        ind[j][i] = ind[j - nStates][i];
                } else {
                    // the lower and upper halves of rdQ25 sorted pairwise
                    for (std::int32_t j = 0; j < NlsfQuantDelDecStates; ++j) {
                        if (rdQ25[j] > rdQ25[j + NlsfQuantDelDecStates]) {
                            rdMaxQ25[j]                      = rdQ25[j];
                            rdMinQ25[j]                      = rdQ25[j + NlsfQuantDelDecStates];
                            rdQ25[j]                         = rdMinQ25[j];
                            rdQ25[j + NlsfQuantDelDecStates] = rdMaxQ25[j];
                            // the prev_out values swapped
                            const std::int16_t out0Q10            = prevOutQ10[j];
                            prevOutQ10[j]                         = prevOutQ10[j + NlsfQuantDelDecStates];
                            prevOutQ10[j + NlsfQuantDelDecStates] = out0Q10;
                            indSort[j]                            = j + NlsfQuantDelDecStates;
                        } else {
                            rdMinQ25[j] = rdQ25[j];
                            rdMaxQ25[j] = rdQ25[j + NlsfQuantDelDecStates];
                            indSort[j]  = j;
                        }
                    }
                    // the highest RD values of the winning half against the lowest of the losing
                    // half, copied if necessary; indSort then holds the indices of the winners
                    for (;;) {
                        std::int32_t minMaxQ25 = Int32Max;
                        std::int32_t maxMinQ25 = 0;
                        std::int32_t indMinMax = 0;
                        std::int32_t indMaxMin = 0;
                        for (std::int32_t j = 0; j < NlsfQuantDelDecStates; ++j) {
                            if (minMaxQ25 > rdMaxQ25[j]) {
                                minMaxQ25 = rdMaxQ25[j];
                                indMinMax = j;
                            }
                            if (maxMinQ25 < rdMinQ25[j]) {
                                maxMinQ25 = rdMinQ25[j];
                                indMaxMin = j;
                            }
                        }
                        if (minMaxQ25 >= maxMinQ25)
                            break;
                        // indMinMax copied to indMaxMin
                        indSort[indMaxMin]    = indSort[indMinMax] ^ NlsfQuantDelDecStates;
                        rdQ25[indMaxMin]      = rdQ25[indMinMax + NlsfQuantDelDecStates];
                        prevOutQ10[indMaxMin] = prevOutQ10[indMinMax + NlsfQuantDelDecStates];
                        rdMinQ25[indMaxMin]   = 0;
                        rdMaxQ25[indMinMax]   = Int32Max;
                        std::memcpy(ind[indMaxMin], ind[indMinMax], SilkMaxLpcOrder * sizeof(std::int8_t));
                    }
                    // the index incremented if it comes from the upper half
                    for (std::int32_t j = 0; j < NlsfQuantDelDecStates; ++j)
                        ind[j][i] = static_cast<std::int8_t>(ind[j][i] + (indSort[j] >> NlsfQuantDelDecStatesLog2));
                }
            }

            // the last sample: the winner, its indices and RD value
            std::int32_t indTmp = 0;
            std::int32_t minQ25 = Int32Max;
            for (std::int32_t j = 0; j < 2 * NlsfQuantDelDecStates; ++j) {
                if (minQ25 > rdQ25[j]) {
                    minQ25 = rdQ25[j];
                    indTmp = j;
                }
            }
            for (std::int32_t j = 0; j < order; ++j)
                indices[j] = ind[indTmp & (NlsfQuantDelDecStates - 1)][j];
            indices[0] = static_cast<std::int8_t>(indices[0] + (indTmp >> NlsfQuantDelDecStatesLog2));
            return minQ25;
        }

        // silk_NLSF_decode: the predictive dequantisation, the first stage added, stabilised
        void NlsfDecode(std::int16_t* pNlsfQ15, const std::int8_t* nlsfIndices, const SilkNlsfCb* cb) noexcept
        {
            std::uint8_t predQ8[SilkMaxLpcOrder];
            std::int16_t ecIx[SilkMaxLpcOrder];
            std::int16_t resQ10[SilkMaxLpcOrder];
            NlsfUnpack(ecIx, predQ8, cb, nlsfIndices[0]);

            // the predictive residual dequantiser (silk_NLSF_residual_dequant)
            constexpr std::int32_t levelAdjQ10 = FixConst(NlsfQuantLevelAdj, 10);
            std::int32_t           outQ10      = 0;
            for (std::int32_t i = cb->order - 1; i >= 0; --i) {
                const std::int32_t predQ10 = Smulbb(outQ10, static_cast<std::int16_t>(predQ8[i])) >> 8;
                outQ10                     = Lshift(nlsfIndices[i + 1], 10);
                if (outQ10 > 0)
                    outQ10 = outQ10 - levelAdjQ10;
                else if (outQ10 < 0)
                    outQ10 = outQ10 + levelAdjQ10;
                outQ10    = Smlawb(predQ10, outQ10, cb->quantStepSize_Q16);
                resQ10[i] = static_cast<std::int16_t>(outQ10);
            }

            // the inverse square-rooted weights applied, the first stage added
            const std::uint8_t* pCbElement = &cb->CB1_NLSF_Q8[nlsfIndices[0] * cb->order];
            const std::int16_t* pCbWghtQ9  = &cb->CB1_Wght_Q9[nlsfIndices[0] * cb->order];
            for (std::int32_t i = 0; i < cb->order; ++i) {
                const std::int32_t nlsfQ15Tmp
                    = AddLshift32(Div32_16(Lshift(static_cast<std::int32_t>(resQ10[i]), 14), pCbWghtQ9[i]), static_cast<std::int16_t>(pCbElement[i]), 7);
                pNlsfQ15[i] = static_cast<std::int16_t>(Limit(nlsfQ15Tmp, 0, 32767));
            }
            NlsfStabilize(pNlsfQ15, cb->deltaMin_Q15, cb->order);
        }

        // silk_NLSF_encode: the NLSF vector quantised (multi-stage VQ with a trellis quantiser)
        void NlsfEncode(std::int8_t* nlsfIndices, std::int16_t* pNlsfQ15, const SilkNlsfCb* cb, const std::int16_t* pWQ2, std::int32_t nlsfMuQ20,
                        std::int32_t nSurvivors, std::int32_t signalType) noexcept
        {
            NlsfStabilize(pNlsfQ15, cb->deltaMin_Q15, cb->order);

            // the first stage: VQ, the quantisation errors sorted
            std::int32_t errQ24[NlsfMaxVectors];
            NlsfVq(errQ24, pNlsfQ15, cb->CB1_NLSF_Q8, cb->CB1_Wght_Q9, cb->nVectors, cb->order);
            std::int32_t tempIndices1[NlsfMaxVectors];
            InsertionSortIncreasing(errQ24, tempIndices1, cb->nVectors, nSurvivors);

            std::int32_t rdQ25[NlsfMaxVectors];
            std::int8_t  tempIndices2[NlsfMaxVectors * SilkMaxLpcOrder];
            std::int16_t resQ10[SilkMaxLpcOrder];
            std::int16_t wAdjQ5[SilkMaxLpcOrder];
            std::uint8_t predQ8[SilkMaxLpcOrder];
            std::int16_t ecIx[SilkMaxLpcOrder];
            for (std::int32_t s = 0; s < nSurvivors; ++s) {
                const std::int32_t ind1 = tempIndices1[s];
                // the residual after the first stage
                const std::uint8_t* pCbElement = &cb->CB1_NLSF_Q8[ind1 * cb->order];
                const std::int16_t* pCbWghtQ9  = &cb->CB1_Wght_Q9[ind1 * cb->order];
                for (std::int32_t i = 0; i < cb->order; ++i) {
                    const std::int16_t nlsfTmpQ15 = Lshift16(static_cast<std::int16_t>(pCbElement[i]), 7);
                    const std::int32_t wTmpQ9     = pCbWghtQ9[i];
                    resQ10[i]                     = static_cast<std::int16_t>(Smulbb(pNlsfQ15[i] - nlsfTmpQ15, wTmpQ9) >> 14);
                    wAdjQ5[i]                     = static_cast<std::int16_t>(Div32VarQ(static_cast<std::int32_t>(pWQ2[i]), Smulbb(wTmpQ9, wTmpQ9), 21));
                }
                // the entropy table indices and the predictor of the CB1 index
                NlsfUnpack(ecIx, predQ8, cb, ind1);
                // the trellis quantiser
                rdQ25[s] = NlsfDelDecQuant(&tempIndices2[s * SilkMaxLpcOrder], resQ10, wAdjQ5, predQ8, ecIx, cb->ec_Rates_Q5, cb->quantStepSize_Q16,
                                           cb->invQuantStepSize_Q6, nlsfMuQ20, cb->order);
                // plus the rate of the first stage
                const std::uint8_t* iCdf   = &cb->CB1_iCDF[(signalType >> 1) * cb->nVectors];
                const std::int32_t  probQ8 = ind1 == 0 ? 256 - iCdf[ind1] : iCdf[ind1 - 1] - iCdf[ind1];
                const std::int32_t  bitsQ7 = (8 << 7) - Lin2Log(probQ8);
                rdQ25[s]                   = Smlabb(rdQ25[s], bitsQ7, nlsfMuQ20 >> 2);
            }

            // the lowest rate-distortion error
            std::int32_t bestIndex = 0;
            InsertionSortIncreasing(rdQ25, &bestIndex, nSurvivors, 1);
            nlsfIndices[0] = static_cast<std::int8_t>(tempIndices1[bestIndex]);
            std::memcpy(&nlsfIndices[1], &tempIndices2[bestIndex * SilkMaxLpcOrder], cb->order * sizeof(std::int8_t));

            NlsfDecode(pNlsfQ15, nlsfIndices, cb);
        }

    } // namespace

    void A2Nlsf(std::int16_t* nlsf, std::int32_t* aQ16, std::int32_t d) noexcept
    {
        std::int32_t       p[MaxOrderLpc / 2 + 1];
        std::int32_t       q[MaxOrderLpc / 2 + 1];
        std::int32_t*      pq[2] = {p, q};
        const std::int32_t dd    = d >> 1;
        A2NlsfInit(aQ16, p, q, dd);

        // the roots, alternating between P and Q
        std::int32_t* poly   = p;
        std::int32_t  xlo    = LSFCosTab_FIX_Q12[0];
        std::int32_t  ylo    = A2NlsfEvalPoly(poly, xlo, dd);
        std::int32_t  rootIx = 0;
        if (ylo < 0) {
            // the first NLSF zero, on to the next
            nlsf[0] = 0;
            poly    = q;
            ylo     = A2NlsfEvalPoly(poly, xlo, dd);
            rootIx  = 1;
        }
        std::int32_t k   = 1; // the loop counter
        std::int32_t i   = 0; // the bandwidth expansions applied
        std::int32_t thr = 0;
        for (;;) {
            std::int32_t xhi = LSFCosTab_FIX_Q12[k];
            std::int32_t yhi = A2NlsfEvalPoly(poly, xhi, dd);

            // a zero crossing
            if ((ylo <= 0 && yhi >= thr) || (ylo >= 0 && yhi <= -thr)) {
                // a root exactly at the end of the interval: the next root in the next interval
                thr = yhi == 0 ? 1 : 0;
                // binary division
                std::int32_t ffrac = -256;
                for (std::int32_t m = 0; m < BinDivStepsA2Nlsf; ++m) {
                    const std::int32_t xmid = RshiftRound(xlo + xhi, 1);
                    const std::int32_t ymid = A2NlsfEvalPoly(poly, xmid, dd);
                    if ((ylo <= 0 && ymid >= 0) || (ylo >= 0 && ymid <= 0)) {
                        // a lower frequency
                        xhi = xmid;
                        yhi = ymid;
                    } else {
                        // a higher frequency
                        xlo   = xmid;
                        ylo   = ymid;
                        ffrac = AddRshift(ffrac, 128, m);
                    }
                }
                // interpolated
                if (Abs(ylo) < 65536) {
                    // no division by zero
                    const std::int32_t den = ylo - yhi;
                    const std::int32_t nom = Lshift(ylo, 8 - BinDivStepsA2Nlsf) + (den >> 1);
                    if (den != 0)
                        ffrac += Div32(nom, den);
                } else {
                    // abs(ylo - yhi) >= abs(ylo) >= 65536: no division by zero
                    ffrac += Div32(ylo, (ylo - yhi) >> (8 - BinDivStepsA2Nlsf));
                }
                nlsf[rootIx] = static_cast<std::int16_t>(Min32(Lshift(k, 8) + ffrac, Int16Max));
                ++rootIx;
                if (rootIx >= d)
                    break; // all roots found
                // the other polynomial
                poly = pq[rootIx & 1];
                xlo  = LSFCosTab_FIX_Q12[k - 1];
                ylo  = Lshift(1 - (rootIx & 2), 12);
            } else {
                ++k;
                xlo = xhi;
                ylo = yhi;
                thr = 0;
                if (k > LsfCosTabSzFix) {
                    ++i;
                    if (i > MaxIterationsA2Nlsf) {
                        // the NLSFs of a white spectrum
                        nlsf[0] = static_cast<std::int16_t>(Div32_16(1 << 15, d + 1));
                        for (k = 1; k < d; ++k)
                            nlsf[k] = static_cast<std::int16_t>(nlsf[k - 1] + nlsf[0]);
                        return;
                    }
                    // an error: progressively more bandwidth expansion, run again
                    Bwexpander32(aQ16, d, 65536 - Lshift(1, i));
                    A2NlsfInit(aQ16, p, q, dd);
                    poly = p;
                    xlo  = LSFCosTab_FIX_Q12[0];
                    ylo  = A2NlsfEvalPoly(poly, xlo, dd);
                    if (ylo < 0) {
                        nlsf[0] = 0;
                        poly    = q;
                        ylo     = A2NlsfEvalPoly(poly, xlo, dd);
                        rootIx  = 1;
                    } else {
                        rootIx = 0;
                    }
                    k = 1;
                }
            }
        }
    }

    void Nlsf2A(std::int16_t* aQ12, const std::int16_t* nlsf, std::int32_t d) noexcept
    {
        // an ordering that maximises the numerical accuracy of Nlsf2AFindPoly
        static constexpr std::uint8_t ordering16[16] = {0, 15, 8, 7, 4, 11, 12, 3, 2, 13, 10, 5, 6, 9, 14, 1};
        static constexpr std::uint8_t ordering10[10] = {0, 9, 6, 3, 4, 5, 8, 1, 2, 7};
        const std::uint8_t*           ordering       = d == 16 ? ordering16 : ordering10;

        // the LSFs to 2 * cos(LSF), piecewise linear from the table
        std::int32_t cosLsfQa[MaxOrderLpc];
        for (std::int32_t k = 0; k < d; ++k) {
            // fInt 0..127 (rounded down), fFrac 0..255
            const std::int32_t fInt   = nlsf[k] >> (15 - 7);
            const std::int32_t fFrac  = nlsf[k] - Lshift(fInt, 15 - 7);
            const std::int32_t cosVal = LSFCosTab_FIX_Q12[fInt];
            const std::int32_t delta  = LSFCosTab_FIX_Q12[fInt + 1] - cosVal;
            // linear interpolation
            cosLsfQa[ordering[k]] = RshiftRound(Lshift(cosVal, 8) + Mul(delta, fFrac), 20 - Nlsf2AQa);
        }

        // the even and odd polynomials by convolution
        const std::int32_t dd = d >> 1;
        std::int32_t       p[MaxOrderLpc / 2 + 1];
        std::int32_t       q[MaxOrderLpc / 2 + 1];
        Nlsf2AFindPoly(p, &cosLsfQa[0], dd);
        Nlsf2AFindPoly(q, &cosLsfQa[1], dd);

        // to int32 filter coefficients (QA + 1)
        std::int32_t a32Qa1[MaxOrderLpc];
        for (std::int32_t k = 0; k < dd; ++k) {
            const std::int32_t ptmp = p[k + 1] + p[k];
            const std::int32_t qtmp = q[k + 1] - q[k];
            a32Qa1[k]               = -qtmp - ptmp;
            a32Qa1[d - k - 1]       = qtmp - ptmp;
        }

        // to Q12 int16 coefficients
        LpcFit(aQ12, a32Qa1, 12, Nlsf2AQa + 1, d);
        for (std::int32_t i = 0; LpcInversePredGain(aQ12, d) == 0 && i < MaxLpcStabilizeIterations; ++i) {
            // (too close to) unstable: bandwidth expansion on the unscaled coefficients, again in Q12
            Bwexpander32(a32Qa1, d, 65536 - Lshift(2, i));
            for (std::int32_t k = 0; k < d; ++k)
                aQ12[k] = static_cast<std::int16_t>(RshiftRound(a32Qa1[k], Nlsf2AQa + 1 - 12));
        }
    }

    void Interpolate(std::int16_t xi[], const std::int16_t x0[], const std::int16_t x1[], std::int32_t ifactQ2, std::int32_t d) noexcept
    {
        for (std::int32_t i = 0; i < d; ++i)
            xi[i] = static_cast<std::int16_t>(AddRshift(x0[i], Smulbb(x1[i] - x0[i], ifactQ2), 2));
    }

    void ProcessNlsfs(SilkEncoderState* enc, std::int16_t predCoefQ12[2][SilkMaxLpcOrder], std::int16_t nlsfQ15[SilkMaxLpcOrder],
                      const std::int16_t prevNlsfqQ15[SilkMaxLpcOrder]) noexcept
    {
        // the mu values: NLSF_mu = 0.003 - 0.0015 * speech_activity
        std::int32_t nlsfMuQ20 = Smlawb(FixConst(0.003, 20), FixConst(-0.001, 28), enc->speech_activity_Q8);
        if (enc->nb_subfr == 2) {
            // times 1.5 for 10 ms packets
            nlsfMuQ20 = AddRshift(nlsfMuQ20, nlsfMuQ20, 1);
        }

        // the NLSF weights
        std::int16_t pNlsfwQw[SilkMaxLpcOrder];
        NlsfVqWeightsLaroia(pNlsfwQw, nlsfQ15, enc->predictLPCOrder);

        // the weights updated for interpolated NLSFs
        const bool   doInterpolate = enc->useInterpolatedNLSFs == 1 && enc->indices.NLSFInterpCoef_Q2 < 4;
        std::int16_t pNlsf0TempQ15[SilkMaxLpcOrder];
        if (doInterpolate) {
            // the interpolated NLSF vector of the first half, its weights
            Interpolate(pNlsf0TempQ15, prevNlsfqQ15, nlsfQ15, enc->indices.NLSFInterpCoef_Q2, enc->predictLPCOrder);
            std::int16_t pNlsfw0TempQw[SilkMaxLpcOrder];
            NlsfVqWeightsLaroia(pNlsfw0TempQw, pNlsf0TempQ15, enc->predictLPCOrder);
            // the contribution of the first half
            const std::int16_t iSqrQ15 = static_cast<std::int16_t>(Lshift(Smulbb(enc->indices.NLSFInterpCoef_Q2, enc->indices.NLSFInterpCoef_Q2), 11));
            for (std::int32_t i = 0; i < enc->predictLPCOrder; ++i)
                pNlsfwQw[i] = static_cast<std::int16_t>((pNlsfwQw[i] >> 1) + (Smulbb(pNlsfw0TempQw[i], iSqrQ15) >> 16));
        }

        NlsfEncode(enc->indices.NLSFIndices, nlsfQ15, enc->psNLSF_CB, pNlsfwQw, nlsfMuQ20, enc->NLSF_MSVQ_Survivors, enc->indices.signalType);

        // the quantised NLSFs back to LPC coefficients
        Nlsf2A(predCoefQ12[1], nlsfQ15, enc->predictLPCOrder);
        if (doInterpolate) {
            // the interpolated, quantised NLSF vector of the first half
            Interpolate(pNlsf0TempQ15, prevNlsfqQ15, nlsfQ15, enc->indices.NLSFInterpCoef_Q2, enc->predictLPCOrder);
            Nlsf2A(predCoefQ12[0], pNlsf0TempQ15, enc->predictLPCOrder);
        } else {
            // the first half copied from the second
            std::memcpy(predCoefQ12[0], predCoefQ12[1], enc->predictLPCOrder * sizeof(std::int16_t));
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk
