// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "SilkPredCoefs.h"

#include <cfloat>
#include <cmath>
#include <cstring>

#include "SilkDefines.h"
#include "SilkMacros.h"
#include "SilkNlsf.h"
#include "SilkSigProc.h"
#include "SilkSigProcFlp.h"
#include "SilkTables.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk {

    namespace {

        constexpr std::int32_t LtpOrder                         = SilkLtpOrder;
        constexpr std::int32_t MaxOrderLpc                      = 24; // SILK_MAX_ORDER_LPC
        constexpr float        MaxPredictionPowerGain           = 1e4f;
        constexpr float        MaxPredictionPowerGainAfterReset = 1e2f;

        // --- LTP --------------------------------------------------------------------------------

        // silk_corrVector_FLP: X' * t
        void CorrVectorFlp(const float* x, const float* t, std::int32_t l, std::int32_t order, float* xt, std::int32_t arch) noexcept
        {
            const float* ptr1 = &x[order - 1]; // the first sample of column 0 of X
            for (std::int32_t lag = 0; lag < order; ++lag) {
                xt[lag] = static_cast<float>(InnerProductFlp(ptr1, t, l, arch));
                --ptr1; // the next column of X
            }
        }

        // silk_corrMatrix_FLP: X' * X
        void CorrMatrixFlp(const float* x, std::int32_t l, std::int32_t order, float* xx, std::int32_t arch) noexcept
        {
            const float* ptr1   = &x[order - 1]; // the first sample of column 0 of X
            double       energy = EnergyFlp(ptr1, l);
            xx[0]               = static_cast<float>(energy);
            for (std::int32_t j = 1; j < order; ++j) {
                energy += ptr1[-j] * ptr1[-j] - ptr1[l - j] * ptr1[l - j];
                xx[j * order + j] = static_cast<float>(energy);
            }
            const float* ptr2 = &x[order - 2]; // the first sample of column 1 of X
            for (std::int32_t lag = 1; lag < order; ++lag) {
                // X[:, 0]' * X[:, lag]
                energy          = InnerProductFlp(ptr1, ptr2, l, arch);
                xx[lag * order] = static_cast<float>(energy);
                xx[lag]         = static_cast<float>(energy);
                // X[:, j]' * X[:, j + lag]
                for (std::int32_t j = 1; j < order - lag; ++j) {
                    energy += ptr1[-j] * ptr2[-j] - ptr1[l - j] * ptr2[l - j];
                    xx[(lag + j) * order + j] = static_cast<float>(energy);
                    xx[j * order + lag + j]   = static_cast<float>(energy);
                }
                --ptr2; // the next column of X
            }
        }

        void ScaleVectorFlp(float* data, float gain, std::int32_t dataSize) noexcept
        {
            for (std::int32_t i = 0; i < dataSize; ++i)
                data[i] *= gain;
        }

        // silk_find_LTP_FLP: the weights of the LTP quantisation
        void FindLtp(float xx[SilkMaxNbSubfr * LtpOrder * LtpOrder], float xX[SilkMaxNbSubfr * LtpOrder], const float rPtr[],
                     const std::int32_t lag[SilkMaxNbSubfr], std::int32_t subfrLength, std::int32_t nbSubfr, std::int32_t arch) noexcept
        {
            float* xXPtr = xX;
            float* xxPtr = xx;
            for (std::int32_t k = 0; k < nbSubfr; ++k) {
                const float* lagPtr = rPtr - (lag[k] + LtpOrder / 2);
                CorrMatrixFlp(lagPtr, subfrLength, LtpOrder, xxPtr, arch);
                CorrVectorFlp(lagPtr, rPtr, subfrLength, LtpOrder, xXPtr, arch);
                const float energy = static_cast<float>(EnergyFlp(rPtr, subfrLength + LtpOrder));
                const float temp   = 1.0f / Max(energy, LtpCorrInvMax * 0.5f * (xxPtr[0] + xxPtr[24]) + 1.0f);
                ScaleVectorFlp(xxPtr, temp, LtpOrder * LtpOrder);
                ScaleVectorFlp(xXPtr, temp, LtpOrder);
                rPtr += subfrLength;
                xxPtr += LtpOrder * LtpOrder;
                xXPtr += LtpOrder;
            }
        }

        // silk_VQ_WMat_EC_c: the entropy constrained, matrix-weighted VQ of one 5-element vector
        void VqWMatEc(std::int8_t* ind, std::int32_t* resNrgQ15, std::int32_t* rateDistQ8, std::int32_t* gainQ7, const std::int32_t* xxQ17,
                      const std::int32_t* xXQ17, const std::int8_t* cbQ7, const std::uint8_t* cbGainQ7, const std::uint8_t* clQ5, std::int32_t subfrLen,
                      std::int32_t maxGainQ7, std::int32_t l) noexcept
        {
            // negated, to the new Q domain
            std::int32_t negXXQ24[5];
            for (std::int32_t i = 0; i < 5; ++i)
                negXXQ24[i] = -Lshift32(xXQ17[i], 7);

            // over the codebook
            *rateDistQ8              = Int32Max;
            *resNrgQ15               = Int32Max;
            const std::int8_t* cbRow = cbQ7;
            // something safe if things go really bad
            *ind = 0;
            for (std::int32_t k = 0; k < l; ++k) {
                const std::int32_t gainTmpQ7 = cbGainQ7[k];
                // the weighted rate: the quantisation error 1 - 2 * xX * cb + cb' * XX * cb
                std::int32_t sum1Q15 = FixConst(1.001, 15);
                // the penalty for a too large gain
                const std::int32_t penalty = Lshift32(Max(gainTmpQ7 - maxGainQ7, 0), 11);

                // the first row of XX
                std::int32_t sum2Q24 = Mla(negXXQ24[0], xxQ17[1], cbRow[1]);
                sum2Q24              = Mla(sum2Q24, xxQ17[2], cbRow[2]);
                sum2Q24              = Mla(sum2Q24, xxQ17[3], cbRow[3]);
                sum2Q24              = Mla(sum2Q24, xxQ17[4], cbRow[4]);
                sum2Q24              = Lshift32(sum2Q24, 1);
                sum2Q24              = Mla(sum2Q24, xxQ17[0], cbRow[0]);
                sum1Q15              = Smlawb(sum1Q15, sum2Q24, cbRow[0]);
                // the second row
                sum2Q24 = Mla(negXXQ24[1], xxQ17[7], cbRow[2]);
                sum2Q24 = Mla(sum2Q24, xxQ17[8], cbRow[3]);
                sum2Q24 = Mla(sum2Q24, xxQ17[9], cbRow[4]);
                sum2Q24 = Lshift32(sum2Q24, 1);
                sum2Q24 = Mla(sum2Q24, xxQ17[6], cbRow[1]);
                sum1Q15 = Smlawb(sum1Q15, sum2Q24, cbRow[1]);
                // the third row
                sum2Q24 = Mla(negXXQ24[2], xxQ17[13], cbRow[3]);
                sum2Q24 = Mla(sum2Q24, xxQ17[14], cbRow[4]);
                sum2Q24 = Lshift32(sum2Q24, 1);
                sum2Q24 = Mla(sum2Q24, xxQ17[12], cbRow[2]);
                sum1Q15 = Smlawb(sum1Q15, sum2Q24, cbRow[2]);
                // the fourth row
                sum2Q24 = Mla(negXXQ24[3], xxQ17[19], cbRow[4]);
                sum2Q24 = Lshift32(sum2Q24, 1);
                sum2Q24 = Mla(sum2Q24, xxQ17[18], cbRow[3]);
                sum1Q15 = Smlawb(sum1Q15, sum2Q24, cbRow[3]);
                // the last row
                sum2Q24 = Lshift32(negXXQ24[4], 1);
                sum2Q24 = Mla(sum2Q24, xxQ17[24], cbRow[4]);
                sum1Q15 = Smlawb(sum1Q15, sum2Q24, cbRow[4]);

                // the best one
                if (sum1Q15 >= 0) {
                    // the residual energy to bits with the high-rate assumption (6 dB = 1 bit/sample)
                    const std::int32_t bitsResQ8 = Smulbb(subfrLen, Lin2Log(sum1Q15 + penalty) - (15 << 7));
                    // the code length component halved ("-1"): slightly better quality
                    const std::int32_t bitsTotQ8 = AddLshift32(bitsResQ8, clQ5[k], 3 - 1);
                    if (bitsTotQ8 <= *rateDistQ8) {
                        *rateDistQ8 = bitsTotQ8;
                        *resNrgQ15  = sum1Q15 + penalty;
                        *ind        = static_cast<std::int8_t>(k);
                        *gainQ7     = gainTmpQ7;
                    }
                }
                cbRow += LtpOrder;
            }
        }

        // silk_quant_LTP_gains: over the codebooks of different rates/distortions, the best one
        void QuantLtpGains(std::int16_t bQ14[SilkMaxNbSubfr * LtpOrder], std::int8_t cbkIndex[SilkMaxNbSubfr], std::int8_t* periodicityIndex,
                           std::int32_t* sumLogGainQ7, std::int32_t* predGainDbQ7, const std::int32_t xxQ17[], const std::int32_t xXQ17[],
                           std::int32_t subfrLen, std::int32_t nbSubfr) noexcept
        {
            std::int8_t  tempIdx[SilkMaxNbSubfr];
            std::int32_t minRateDistQ7    = Int32Max;
            std::int32_t bestSumLogGainQ7 = 0;
            std::int32_t resNrgQ15        = 0;
            for (std::int32_t k = 0; k < 3; ++k) {
                // a safety margin of the pitch gain control, for state rescaling/rewhitening
                const std::int32_t  gainSafety = FixConst(0.4, 7);
                const std::uint8_t* clPtrQ5    = LTP_gain_BITS_Q5_ptrs[k];
                const std::int8_t*  cbkPtrQ7   = LTP_vq_ptrs_Q7[k];
                const std::uint8_t* cbkGainPtr = LTP_vq_gain_ptrs_Q7[k];
                const std::int32_t  cbkSize    = LTP_vq_sizes[k];
                const std::int32_t* xxPtr      = xxQ17;
                const std::int32_t* xXPtr      = xXQ17;
                resNrgQ15                      = 0;
                std::int32_t rateDistQ7        = 0;
                std::int32_t sumLogGainTmpQ7   = *sumLogGainQ7;
                for (std::int32_t j = 0; j < nbSubfr; ++j) {
                    const std::int32_t maxGainQ7       = Log2Lin((FixConst(MaxSumLogGainDb / 6.0, 7) - sumLogGainTmpQ7) + FixConst(7, 7)) - gainSafety;
                    std::int32_t       resNrgQ15Subfr  = 0;
                    std::int32_t       rateDistQ7Subfr = 0;
                    std::int32_t       gainQ7          = 0;
                    VqWMatEc(&tempIdx[j], &resNrgQ15Subfr, &rateDistQ7Subfr, &gainQ7, xxPtr, xXPtr, cbkPtrQ7, cbkGainPtr, clPtrQ5, subfrLen, maxGainQ7,
                             cbkSize);
                    resNrgQ15       = AddPosSat32(resNrgQ15, resNrgQ15Subfr);
                    rateDistQ7      = AddPosSat32(rateDistQ7, rateDistQ7Subfr);
                    sumLogGainTmpQ7 = Max(0, sumLogGainTmpQ7 + Lin2Log(gainSafety + gainQ7) - FixConst(7, 7));
                    xxPtr += LtpOrder * LtpOrder;
                    xXPtr += LtpOrder;
                }
                if (rateDistQ7 <= minRateDistQ7) {
                    minRateDistQ7     = rateDistQ7;
                    *periodicityIndex = static_cast<std::int8_t>(k);
                    std::memcpy(cbkIndex, tempIdx, nbSubfr * sizeof(std::int8_t));
                    bestSumLogGainQ7 = sumLogGainTmpQ7;
                }
            }

            const std::int8_t* cbkPtrQ7 = LTP_vq_ptrs_Q7[*periodicityIndex];
            for (std::int32_t j = 0; j < nbSubfr; ++j) {
                for (std::int32_t k = 0; k < LtpOrder; ++k)
                    bQ14[j * LtpOrder + k] = static_cast<std::int16_t>(Lshift(cbkPtrQ7[cbkIndex[j] * LtpOrder + k], 7));
            }
            // as in libopus: the residual energy of the last codebook
            resNrgQ15     = nbSubfr == 2 ? resNrgQ15 >> 1 : resNrgQ15 >> 2;
            *sumLogGainQ7 = bestSumLogGainQ7;
            *predGainDbQ7 = Smulbb(-3, Lin2Log(resNrgQ15) - (15 << 7));
        }

        // silk_quant_LTP_gains_FLP
        void QuantLtpGainsFlp(float b[SilkMaxNbSubfr * LtpOrder], std::int8_t cbkIndex[SilkMaxNbSubfr], std::int8_t* periodicityIndex,
                              std::int32_t* sumLogGainQ7, float* predGainDb, const float xx[], const float xX[], std::int32_t subfrLen,
                              std::int32_t nbSubfr) noexcept
        {
            std::int32_t xxQ17[SilkMaxNbSubfr * LtpOrder * LtpOrder];
            std::int32_t xXQ17[SilkMaxNbSubfr * LtpOrder];
            for (std::int32_t i = 0; i < nbSubfr * LtpOrder * LtpOrder; ++i)
                xxQ17[i] = Float2Int(xx[i] * 131072.0f);
            for (std::int32_t i = 0; i < nbSubfr * LtpOrder; ++i)
                xXQ17[i] = Float2Int(xX[i] * 131072.0f);
            std::int16_t bQ14[SilkMaxNbSubfr * LtpOrder];
            std::int32_t predGainDbQ7 = 0;
            QuantLtpGains(bQ14, cbkIndex, periodicityIndex, sumLogGainQ7, &predGainDbQ7, xxQ17, xXQ17, subfrLen, nbSubfr);
            for (std::int32_t i = 0; i < nbSubfr * LtpOrder; ++i)
                b[i] = static_cast<float>(bQ14[i]) * (1.0f / 16384.0f);
            *predGainDb = static_cast<float>(predGainDbQ7) * (1.0f / 128.0f);
        }

        // silk_LTP_scale_ctrl_FLP
        void LtpScaleCtrl(SilkEncoderStateFlp* enc, SilkEncoderControlFlp* ctrl, std::int32_t condCoding) noexcept
        {
            SilkEncoderState* c = &enc->sCmn;
            if (condCoding == CodeIndependently) {
                // only for the first frame of the packet
                std::int32_t roundLoss = c->PacketLoss_perc * c->nFramesPerPacket;
                // LBRR reduces the effective loss; in practice not squared, as losses are not
                // independent, but that still works best; never below 2%
                if (c->LBRR_flag != 0)
                    roundLoss = 2 + Smulbb(roundLoss, roundLoss) / 100;
                const std::int32_t gainLoss = Smulbb(static_cast<std::int16_t>(ctrl->LTPredCodGain), roundLoss);
                c->indices.LTP_scaleIndex   = static_cast<std::int8_t>(gainLoss > Log2Lin(2900 - c->SNR_dB_Q7) ? 1 : 0);
                c->indices.LTP_scaleIndex   = static_cast<std::int8_t>(c->indices.LTP_scaleIndex + (gainLoss > Log2Lin(3900 - c->SNR_dB_Q7) ? 1 : 0));
            } else {
                // the minimum scaling by default
                c->indices.LTP_scaleIndex = 0;
            }
            ctrl->LTP_scale = static_cast<float>(LTPScales_table_Q14[c->indices.LTP_scaleIndex]) / 16384.0f;
        }

        // silk_LTP_analysis_filter_FLP: the LTP residual, scaled by the inverse gains
        void LtpAnalysisFilter(float* ltpRes, const float* x, const float b[LtpOrder * SilkMaxNbSubfr], const std::int32_t pitchL[SilkMaxNbSubfr],
                               const float invGains[SilkMaxNbSubfr], std::int32_t subfrLength, std::int32_t nbSubfr, std::int32_t preLength) noexcept
        {
            const float* xPtr      = x;
            float*       ltpResPtr = ltpRes;
            for (std::int32_t k = 0; k < nbSubfr; ++k) {
                const float* xLagPtr = xPtr - pitchL[k];
                const float  invGain = invGains[k];
                float        btmp[LtpOrder];
                for (std::int32_t i = 0; i < LtpOrder; ++i)
                    btmp[i] = b[k * LtpOrder + i];
                // the LTP analysis FIR filter
                for (std::int32_t i = 0; i < subfrLength + preLength; ++i) {
                    ltpResPtr[i] = xPtr[i];
                    // minus the long-term prediction
                    for (std::int32_t j = 0; j < LtpOrder; ++j)
                        ltpResPtr[i] -= btmp[j] * xLagPtr[LtpOrder / 2 - j];
                    ltpResPtr[i] *= invGain;
                    ++xLagPtr;
                }
                ltpResPtr += subfrLength + preLength;
                xPtr += subfrLength;
            }
        }

        // --- LPC --------------------------------------------------------------------------------

        // silk_burg_modified_FLP: the prediction coefficients from the stacked sub-frames;
        // returns the residual energy
        float BurgModified(float a[], const float x[], float minInvGain, std::int32_t subfrLength, std::int32_t nbSubfr, std::int32_t d,
                           std::int32_t arch) noexcept
        {
            double cFirstRow[MaxOrderLpc];
            double cLastRow[MaxOrderLpc];
            double cAf[MaxOrderLpc + 1];
            double cAb[MaxOrderLpc + 1];
            double af[MaxOrderLpc];

            // the autocorrelations, summed over the sub-frames
            double c0 = EnergyFlp(x, nbSubfr * subfrLength);
            std::memset(cFirstRow, 0, MaxOrderLpc * sizeof(double));
            for (std::int32_t s = 0; s < nbSubfr; ++s) {
                const float* xPtr = x + s * subfrLength;
                for (std::int32_t n = 1; n < d + 1; ++n)
                    cFirstRow[n - 1] += InnerProductFlp(xPtr, xPtr + n, subfrLength - n, arch);
            }
            std::memcpy(cLastRow, cFirstRow, MaxOrderLpc * sizeof(double));

            cAb[0] = cAf[0]       = c0 + FindLpcCondFac * c0 + 1e-9f;
            double invGain        = 1.0f;
            bool   reachedMaxGain = false;
            for (std::int32_t n = 0; n < d; ++n) {
                // the first row of the correlation matrix (without its first element), the last
                // row (without its last element, reversed), C * Af, C * flipud(Af) (reversed)
                for (std::int32_t s = 0; s < nbSubfr; ++s) {
                    const float* xPtr = x + s * subfrLength;
                    double       tmp1 = xPtr[n];
                    double       tmp2 = xPtr[subfrLength - n - 1];
                    for (std::int32_t k = 0; k < n; ++k) {
                        cFirstRow[k] -= xPtr[n] * xPtr[n - k - 1];
                        cLastRow[k] -= xPtr[subfrLength - n - 1] * xPtr[subfrLength - n + k];
                        const double atmp = af[k];
                        tmp1 += xPtr[n - k - 1] * atmp;
                        tmp2 += xPtr[subfrLength - n + k] * atmp;
                    }
                    for (std::int32_t k = 0; k <= n; ++k) {
                        cAf[k] -= tmp1 * xPtr[n - k];
                        cAb[k] -= tmp2 * xPtr[subfrLength - n + k - 1];
                    }
                }
                double tmp1 = cFirstRow[n];
                double tmp2 = cLastRow[n];
                for (std::int32_t k = 0; k < n; ++k) {
                    const double atmp = af[k];
                    tmp1 += cLastRow[n - k - 1] * atmp;
                    tmp2 += cFirstRow[n - k - 1] * atmp;
                }
                cAf[n + 1] = tmp1;
                cAb[n + 1] = tmp2;

                // the numerator and denominator of the next reflection (parcor) coefficient
                double num  = cAb[n + 1];
                double nrgB = cAb[0];
                double nrgF = cAf[0];
                for (std::int32_t k = 0; k < n; ++k) {
                    const double atmp = af[k];
                    num += cAb[n - k] * atmp;
                    nrgB += cAb[k + 1] * atmp;
                    nrgF += cAf[k + 1] * atmp;
                }

                // the next reflection coefficient
                double rc = -2.0 * num / (nrgF + nrgB);

                // the inverse prediction gain
                tmp1 = invGain * (1.0 - rc * rc);
                if (tmp1 <= minInvGain) {
                    // the maximum prediction gain exceeded: exactly hit with the original sign
                    rc = std::sqrt(1.0 - minInvGain / invGain);
                    if (num > 0)
                        rc = -rc;
                    invGain        = minInvGain;
                    reachedMaxGain = true;
                } else {
                    invGain = tmp1;
                }

                // the AR coefficients updated
                for (std::int32_t k = 0; k < (n + 1) >> 1; ++k) {
                    tmp1          = af[k];
                    tmp2          = af[n - k - 1];
                    af[k]         = tmp1 + rc * tmp2;
                    af[n - k - 1] = tmp2 + rc * tmp1;
                }
                af[n] = rc;

                if (reachedMaxGain) {
                    // the remaining coefficients zero
                    for (std::int32_t k = n + 1; k < d; ++k)
                        af[k] = 0.0;
                    break;
                }

                // C * Af and C * Ab updated
                for (std::int32_t k = 0; k <= n + 1; ++k) {
                    tmp1 = cAf[k];
                    cAf[k] += rc * cAb[n - k + 1];
                    cAb[n - k + 1] += rc * tmp1;
                }
            }

            double nrgF = 0.0;
            if (reachedMaxGain) {
                for (std::int32_t k = 0; k < d; ++k)
                    a[k] = static_cast<float>(-af[k]);
                // the energy of the preceding samples subtracted, the residual energy approximated
                for (std::int32_t s = 0; s < nbSubfr; ++s)
                    c0 -= EnergyFlp(x + s * subfrLength, d);
                nrgF = c0 * invGain;
            } else {
                // the residual energy
                nrgF        = cAf[0];
                double tmp1 = 1.0;
                for (std::int32_t k = 0; k < d; ++k) {
                    const double atmp = af[k];
                    nrgF += cAf[k + 1] * atmp;
                    tmp1 += atmp * atmp;
                    a[k] = static_cast<float>(-atmp);
                }
                nrgF -= FindLpcCondFac * c0 * tmp1;
            }
            return static_cast<float>(nrgF);
        }

        // silk_A2NLSF_FLP
        void A2NlsfFlp(std::int16_t* nlsfQ15, const float* pAr, std::int32_t lpcOrder) noexcept
        {
            std::int32_t aFixQ16[SilkMaxLpcOrder];
            for (std::int32_t i = 0; i < lpcOrder; ++i)
                aFixQ16[i] = Float2Int(pAr[i] * 65536.0f);
            A2Nlsf(nlsfQ15, aFixQ16, lpcOrder);
        }

        // silk_NLSF2A_FLP
        void Nlsf2AFlp(float* pAr, const std::int16_t* nlsfQ15, std::int32_t lpcOrder) noexcept
        {
            std::int16_t aFixQ12[SilkMaxLpcOrder];
            Nlsf2A(aFixQ12, nlsfQ15, lpcOrder);
            for (std::int32_t i = 0; i < lpcOrder; ++i)
                pAr[i] = static_cast<float>(aFixQ12[i]) * (1.0f / 4096.0f);
        }

        // silk_find_LPC_FLP: the NLSFs of the LPC analysis, with the NLSF interpolation factor
        void FindLpc(SilkEncoderState* c, std::int16_t nlsfQ15[], const float x[], float minInvGain, std::int32_t arch) noexcept
        {
            const std::int32_t subfrLength = c->subfr_length + c->predictLPCOrder;

            // no interpolation by default
            c->indices.NLSFInterpCoef_Q2 = 4;

            // the Burg AR analysis of the full frame
            float a[SilkMaxLpcOrder];
            float resNrg = BurgModified(a, x, minInvGain, subfrLength, c->nb_subfr, c->predictLPCOrder, arch);

            if (c->useInterpolatedNLSFs != 0 && c->first_frame_after_reset == 0 && c->nb_subfr == SilkMaxNbSubfr) {
                // the optimum of the last 10 ms; its residual energy subtracted here instead of
                // added to the first 10 ms in each iteration below
                float aTmp[SilkMaxLpcOrder];
                resNrg -= BurgModified(aTmp, x + (SilkMaxNbSubfr / 2) * subfrLength, minInvGain, subfrLength, SilkMaxNbSubfr / 2, c->predictLPCOrder, arch);

                // to NLSFs
                A2NlsfFlp(nlsfQ15, aTmp, c->predictLPCOrder);

                // the interpolation index with the lowest residual energy
                float        resNrg2nd = FLT_MAX;
                std::int16_t nlsf0Q15[SilkMaxLpcOrder];
                float        lpcRes[SilkMaxFrameLength + SilkMaxNbSubfr * SilkMaxLpcOrder];
                for (std::int32_t k = 3; k >= 0; --k) {
                    // the interpolated NLSFs of the first half, to LPC
                    Interpolate(nlsf0Q15, c->prev_NLSFq_Q15, nlsfQ15, k, c->predictLPCOrder);
                    Nlsf2AFlp(aTmp, nlsf0Q15, c->predictLPCOrder);

                    // the residual energy with the interpolation
                    LpcAnalysisFilterFlp(lpcRes, aTmp, x, 2 * subfrLength, c->predictLPCOrder);
                    const float resNrgInterp = static_cast<float>(EnergyFlp(lpcRes + c->predictLPCOrder, subfrLength - c->predictLPCOrder)
                                                                  + EnergyFlp(lpcRes + c->predictLPCOrder + subfrLength, subfrLength - c->predictLPCOrder));

                    if (resNrgInterp < resNrg) {
                        // a lower residual energy with interpolation
                        resNrg                       = resNrgInterp;
                        c->indices.NLSFInterpCoef_Q2 = static_cast<std::int8_t>(k);
                    } else if (resNrgInterp > resNrg2nd) {
                        // the residual energies only climb from here
                        break;
                    }
                    resNrg2nd = resNrgInterp;
                }
            }

            // no interpolation: the NLSFs of the full frame AR coefficients
            if (c->indices.NLSFInterpCoef_Q2 == 4)
                A2NlsfFlp(nlsfQ15, a, c->predictLPCOrder);
        }

        // silk_residual_energy_FLP: the residual energies of the sub-frames, each with LPC_order
        // preceding samples
        void ResidualEnergy(float nrgs[SilkMaxNbSubfr], const float x[], float a[2][SilkMaxLpcOrder], const float gains[], std::int32_t subfrLength,
                            std::int32_t nbSubfr, std::int32_t lpcOrder) noexcept
        {
            float              lpcRes[(SilkMaxFrameLength + SilkMaxNbSubfr * SilkMaxLpcOrder) / 2];
            float*             lpcResPtr = lpcRes + lpcOrder;
            const std::int32_t shift     = lpcOrder + subfrLength;

            // the LPC residual of each frame half, the sub-frame energies
            LpcAnalysisFilterFlp(lpcRes, a[0], x + 0 * shift, 2 * shift, lpcOrder);
            nrgs[0] = static_cast<float>(gains[0] * gains[0] * EnergyFlp(lpcResPtr + 0 * shift, subfrLength));
            nrgs[1] = static_cast<float>(gains[1] * gains[1] * EnergyFlp(lpcResPtr + 1 * shift, subfrLength));
            if (nbSubfr == SilkMaxNbSubfr) {
                LpcAnalysisFilterFlp(lpcRes, a[1], x + 2 * shift, 2 * shift, lpcOrder);
                nrgs[2] = static_cast<float>(gains[2] * gains[2] * EnergyFlp(lpcResPtr + 0 * shift, subfrLength));
                nrgs[3] = static_cast<float>(gains[3] * gains[3] * EnergyFlp(lpcResPtr + 1 * shift, subfrLength));
            }
        }

    } // namespace

    void FindPredCoefs(SilkEncoderStateFlp* enc, SilkEncoderControlFlp* ctrl, const float resPitch[], const float x[], std::int32_t condCoding) noexcept
    {
        SilkEncoderState* c = &enc->sCmn;

        // the weights of the weighted least squares
        float invGains[SilkMaxNbSubfr];
        for (std::int32_t i = 0; i < c->nb_subfr; ++i)
            invGains[i] = 1.0f / ctrl->Gains[i];

        float lpcInPre[SilkMaxNbSubfr * SilkMaxLpcOrder + SilkMaxFrameLength];
        if (c->indices.signalType == TypeVoiced) {
            // voiced: the LTP analysis, the quantised LTP gains, the LTP scaling, the LTP residual
            float xxLtp[SilkMaxNbSubfr * LtpOrder * LtpOrder];
            float xXLtp[SilkMaxNbSubfr * LtpOrder];
            FindLtp(xxLtp, xXLtp, resPitch, ctrl->pitchL, c->subfr_length, c->nb_subfr, c->arch);
            QuantLtpGainsFlp(ctrl->LTPCoef, c->indices.LTPIndex, &c->indices.PERIndex, &c->sum_log_gain_Q7, &ctrl->LTPredCodGain, xxLtp, xXLtp, c->subfr_length,
                             c->nb_subfr);
            LtpScaleCtrl(enc, ctrl, condCoding);
            LtpAnalysisFilter(lpcInPre, x - c->predictLPCOrder, ctrl->LTPCoef, ctrl->pitchL, invGains, c->subfr_length, c->nb_subfr, c->predictLPCOrder);
        } else {
            // unvoiced: the signal with prepended sub-frames, scaled by the inverse gains
            const float* xPtr    = x - c->predictLPCOrder;
            float*       xPrePtr = lpcInPre;
            for (std::int32_t i = 0; i < c->nb_subfr; ++i) {
                // silk_scale_copy_vector_FLP
                for (std::int32_t j = 0; j < c->subfr_length + c->predictLPCOrder; ++j)
                    xPrePtr[j] = invGains[i] * xPtr[j];
                xPrePtr += c->subfr_length + c->predictLPCOrder;
                xPtr += c->subfr_length;
            }
            std::memset(ctrl->LTPCoef, 0, c->nb_subfr * LtpOrder * sizeof(float));
            ctrl->LTPredCodGain = 0.0f;
            c->sum_log_gain_Q7  = 0;
        }

        // a limit on the total predictive coding gain
        float minInvGain = 0.0f;
        if (c->first_frame_after_reset != 0) {
            minInvGain = 1.0f / MaxPredictionPowerGainAfterReset;
        } else {
            minInvGain = static_cast<float>(std::pow(2.0, static_cast<double>(ctrl->LTPredCodGain / 3))) / MaxPredictionPowerGain;
            minInvGain /= 0.25f + 0.75f * ctrl->coding_quality;
        }

        // the LPC analysis of the LTP-filtered (voiced) or unfiltered (unvoiced) input; zero
        // NLSFs, so that no junk gets to the state
        std::int16_t nlsfQ15[SilkMaxLpcOrder] = {};
        FindLpc(c, nlsfQ15, lpcInPre, minInvGain, c->arch);

        // the NLSFs quantised (silk_process_NLSFs_FLP)
        std::int16_t predCoefQ12[2][SilkMaxLpcOrder];
        ProcessNlsfs(c, predCoefQ12, nlsfQ15, c->prev_NLSFq_Q15);
        for (std::int32_t j = 0; j < 2; ++j) {
            for (std::int32_t i = 0; i < c->predictLPCOrder; ++i)
                ctrl->PredCoef[j][i] = static_cast<float>(predCoefQ12[j][i]) * (1.0f / 4096.0f);
        }

        // the residual energy with the quantised LPC coefficients
        ResidualEnergy(ctrl->ResNrg, lpcInPre, ctrl->PredCoef, ctrl->Gains, c->subfr_length, c->nb_subfr, c->predictLPCOrder);

        // the NLSFs for the interpolation in the next frame
        std::memcpy(c->prev_NLSFq_Q15, nlsfQ15, sizeof(c->prev_NLSFq_Q15));
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk
