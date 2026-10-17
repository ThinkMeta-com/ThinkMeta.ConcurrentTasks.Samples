// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "SilkNsq.h"

#include <cstring>

#include "SilkDefines.h"
#include "SilkMacros.h"
#include "SilkTables.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk {

    namespace {

        constexpr std::int32_t LtpOrder            = SilkLtpOrder;
        constexpr std::int32_t NsqLpcBufLength     = SilkNsqLpcBufLength;
        constexpr std::int32_t HarmShapeFirTaps    = 3;
        constexpr std::int32_t QuantLevelAdjustQ10 = 80;
        constexpr std::int32_t DecisionDelay       = 40;
        constexpr std::int32_t LtpMemLength        = LtpMemLengthMs * SilkMaxFsKhz;

        // the parameters of one sub-frame shared by both quantisers
        struct NsqParams {
            const std::int16_t* aQ12;
            const std::int16_t* bQ14;
            const std::int16_t* arShpQ13;
            std::int32_t        lag;
            std::int32_t        harmShapeFirPackedQ14;
            std::int32_t        tiltQ14;
            std::int32_t        lfShpQ14;
            std::int32_t        gainQ16;
            std::int32_t        lambdaQ10;
            std::int32_t        offsetQ10;
        };

        // silk_LPC_analysis_filter: the fixed-point LPC residual (the first d samples zero)
        void LpcAnalysisFilter(std::int16_t* out, const std::int16_t* in, const std::int16_t* b, std::int32_t len, std::int32_t d) noexcept
        {
            for (std::int32_t ix = d; ix < len; ++ix) {
                const std::int16_t* inPtr = &in[ix - 1];
                // wrap-around allowed, so that two wraps can cancel
                std::int32_t out32Q12 = Smulbb(inPtr[0], b[0]);
                for (std::int32_t j = 1; j < d; ++j)
                    out32Q12 = SmlabbOvflw(out32Q12, inPtr[-j], b[j]);
                // the prediction subtracted, to Q0, saturated
                out32Q12 = Sub32Ovflw(Lshift(static_cast<std::int32_t>(inPtr[1]), 12), out32Q12);
                out[ix]  = static_cast<std::int16_t>(Sat16(RshiftRound(out32Q12, 12)));
            }
            std::memset(out, 0, d * sizeof(std::int16_t));
        }

        // silk_noise_shape_quantizer_short_prediction_c
        std::int32_t ShortPrediction(const std::int32_t* buf32, const std::int16_t* coef16, std::int32_t order) noexcept
        {
            // no bias: silk_SMLAWB always rounds to -inf
            std::int32_t out = order >> 1;
            for (std::int32_t j = 0; j < order; ++j)
                out = Smlawb(out, buf32[-j], coef16[j]);
            return out;
        }

        // silk_NSQ_noise_shape_feedback_loop_c
        std::int32_t NoiseShapeFeedbackLoop(const std::int32_t* data0, std::int32_t* data1, const std::int16_t* coef, std::int32_t order) noexcept
        {
            std::int32_t tmp2 = data0[0];
            std::int32_t tmp1 = data1[0];
            data1[0]          = tmp2;
            std::int32_t out  = order >> 1;
            out               = Smlawb(out, tmp2, coef[0]);
            for (std::int32_t j = 2; j < order; j += 2) {
                tmp2         = data1[j - 1];
                data1[j - 1] = tmp1;
                out          = Smlawb(out, tmp1, coef[j - 1]);
                tmp1         = data1[j + 0];
                data1[j + 0] = tmp2;
                out          = Smlawb(out, tmp2, coef[j]);
            }
            data1[order - 1] = tmp1;
            out              = Smlawb(out, tmp1, coef[order - 1]);
            // Q11 -> Q12
            return Lshift32(out, 1);
        }

        // the two quantisation level candidates and their rate-distortion (rd1/rd2 in Q20)
        void QuantCandidates(std::int32_t rQ10, std::int32_t offsetQ10, std::int32_t lambdaQ10, std::int32_t* q1Q10Out, std::int32_t* q2Q10Out,
                             std::int32_t* rd1Out, std::int32_t* rd2Out) noexcept
        {
            std::int32_t q1Q10 = rQ10 - offsetQ10;
            std::int32_t q1Q0  = q1Q10 >> 10;
            if (lambdaQ10 > 2048) {
                // for aggressive RDO the bias becomes more than one pulse
                const std::int32_t rdoOffset = lambdaQ10 / 2 - 512;
                if (q1Q10 > rdoOffset)
                    q1Q0 = (q1Q10 - rdoOffset) >> 10;
                else if (q1Q10 < -rdoOffset)
                    q1Q0 = (q1Q10 + rdoOffset) >> 10;
                else if (q1Q10 < 0)
                    q1Q0 = -1;
                else
                    q1Q0 = 0;
            }
            std::int32_t q2Q10 = 0;
            std::int32_t rd1   = 0;
            std::int32_t rd2   = 0;
            if (q1Q0 > 0) {
                q1Q10 = Lshift(q1Q0, 10) - QuantLevelAdjustQ10;
                q1Q10 = q1Q10 + offsetQ10;
                q2Q10 = q1Q10 + 1024;
                rd1   = Smulbb(q1Q10, lambdaQ10);
                rd2   = Smulbb(q2Q10, lambdaQ10);
            } else if (q1Q0 == 0) {
                q1Q10 = offsetQ10;
                q2Q10 = q1Q10 + (1024 - QuantLevelAdjustQ10);
                rd1   = Smulbb(q1Q10, lambdaQ10);
                rd2   = Smulbb(q2Q10, lambdaQ10);
            } else if (q1Q0 == -1) {
                q2Q10 = offsetQ10;
                q1Q10 = q2Q10 - (1024 - QuantLevelAdjustQ10);
                rd1   = Smulbb(-q1Q10, lambdaQ10);
                rd2   = Smulbb(q2Q10, lambdaQ10);
            } else {
                // q1Q0 < -1
                q1Q10 = Lshift(q1Q0, 10) + QuantLevelAdjustQ10;
                q1Q10 = q1Q10 + offsetQ10;
                q2Q10 = q1Q10 + 1024;
                rd1   = Smulbb(-q1Q10, lambdaQ10);
                rd2   = Smulbb(-q2Q10, lambdaQ10);
            }
            std::int32_t rrQ10 = rQ10 - q1Q10;
            *rd1Out            = Smlabb(rd1, rrQ10, rrQ10);
            rrQ10              = rQ10 - q2Q10;
            *rd2Out            = Smlabb(rd2, rrQ10, rrQ10);
            *q1Q10Out          = q1Q10;
            *q2Q10Out          = q2Q10;
        }

        // the shared input scaling of both quantisers: the input scaled with 1 / gain, the
        // re-whitened LTP state scaled; returns gain_adj_Q16 if the gain changed, else 0
        std::int32_t ScaleInput(const SilkEncoderState* c, SilkNsqState* nsq, const std::int16_t x16[], std::int32_t xScQ10[], const std::int16_t sLtp[],
                                std::int32_t sLtpQ15[], std::int32_t subfr, std::int32_t ltpScaleQ14, const std::int32_t gainsQ16[], std::int32_t lag) noexcept
        {
            std::int32_t       invGainQ31 = Inverse32VarQ(Max(gainsQ16[subfr], 1), 47);
            const std::int32_t invGainQ26 = RshiftRound(invGainQ31, 5);
            for (std::int32_t i = 0; i < c->subfr_length; ++i)
                xScQ10[i] = Smulww(x16[i], invGainQ26);

            // after re-whitening the LTP state is unscaled: scaled with the inverse gain
            if (nsq->rewhite_flag != 0) {
                // the LTP downscaling
                if (subfr == 0)
                    invGainQ31 = Lshift(Smulwb(invGainQ31, ltpScaleQ14), 2);
                for (std::int32_t i = nsq->sLTP_buf_idx - lag - LtpOrder / 2; i < nsq->sLTP_buf_idx; ++i)
                    sLtpQ15[i] = Smulwb(invGainQ31, sLtp[i]);
            }

            // a changing gain
            if (gainsQ16[subfr] == nsq->prev_gain_Q16)
                return 0;
            const std::int32_t gainAdjQ16 = Div32VarQ(nsq->prev_gain_Q16, gainsQ16[subfr], 16);
            // the long-term shaping state scaled
            for (std::int32_t i = nsq->sLTP_shp_buf_idx - c->ltp_mem_length; i < nsq->sLTP_shp_buf_idx; ++i)
                nsq->sLTP_shp_Q14[i] = Smulww(gainAdjQ16, nsq->sLTP_shp_Q14[i]);
            return gainAdjQ16;
        }

        // --- NSQ.c ------------------------------------------------------------------------------

        // silk_nsq_scale_states
        void NsqScaleStates(const SilkEncoderState* c, SilkNsqState* nsq, const std::int16_t x16[], std::int32_t xScQ10[], const std::int16_t sLtp[],
                            std::int32_t sLtpQ15[], std::int32_t subfr, std::int32_t ltpScaleQ14, const std::int32_t gainsQ16[], const std::int32_t pitchL[],
                            std::int32_t signalType) noexcept
        {
            const std::int32_t lag        = pitchL[subfr];
            const std::int32_t gainAdjQ16 = ScaleInput(c, nsq, x16, xScQ10, sLtp, sLtpQ15, subfr, ltpScaleQ14, gainsQ16, lag);
            if (gainsQ16[subfr] == nsq->prev_gain_Q16)
                return;
            // the long-term prediction state scaled
            if (signalType == TypeVoiced && nsq->rewhite_flag == 0) {
                for (std::int32_t i = nsq->sLTP_buf_idx - lag - LtpOrder / 2; i < nsq->sLTP_buf_idx; ++i)
                    sLtpQ15[i] = Smulww(gainAdjQ16, sLtpQ15[i]);
            }
            nsq->sLF_AR_shp_Q14 = Smulww(gainAdjQ16, nsq->sLF_AR_shp_Q14);
            nsq->sDiff_shp_Q14  = Smulww(gainAdjQ16, nsq->sDiff_shp_Q14);
            // the short-term prediction and shaping states scaled
            for (std::int32_t i = 0; i < NsqLpcBufLength; ++i)
                nsq->sLPC_Q14[i] = Smulww(gainAdjQ16, nsq->sLPC_Q14[i]);
            for (std::int32_t i = 0; i < SilkMaxShapeLpcOrder; ++i)
                nsq->sAR2_Q14[i] = Smulww(gainAdjQ16, nsq->sAR2_Q14[i]);
            // the gain saved
            nsq->prev_gain_Q16 = gainsQ16[subfr];
        }

        // silk_noise_shape_quantizer
        void NoiseShapeQuantizer(SilkNsqState* nsq, std::int32_t signalType, const std::int32_t xScQ10[], std::int8_t pulses[], std::int16_t xq[],
                                 std::int32_t sLtpQ15[], const NsqParams& p, std::int32_t length, std::int32_t shapingLpcOrder,
                                 std::int32_t predictLpcOrder) noexcept
        {
            std::int32_t*      shpLagPtr  = &nsq->sLTP_shp_Q14[nsq->sLTP_shp_buf_idx - p.lag + HarmShapeFirTaps / 2];
            std::int32_t*      predLagPtr = &sLtpQ15[nsq->sLTP_buf_idx - p.lag + LtpOrder / 2];
            const std::int32_t gainQ10    = p.gainQ16 >> 6;

            // the short term AR state
            std::int32_t* psLpcQ14 = &nsq->sLPC_Q14[NsqLpcBufLength - 1];
            for (std::int32_t i = 0; i < length; ++i) {
                // the dither
                nsq->rand_seed = Rand(nsq->rand_seed);

                // the short-term prediction
                const std::int32_t lpcPredQ10 = ShortPrediction(psLpcQ14, p.aQ12, predictLpcOrder);

                // the long-term prediction (no bias: silk_SMLAWB always rounds to -inf)
                std::int32_t ltpPredQ13 = 0;
                if (signalType == TypeVoiced) {
                    ltpPredQ13 = 2;
                    for (std::int32_t j = 0; j < LtpOrder; ++j)
                        ltpPredQ13 = Smlawb(ltpPredQ13, predLagPtr[-j], p.bQ14[j]);
                    ++predLagPtr;
                }

                // the noise shape feedback
                std::int32_t nArQ12 = NoiseShapeFeedbackLoop(&nsq->sDiff_shp_Q14, nsq->sAR2_Q14, p.arShpQ13, shapingLpcOrder);
                nArQ12              = Smlawb(nArQ12, nsq->sLF_AR_shp_Q14, p.tiltQ14);
                std::int32_t nLfQ12 = Smulwb(nsq->sLTP_shp_Q14[nsq->sLTP_shp_buf_idx - 1], p.lfShpQ14);
                nLfQ12              = Smlawt(nLfQ12, nsq->sLF_AR_shp_Q14, p.lfShpQ14);

                // the prediction and noise shaping signals combined
                std::int32_t tmp1 = Sub32Ovflw(Lshift32(lpcPredQ10, 2), nArQ12); // Q12
                tmp1              = Sub32Ovflw(tmp1, nLfQ12);                    // Q12
                if (p.lag > 0) {
                    // symmetric, packed FIR coefficients
                    std::int32_t nLtpQ13 = Smulwb(AddSat32(shpLagPtr[0], shpLagPtr[-2]), p.harmShapeFirPackedQ14);
                    nLtpQ13              = Smlawt(nLtpQ13, shpLagPtr[-1], p.harmShapeFirPackedQ14);
                    nLtpQ13              = Lshift(nLtpQ13, 1);
                    ++shpLagPtr;
                    const std::int32_t tmp2 = ltpPredQ13 - nLtpQ13;                // Q13
                    tmp1                    = Add32Ovflw(tmp2, Lshift32(tmp1, 1)); // Q13
                    tmp1                    = RshiftRound(tmp1, 3);                // Q10
                } else {
                    tmp1 = RshiftRound(tmp1, 2); // Q10
                }

                // the residual error, the sign flipped by the dither
                std::int32_t rQ10 = xScQ10[i] - tmp1;
                if (nsq->rand_seed < 0)
                    rQ10 = -rQ10;
                rQ10 = Limit(rQ10, -(31 << 10), 30 << 10);

                // the two quantisation level candidates and their rate-distortion
                std::int32_t q1Q10  = 0;
                std::int32_t q2Q10  = 0;
                std::int32_t rd1Q20 = 0;
                std::int32_t rd2Q20 = 0;
                QuantCandidates(rQ10, p.offsetQ10, p.lambdaQ10, &q1Q10, &q2Q10, &rd1Q20, &rd2Q20);
                if (rd2Q20 < rd1Q20)
                    q1Q10 = q2Q10;
                pulses[i] = static_cast<std::int8_t>(RshiftRound(q1Q10, 10));

                // the excitation, plus the predictions
                std::int32_t excQ14 = Lshift(q1Q10, 4);
                if (nsq->rand_seed < 0)
                    excQ14 = -excQ14;
                const std::int32_t lpcExcQ14 = AddLshift32(excQ14, ltpPredQ13, 1);
                const std::int32_t xqQ14     = Add32Ovflw(lpcExcQ14, Lshift32(lpcPredQ10, 4));

                // xq scaled back to the normal level
                xq[i] = static_cast<std::int16_t>(Sat16(RshiftRound(Smulww(xqQ14, gainQ10), 8)));

                // the states
                ++psLpcQ14;
                *psLpcQ14                                = xqQ14;
                nsq->sDiff_shp_Q14                       = Sub32Ovflw(xqQ14, Lshift32(xScQ10[i], 4));
                const std::int32_t sLfArShpQ14           = Sub32Ovflw(nsq->sDiff_shp_Q14, Lshift32(nArQ12, 2));
                nsq->sLF_AR_shp_Q14                      = sLfArShpQ14;
                nsq->sLTP_shp_Q14[nsq->sLTP_shp_buf_idx] = Sub32Ovflw(sLfArShpQ14, Lshift32(nLfQ12, 2));
                sLtpQ15[nsq->sLTP_buf_idx]               = Lshift(lpcExcQ14, 1);
                ++nsq->sLTP_shp_buf_idx;
                ++nsq->sLTP_buf_idx;

                // the dither depends on the quantised signal
                nsq->rand_seed = Add32Ovflw(nsq->rand_seed, pulses[i]);
            }

            // the LPC synthesis buffer
            std::memcpy(nsq->sLPC_Q14, &nsq->sLPC_Q14[length], NsqLpcBufLength * sizeof(std::int32_t));
        }

        // the parameters of sub-frame k
        NsqParams SubframeParams(std::int32_t k, bool lsfInterpolation, const std::int16_t* predCoefQ12, const std::int16_t* ltpCoefQ14,
                                 const std::int16_t* arQ13, const std::int32_t harmShapeGainQ14[], const std::int32_t tiltQ14[], const std::int32_t lfShpQ14[],
                                 const std::int32_t gainsQ16[], std::int32_t lambdaQ10, std::int32_t offsetQ10, std::int32_t lag) noexcept
        {
            NsqParams p;
            p.aQ12     = &predCoefQ12[((k >> 1) | (lsfInterpolation ? 0 : 1)) * SilkMaxLpcOrder];
            p.bQ14     = &ltpCoefQ14[k * LtpOrder];
            p.arShpQ13 = &arQ13[k * SilkMaxShapeLpcOrder];
            p.lag      = lag;
            // the noise shape parameters
            p.harmShapeFirPackedQ14 = harmShapeGainQ14[k] >> 2;
            p.harmShapeFirPackedQ14 |= Lshift(static_cast<std::int32_t>(harmShapeGainQ14[k] >> 1), 16);
            p.tiltQ14   = tiltQ14[k];
            p.lfShpQ14  = lfShpQ14[k];
            p.gainQ16   = gainsQ16[k];
            p.lambdaQ10 = lambdaQ10;
            p.offsetQ10 = offsetQ10;
            return p;
        }

        // silk_NSQ_c
        void Nsq(const SilkEncoderState* c, SilkNsqState* nsq, SilkSideInfoIndices* indices, const std::int16_t x16[], std::int8_t pulses[],
                 const std::int16_t* predCoefQ12, const std::int16_t ltpCoefQ14[], const std::int16_t arQ13[], const std::int32_t harmShapeGainQ14[],
                 const std::int32_t tiltQ14[], const std::int32_t lfShpQ14[], const std::int32_t gainsQ16[], const std::int32_t pitchL[],
                 std::int32_t lambdaQ10, std::int32_t ltpScaleQ14) noexcept
        {
            nsq->rand_seed = indices->Seed;
            // the unvoiced lag: the previous one, overwritten for voiced
            std::int32_t       lag              = nsq->lagPrev;
            const std::int32_t offsetQ10        = Quantization_Offsets_Q10[indices->signalType >> 1][indices->quantOffsetType];
            const bool         lsfInterpolation = indices->NLSFInterpCoef_Q2 != 4;

            std::int32_t sLtpQ15[LtpMemLength + SilkMaxFrameLength];
            std::int16_t sLtp[LtpMemLength + SilkMaxFrameLength];
            std::int32_t xScQ10[SilkMaxSubFrameLength];

            // the pointers to the start of the sub-frame
            nsq->sLTP_shp_buf_idx = c->ltp_mem_length;
            nsq->sLTP_buf_idx     = c->ltp_mem_length;
            std::int16_t* pxq     = &nsq->xq[c->ltp_mem_length];
            for (std::int32_t k = 0; k < c->nb_subfr; ++k) {
                nsq->rewhite_flag = 0;
                if (indices->signalType == TypeVoiced) {
                    lag = pitchL[k];
                    // re-whitening with the new A coefficients
                    if ((k & (3 - Lshift(lsfInterpolation ? 1 : 0, 1))) == 0) {
                        const std::int16_t* aQ12     = &predCoefQ12[((k >> 1) | (lsfInterpolation ? 0 : 1)) * SilkMaxLpcOrder];
                        const std::int32_t  startIdx = c->ltp_mem_length - lag - c->predictLPCOrder - LtpOrder / 2;
                        LpcAnalysisFilter(&sLtp[startIdx], &nsq->xq[startIdx + k * c->subfr_length], aQ12, c->ltp_mem_length - startIdx, c->predictLPCOrder);
                        nsq->rewhite_flag = 1;
                        nsq->sLTP_buf_idx = c->ltp_mem_length;
                    }
                }
                const NsqParams p = SubframeParams(k, lsfInterpolation, predCoefQ12, ltpCoefQ14, arQ13, harmShapeGainQ14, tiltQ14, lfShpQ14, gainsQ16,
                                                   lambdaQ10, offsetQ10, lag);

                NsqScaleStates(c, nsq, x16, xScQ10, sLtp, sLtpQ15, k, ltpScaleQ14, gainsQ16, pitchL, indices->signalType);
                NoiseShapeQuantizer(nsq, indices->signalType, xScQ10, pulses, pxq, sLtpQ15, p, c->subfr_length, c->shapingLPCOrder, c->predictLPCOrder);

                x16 += c->subfr_length;
                pulses += c->subfr_length;
                pxq += c->subfr_length;
            }

            // lagPrev of the next frame
            nsq->lagPrev = pitchL[c->nb_subfr - 1];

            // the quantised speech and the noise shaping signals saved
            std::memmove(nsq->xq, &nsq->xq[c->frame_length], c->ltp_mem_length * sizeof(std::int16_t));
            std::memmove(nsq->sLTP_shp_Q14, &nsq->sLTP_shp_Q14[c->frame_length], c->ltp_mem_length * sizeof(std::int32_t));
        }

        // --- NSQ_del_dec.c ----------------------------------------------------------------------

        // all fields int32, sLPC_Q14 first: a state is copied from a sample offset on
        struct NsqDelDec {
            std::int32_t sLPC_Q14[SilkMaxSubFrameLength + NsqLpcBufLength];
            std::int32_t RandState[DecisionDelay];
            std::int32_t Q_Q10[DecisionDelay];
            std::int32_t Xq_Q14[DecisionDelay];
            std::int32_t Pred_Q15[DecisionDelay];
            std::int32_t Shape_Q14[DecisionDelay];
            std::int32_t sAR2_Q14[SilkMaxShapeLpcOrder];
            std::int32_t LF_AR_Q14;
            std::int32_t Diff_Q14;
            std::int32_t Seed;
            std::int32_t SeedInit;
            std::int32_t RD_Q10;
        };

        struct NsqSample {
            std::int32_t Q_Q10;
            std::int32_t RD_Q10;
            std::int32_t xq_Q14;
            std::int32_t LF_AR_Q14;
            std::int32_t Diff_Q14;
            std::int32_t sLTP_shp_Q14;
            std::int32_t LPC_exc_Q14;
        };

        // silk_nsq_del_dec_scale_states
        void NsqDelDecScaleStates(const SilkEncoderState* c, SilkNsqState* nsq, NsqDelDec psDelDec[], const std::int16_t x16[], std::int32_t xScQ10[],
                                  const std::int16_t sLtp[], std::int32_t sLtpQ15[], std::int32_t subfr, std::int32_t nStatesDelayedDecision,
                                  std::int32_t ltpScaleQ14, const std::int32_t gainsQ16[], const std::int32_t pitchL[], std::int32_t signalType,
                                  std::int32_t decisionDelay) noexcept
        {
            const std::int32_t lag        = pitchL[subfr];
            const std::int32_t gainAdjQ16 = ScaleInput(c, nsq, x16, xScQ10, sLtp, sLtpQ15, subfr, ltpScaleQ14, gainsQ16, lag);
            if (gainsQ16[subfr] == nsq->prev_gain_Q16)
                return;
            // the long-term prediction state scaled
            if (signalType == TypeVoiced && nsq->rewhite_flag == 0) {
                for (std::int32_t i = nsq->sLTP_buf_idx - lag - LtpOrder / 2; i < nsq->sLTP_buf_idx - decisionDelay; ++i)
                    sLtpQ15[i] = Smulww(gainAdjQ16, sLtpQ15[i]);
            }
            for (std::int32_t k = 0; k < nStatesDelayedDecision; ++k) {
                NsqDelDec* dd = &psDelDec[k];
                // the scalar states
                dd->LF_AR_Q14 = Smulww(gainAdjQ16, dd->LF_AR_Q14);
                dd->Diff_Q14  = Smulww(gainAdjQ16, dd->Diff_Q14);
                // the short-term prediction and shaping states
                for (std::int32_t i = 0; i < NsqLpcBufLength; ++i)
                    dd->sLPC_Q14[i] = Smulww(gainAdjQ16, dd->sLPC_Q14[i]);
                for (std::int32_t i = 0; i < SilkMaxShapeLpcOrder; ++i)
                    dd->sAR2_Q14[i] = Smulww(gainAdjQ16, dd->sAR2_Q14[i]);
                for (std::int32_t i = 0; i < DecisionDelay; ++i) {
                    dd->Pred_Q15[i]  = Smulww(gainAdjQ16, dd->Pred_Q15[i]);
                    dd->Shape_Q14[i] = Smulww(gainAdjQ16, dd->Shape_Q14[i]);
                }
            }
            // the gain saved
            nsq->prev_gain_Q16 = gainsQ16[subfr];
        }

        // silk_noise_shape_quantizer_del_dec: one sub-frame
        void NoiseShapeQuantizerDelDec(SilkNsqState* nsq, NsqDelDec psDelDec[], std::int32_t signalType, const std::int32_t xQ10[], std::int8_t pulses[],
                                       std::int16_t xq[], std::int32_t sLtpQ15[], std::int32_t delayedGainQ10[], const NsqParams& p, std::int32_t length,
                                       std::int32_t subfr, std::int32_t shapingLpcOrder, std::int32_t predictLpcOrder, std::int32_t warpingQ16,
                                       std::int32_t nStatesDelayedDecision, std::int32_t* smplBufIdx, std::int32_t decisionDelay) noexcept
        {
            NsqSample          psSampleState[MaxDelDecStates][2];
            std::int32_t*      shpLagPtr  = &nsq->sLTP_shp_Q14[nsq->sLTP_shp_buf_idx - p.lag + HarmShapeFirTaps / 2];
            std::int32_t*      predLagPtr = &sLtpQ15[nsq->sLTP_buf_idx - p.lag + LtpOrder / 2];
            const std::int32_t gainQ10    = p.gainQ16 >> 6;

            for (std::int32_t i = 0; i < length; ++i) {
                // the calculations common to all states: the long-term prediction (no bias:
                // silk_SMLAWB always rounds to -inf)
                std::int32_t ltpPredQ14 = 0;
                if (signalType == TypeVoiced) {
                    ltpPredQ14 = 2;
                    for (std::int32_t j = 0; j < LtpOrder; ++j)
                        ltpPredQ14 = Smlawb(ltpPredQ14, predLagPtr[-j], p.bQ14[j]);
                    ltpPredQ14 = Lshift(ltpPredQ14, 1); // Q13 -> Q14
                    ++predLagPtr;
                }

                // the long-term shaping: symmetric, packed FIR coefficients
                std::int32_t nLtpQ14 = 0;
                if (p.lag > 0) {
                    nLtpQ14 = Smulwb(AddSat32(shpLagPtr[0], shpLagPtr[-2]), p.harmShapeFirPackedQ14);
                    nLtpQ14 = Smlawt(nLtpQ14, shpLagPtr[-1], p.harmShapeFirPackedQ14);
                    nLtpQ14 = SubLshift32(ltpPredQ14, nLtpQ14, 2); // Q12 -> Q14
                    ++shpLagPtr;
                }

                for (std::int32_t k = 0; k < nStatesDelayedDecision; ++k) {
                    NsqDelDec* dd = &psDelDec[k];
                    NsqSample* ss = psSampleState[k];

                    // the dither
                    dd->Seed = Rand(dd->Seed);

                    // the short-term prediction
                    const std::int32_t* psLpcQ14   = &dd->sLPC_Q14[NsqLpcBufLength - 1 + i];
                    const std::int32_t  lpcPredQ14 = Lshift(ShortPrediction(psLpcQ14, p.aQ12, predictLpcOrder), 4); // Q10 -> Q14

                    // the noise shape feedback: the output of the lowpass section, of the allpass sections
                    std::int32_t tmp2   = Smlawb(dd->Diff_Q14, dd->sAR2_Q14[0], warpingQ16);
                    std::int32_t tmp1   = Smlawb(dd->sAR2_Q14[0], Sub32Ovflw(dd->sAR2_Q14[1], tmp2), warpingQ16);
                    dd->sAR2_Q14[0]     = tmp2;
                    std::int32_t nArQ14 = shapingLpcOrder >> 1;
                    nArQ14              = Smlawb(nArQ14, tmp2, p.arShpQ13[0]);
                    for (std::int32_t j = 2; j < shapingLpcOrder; j += 2) {
                        tmp2                = Smlawb(dd->sAR2_Q14[j - 1], Sub32Ovflw(dd->sAR2_Q14[j + 0], tmp1), warpingQ16);
                        dd->sAR2_Q14[j - 1] = tmp1;
                        nArQ14              = Smlawb(nArQ14, tmp1, p.arShpQ13[j - 1]);
                        tmp1                = Smlawb(dd->sAR2_Q14[j + 0], Sub32Ovflw(dd->sAR2_Q14[j + 1], tmp2), warpingQ16);
                        dd->sAR2_Q14[j + 0] = tmp2;
                        nArQ14              = Smlawb(nArQ14, tmp2, p.arShpQ13[j]);
                    }
                    dd->sAR2_Q14[shapingLpcOrder - 1] = tmp1;
                    nArQ14                            = Smlawb(nArQ14, tmp1, p.arShpQ13[shapingLpcOrder - 1]);
                    nArQ14                            = Lshift(nArQ14, 1);                        // Q11 -> Q12
                    nArQ14                            = Smlawb(nArQ14, dd->LF_AR_Q14, p.tiltQ14); // Q12
                    nArQ14                            = Lshift(nArQ14, 2);                        // Q12 -> Q14

                    std::int32_t nLfQ14 = Smulwb(dd->Shape_Q14[*smplBufIdx], p.lfShpQ14); // Q12
                    nLfQ14              = Smlawt(nLfQ14, dd->LF_AR_Q14, p.lfShpQ14);      // Q12
                    nLfQ14              = Lshift(nLfQ14, 2);                              // Q12 -> Q14

                    // the input minus the prediction plus the noise feedback:
                    // r = x[i] - LTP_pred - LPC_pred + n_AR + n_Tilt + n_LF + n_LTP
                    tmp1              = AddSat32(nArQ14, nLfQ14);        // Q14
                    tmp2              = Add32Ovflw(nLtpQ14, lpcPredQ14); // Q13
                    tmp1              = SubSat32(tmp2, tmp1);            // Q13
                    tmp1              = RshiftRound(tmp1, 4);            // Q10
                    std::int32_t rQ10 = xQ10[i] - tmp1;                  // the residual error Q10

                    // the sign flipped by the dither
                    if (dd->Seed < 0)
                        rQ10 = -rQ10;
                    rQ10 = Limit(rQ10, -(31 << 10), 30 << 10);

                    // the two quantisation level candidates and their rate-distortion
                    std::int32_t q1Q10 = 0;
                    std::int32_t q2Q10 = 0;
                    std::int32_t rd1   = 0;
                    std::int32_t rd2   = 0;
                    QuantCandidates(rQ10, p.offsetQ10, p.lambdaQ10, &q1Q10, &q2Q10, &rd1, &rd2);
                    const std::int32_t rd1Q10 = rd1 >> 10;
                    const std::int32_t rd2Q10 = rd2 >> 10;
                    if (rd1Q10 < rd2Q10) {
                        ss[0].RD_Q10 = dd->RD_Q10 + rd1Q10;
                        ss[1].RD_Q10 = dd->RD_Q10 + rd2Q10;
                        ss[0].Q_Q10  = q1Q10;
                        ss[1].Q_Q10  = q2Q10;
                    } else {
                        ss[0].RD_Q10 = dd->RD_Q10 + rd2Q10;
                        ss[1].RD_Q10 = dd->RD_Q10 + rd1Q10;
                        ss[0].Q_Q10  = q2Q10;
                        ss[1].Q_Q10  = q1Q10;
                    }

                    // the states of the best and the second best quantisation: the quantised
                    // excitation plus the predictions
                    for (std::int32_t n = 0; n < 2; ++n) {
                        std::int32_t excQ14 = Lshift32(ss[n].Q_Q10, 4);
                        if (dd->Seed < 0)
                            excQ14 = -excQ14;
                        const std::int32_t lpcExcQ14   = excQ14 + ltpPredQ14;
                        const std::int32_t xqQ14       = Add32Ovflw(lpcExcQ14, lpcPredQ14);
                        ss[n].Diff_Q14                 = Sub32Ovflw(xqQ14, Lshift32(xQ10[i], 4));
                        const std::int32_t sLfArShpQ14 = Sub32Ovflw(ss[n].Diff_Q14, nArQ14);
                        ss[n].sLTP_shp_Q14             = SubSat32(sLfArShpQ14, nLfQ14);
                        ss[n].LF_AR_Q14                = sLfArShpQ14;
                        ss[n].LPC_exc_Q14              = lpcExcQ14;
                        ss[n].xq_Q14                   = xqQ14;
                    }
                }

                *smplBufIdx = (*smplBufIdx - 1) % DecisionDelay;
                if (*smplBufIdx < 0)
                    *smplBufIdx += DecisionDelay;
                const std::int32_t lastSmpleIdx = (*smplBufIdx + decisionDelay) % DecisionDelay;

                // the winner
                std::int32_t rdMinQ10  = psSampleState[0][0].RD_Q10;
                std::int32_t winnerInd = 0;
                for (std::int32_t k = 1; k < nStatesDelayedDecision; ++k) {
                    if (psSampleState[k][0].RD_Q10 < rdMinQ10) {
                        rdMinQ10  = psSampleState[k][0].RD_Q10;
                        winnerInd = k;
                    }
                }

                // higher RD values of expired states
                const std::int32_t winnerRandState = psDelDec[winnerInd].RandState[lastSmpleIdx];
                for (std::int32_t k = 0; k < nStatesDelayedDecision; ++k) {
                    if (psDelDec[k].RandState[lastSmpleIdx] != winnerRandState) {
                        psSampleState[k][0].RD_Q10 = psSampleState[k][0].RD_Q10 + (Int32Max >> 4);
                        psSampleState[k][1].RD_Q10 = psSampleState[k][1].RD_Q10 + (Int32Max >> 4);
                    }
                }

                // the worst of the first set and the best of the second set
                std::int32_t rdMaxQ10 = psSampleState[0][0].RD_Q10;
                rdMinQ10              = psSampleState[0][1].RD_Q10;
                std::int32_t rdMaxInd = 0;
                std::int32_t rdMinInd = 0;
                for (std::int32_t k = 1; k < nStatesDelayedDecision; ++k) {
                    if (psSampleState[k][0].RD_Q10 > rdMaxQ10) {
                        rdMaxQ10 = psSampleState[k][0].RD_Q10;
                        rdMaxInd = k;
                    }
                    if (psSampleState[k][1].RD_Q10 < rdMinQ10) {
                        rdMinQ10 = psSampleState[k][1].RD_Q10;
                        rdMinInd = k;
                    }
                }

                // a state replaced if the best of the second set beats the worst of the first
                if (rdMinQ10 < rdMaxQ10) {
                    std::memcpy(reinterpret_cast<std::int32_t*>(&psDelDec[rdMaxInd]) + i, reinterpret_cast<const std::int32_t*>(&psDelDec[rdMinInd]) + i,
                                sizeof(NsqDelDec) - i * sizeof(std::int32_t));
                    std::memcpy(&psSampleState[rdMaxInd][0], &psSampleState[rdMinInd][1], sizeof(NsqSample));
                }

                // the samples of the winner to the output and the long-term filter states
                const NsqDelDec* winner = &psDelDec[winnerInd];
                if (subfr > 0 || i >= decisionDelay) {
                    pulses[i - decisionDelay] = static_cast<std::int8_t>(RshiftRound(winner->Q_Q10[lastSmpleIdx], 10));
                    xq[i - decisionDelay]
                        = static_cast<std::int16_t>(Sat16(RshiftRound(Smulww(winner->Xq_Q14[lastSmpleIdx], delayedGainQ10[lastSmpleIdx]), 8)));
                    nsq->sLTP_shp_Q14[nsq->sLTP_shp_buf_idx - decisionDelay] = winner->Shape_Q14[lastSmpleIdx];
                    sLtpQ15[nsq->sLTP_buf_idx - decisionDelay]               = winner->Pred_Q15[lastSmpleIdx];
                }
                ++nsq->sLTP_shp_buf_idx;
                ++nsq->sLTP_buf_idx;

                // the states
                for (std::int32_t k = 0; k < nStatesDelayedDecision; ++k) {
                    NsqDelDec*       dd               = &psDelDec[k];
                    const NsqSample* ss               = &psSampleState[k][0];
                    dd->LF_AR_Q14                     = ss->LF_AR_Q14;
                    dd->Diff_Q14                      = ss->Diff_Q14;
                    dd->sLPC_Q14[NsqLpcBufLength + i] = ss->xq_Q14;
                    dd->Xq_Q14[*smplBufIdx]           = ss->xq_Q14;
                    dd->Q_Q10[*smplBufIdx]            = ss->Q_Q10;
                    dd->Pred_Q15[*smplBufIdx]         = Lshift32(ss->LPC_exc_Q14, 1);
                    dd->Shape_Q14[*smplBufIdx]        = ss->sLTP_shp_Q14;
                    dd->Seed                          = Add32Ovflw(dd->Seed, RshiftRound(ss->Q_Q10, 10));
                    dd->RandState[*smplBufIdx]        = dd->Seed;
                    dd->RD_Q10                        = ss->RD_Q10;
                }
                delayedGainQ10[*smplBufIdx] = gainQ10;
            }

            // the LPC states
            for (std::int32_t k = 0; k < nStatesDelayedDecision; ++k)
                std::memcpy(psDelDec[k].sLPC_Q14, &psDelDec[k].sLPC_Q14[length], NsqLpcBufLength * sizeof(std::int32_t));
        }

        // the final part of the signals of the winner state to the output and the long-term
        // filter states
        std::int32_t CopyWinner(const SilkEncoderState* c, SilkNsqState* nsq, NsqDelDec psDelDec[], std::int8_t pulses[], std::int16_t pxq[],
                                std::int32_t smplBufIdx, std::int32_t decisionDelay, std::int32_t gain, std::int32_t gainShift) noexcept
        {
            std::int32_t rdMinQ10  = psDelDec[0].RD_Q10;
            std::int32_t winnerInd = 0;
            for (std::int32_t i = 1; i < c->nStatesDelayedDecision; ++i) {
                if (psDelDec[i].RD_Q10 < rdMinQ10) {
                    rdMinQ10  = psDelDec[i].RD_Q10;
                    winnerInd = i;
                }
            }
            const NsqDelDec* dd           = &psDelDec[winnerInd];
            std::int32_t     lastSmpleIdx = smplBufIdx + decisionDelay;
            for (std::int32_t i = 0; i < decisionDelay; ++i) {
                lastSmpleIdx = (lastSmpleIdx - 1) % DecisionDelay;
                if (lastSmpleIdx < 0)
                    lastSmpleIdx += DecisionDelay;
                pulses[i - decisionDelay] = static_cast<std::int8_t>(RshiftRound(dd->Q_Q10[lastSmpleIdx], 10));
                pxq[i - decisionDelay]    = static_cast<std::int16_t>(Sat16(RshiftRound(Smulww(dd->Xq_Q14[lastSmpleIdx], gain), gainShift)));
                nsq->sLTP_shp_Q14[nsq->sLTP_shp_buf_idx - decisionDelay + i] = dd->Shape_Q14[lastSmpleIdx];
            }
            return winnerInd;
        }

        // silk_NSQ_del_dec_c
        void NsqDelDecQuant(const SilkEncoderState* c, SilkNsqState* nsq, SilkSideInfoIndices* indices, const std::int16_t x16[], std::int8_t pulses[],
                            const std::int16_t* predCoefQ12, const std::int16_t ltpCoefQ14[], const std::int16_t arQ13[], const std::int32_t harmShapeGainQ14[],
                            const std::int32_t tiltQ14[], const std::int32_t lfShpQ14[], const std::int32_t gainsQ16[], const std::int32_t pitchL[],
                            std::int32_t lambdaQ10, std::int32_t ltpScaleQ14) noexcept
        {
            // the unvoiced lag: the previous one, overwritten for voiced
            std::int32_t lag = nsq->lagPrev;

            // the delayed decision states
            NsqDelDec psDelDec[MaxDelDecStates];
            std::memset(psDelDec, 0, c->nStatesDelayedDecision * sizeof(NsqDelDec));
            for (std::int32_t k = 0; k < c->nStatesDelayedDecision; ++k) {
                NsqDelDec* dd    = &psDelDec[k];
                dd->Seed         = (k + indices->Seed) & 3;
                dd->SeedInit     = dd->Seed;
                dd->RD_Q10       = 0;
                dd->LF_AR_Q14    = nsq->sLF_AR_shp_Q14;
                dd->Diff_Q14     = nsq->sDiff_shp_Q14;
                dd->Shape_Q14[0] = nsq->sLTP_shp_Q14[c->ltp_mem_length - 1];
                std::memcpy(dd->sLPC_Q14, nsq->sLPC_Q14, NsqLpcBufLength * sizeof(std::int32_t));
                std::memcpy(dd->sAR2_Q14, nsq->sAR2_Q14, sizeof(nsq->sAR2_Q14));
            }

            const std::int32_t offsetQ10  = Quantization_Offsets_Q10[indices->signalType >> 1][indices->quantOffsetType];
            std::int32_t       smplBufIdx = 0; // the index of the oldest samples

            // for voiced frames the decision delay below the pitch lag
            std::int32_t decisionDelay = MinInt(DecisionDelay, c->subfr_length);
            if (indices->signalType == TypeVoiced) {
                for (std::int32_t k = 0; k < c->nb_subfr; ++k)
                    decisionDelay = MinInt(decisionDelay, pitchL[k] - LtpOrder / 2 - 1);
            } else if (lag > 0) {
                decisionDelay = MinInt(decisionDelay, lag - LtpOrder / 2 - 1);
            }
            const bool lsfInterpolation = indices->NLSFInterpCoef_Q2 != 4;

            std::int32_t sLtpQ15[LtpMemLength + SilkMaxFrameLength];
            std::int16_t sLtp[LtpMemLength + SilkMaxFrameLength];
            std::int32_t xScQ10[SilkMaxSubFrameLength];
            std::int32_t delayedGainQ10[DecisionDelay];

            // the pointers to the start of the sub-frame
            std::int16_t* pxq     = &nsq->xq[c->ltp_mem_length];
            nsq->sLTP_shp_buf_idx = c->ltp_mem_length;
            nsq->sLTP_buf_idx     = c->ltp_mem_length;
            std::int32_t subfr    = 0;
            for (std::int32_t k = 0; k < c->nb_subfr; ++k) {
                nsq->rewhite_flag = 0;
                if (indices->signalType == TypeVoiced) {
                    lag = pitchL[k];
                    // re-whitening
                    if ((k & (3 - Lshift(lsfInterpolation ? 1 : 0, 1))) == 0) {
                        if (k == 2) {
                            // the delayed decisions reset: the winner, the others penalised
                            const std::int32_t winnerInd = CopyWinner(c, nsq, psDelDec, pulses, pxq, smplBufIdx, decisionDelay, gainsQ16[1], 14);
                            for (std::int32_t i = 0; i < c->nStatesDelayedDecision; ++i) {
                                if (i != winnerInd)
                                    psDelDec[i].RD_Q10 += Int32Max >> 4;
                            }
                            subfr = 0;
                        }
                        // re-whitened with the new A coefficients
                        const std::int16_t* aQ12     = &predCoefQ12[((k >> 1) | (lsfInterpolation ? 0 : 1)) * SilkMaxLpcOrder];
                        const std::int32_t  startIdx = c->ltp_mem_length - lag - c->predictLPCOrder - LtpOrder / 2;
                        LpcAnalysisFilter(&sLtp[startIdx], &nsq->xq[startIdx + k * c->subfr_length], aQ12, c->ltp_mem_length - startIdx, c->predictLPCOrder);
                        nsq->sLTP_buf_idx = c->ltp_mem_length;
                        nsq->rewhite_flag = 1;
                    }
                }
                const NsqParams p = SubframeParams(k, lsfInterpolation, predCoefQ12, ltpCoefQ14, arQ13, harmShapeGainQ14, tiltQ14, lfShpQ14, gainsQ16,
                                                   lambdaQ10, offsetQ10, lag);

                NsqDelDecScaleStates(c, nsq, psDelDec, x16, xScQ10, sLtp, sLtpQ15, k, c->nStatesDelayedDecision, ltpScaleQ14, gainsQ16, pitchL,
                                     indices->signalType, decisionDelay);
                NoiseShapeQuantizerDelDec(nsq, psDelDec, indices->signalType, xScQ10, pulses, pxq, sLtpQ15, delayedGainQ10, p, c->subfr_length, subfr++,
                                          c->shapingLPCOrder, c->predictLPCOrder, c->warping_Q16, c->nStatesDelayedDecision, &smplBufIdx, decisionDelay);

                x16 += c->subfr_length;
                pulses += c->subfr_length;
                pxq += c->subfr_length;
            }

            // the winner, its final signals
            const std::int32_t winnerInd = CopyWinner(c, nsq, psDelDec, pulses, pxq, smplBufIdx, decisionDelay, gainsQ16[c->nb_subfr - 1] >> 6, 8);
            const NsqDelDec*   dd        = &psDelDec[winnerInd];
            indices->Seed                = static_cast<std::int8_t>(dd->SeedInit);
            std::memcpy(nsq->sLPC_Q14, &dd->sLPC_Q14[c->subfr_length], NsqLpcBufLength * sizeof(std::int32_t));
            std::memcpy(nsq->sAR2_Q14, dd->sAR2_Q14, sizeof(dd->sAR2_Q14));

            // the states
            nsq->sLF_AR_shp_Q14 = dd->LF_AR_Q14;
            nsq->sDiff_shp_Q14  = dd->Diff_Q14;
            nsq->lagPrev        = pitchL[c->nb_subfr - 1];

            // the quantised speech signal saved
            std::memmove(nsq->xq, &nsq->xq[c->frame_length], c->ltp_mem_length * sizeof(std::int16_t));
            std::memmove(nsq->sLTP_shp_Q14, &nsq->sLTP_shp_Q14[c->frame_length], c->ltp_mem_length * sizeof(std::int32_t));
        }

    } // namespace

    void NsqWrapper(SilkEncoderStateFlp* enc, SilkEncoderControlFlp* ctrl, SilkSideInfoIndices* indices, SilkNsqState* nsq, std::int8_t pulses[],
                    const float x[]) noexcept
    {
        const SilkEncoderState* c = &enc->sCmn;

        // the noise shape parameters to fixed point
        std::int16_t arQ13[SilkMaxNbSubfr * SilkMaxShapeLpcOrder];
        for (std::int32_t i = 0; i < c->nb_subfr; ++i) {
            for (std::int32_t j = 0; j < c->shapingLPCOrder; ++j)
                arQ13[i * SilkMaxShapeLpcOrder + j] = static_cast<std::int16_t>(Float2Int(ctrl->AR[i * SilkMaxShapeLpcOrder + j] * 8192.0f));
        }
        // LF_shp packs two int16 coefficients per int32 value
        std::int32_t lfShpQ14[SilkMaxNbSubfr];
        std::int32_t tiltQ14[SilkMaxNbSubfr];
        std::int32_t harmShapeGainQ14[SilkMaxNbSubfr];
        for (std::int32_t i = 0; i < c->nb_subfr; ++i) {
            lfShpQ14[i] = Lshift32(Float2Int(ctrl->LF_AR_shp[i] * 16384.0f), 16)
                | static_cast<std::int32_t>(static_cast<std::uint16_t>(Float2Int(ctrl->LF_MA_shp[i] * 16384.0f)));
            tiltQ14[i]          = Float2Int(ctrl->Tilt[i] * 16384.0f);
            harmShapeGainQ14[i] = Float2Int(ctrl->HarmShapeGain[i] * 16384.0f);
        }
        const std::int32_t lambdaQ10 = Float2Int(ctrl->Lambda * 1024.0f);

        // the prediction and coding parameters
        std::int16_t ltpCoefQ14[LtpOrder * SilkMaxNbSubfr];
        for (std::int32_t i = 0; i < c->nb_subfr * LtpOrder; ++i)
            ltpCoefQ14[i] = static_cast<std::int16_t>(Float2Int(ctrl->LTPCoef[i] * 16384.0f));
        std::int16_t predCoefQ12[2][SilkMaxLpcOrder];
        for (std::int32_t j = 0; j < 2; ++j) {
            for (std::int32_t i = 0; i < c->predictLPCOrder; ++i)
                predCoefQ12[j][i] = static_cast<std::int16_t>(Float2Int(ctrl->PredCoef[j][i] * 4096.0f));
        }
        std::int32_t gainsQ16[SilkMaxNbSubfr];
        for (std::int32_t i = 0; i < c->nb_subfr; ++i)
            gainsQ16[i] = Float2Int(ctrl->Gains[i] * 65536.0f);
        const std::int32_t ltpScaleQ14 = indices->signalType == TypeVoiced ? LTPScales_table_Q14[indices->LTP_scaleIndex] : 0;

        // the input to fixed point
        std::int16_t x16[SilkMaxFrameLength];
        for (std::int32_t i = 0; i < c->frame_length; ++i)
            x16[i] = static_cast<std::int16_t>(Float2Int(x[i]));

        if (c->nStatesDelayedDecision > 1 || c->warping_Q16 > 0)
            NsqDelDecQuant(c, nsq, indices, x16, pulses, predCoefQ12[0], ltpCoefQ14, arQ13, harmShapeGainQ14, tiltQ14, lfShpQ14, gainsQ16, ctrl->pitchL,
                           lambdaQ10, ltpScaleQ14);
        else
            Nsq(c, nsq, indices, x16, pulses, predCoefQ12[0], ltpCoefQ14, arQ13, harmShapeGainQ14, tiltQ14, lfShpQ14, gainsQ16, ctrl->pitchL, lambdaQ10,
                ltpScaleQ14);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk
