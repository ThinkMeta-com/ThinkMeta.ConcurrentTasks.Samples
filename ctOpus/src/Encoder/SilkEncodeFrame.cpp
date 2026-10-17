// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "SilkEncodeFrame.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "SilkCoding.h"
#include "SilkDefines.h"
#include "SilkMacros.h"
#include "SilkNoiseShape.h"
#include "SilkNsq.h"
#include "SilkPitchAnalysis.h"
#include "SilkPredCoefs.h"
#include "SilkSigProc.h"
#include "SilkTables.h"
#include "SilkVad.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk {

    namespace {

        constexpr std::int32_t MaxPacketBytes = 1275;

        // silk_process_gains_FLP: the gains limited and quantised, the quantiser offset and the
        // rate/distortion trade-off
        void ProcessGains(SilkEncoderStateFlp* enc, SilkEncoderControlFlp* ctrl, std::int32_t condCoding) noexcept
        {
            SilkShapeStateFlp* shape = &enc->sShape;

            // a gain reduction for a high LTP coding gain
            if (enc->sCmn.indices.signalType == TypeVoiced) {
                const float s = 1.0f - 0.5f * Sigmoid(0.25f * (ctrl->LTPredCodGain - 12.0f));
                for (std::int32_t k = 0; k < enc->sCmn.nb_subfr; ++k)
                    ctrl->Gains[k] *= s;
            }

            // the quantised signal limited: a soft limit on the ratio of residual energy and squared gains
            const float exponent     = 0.33f * (21.0f - enc->sCmn.SNR_dB_Q7 * (1 / 128.0f));
            const float invMaxSqrVal = static_cast<float>(std::pow(2.0, static_cast<double>(exponent)) / enc->sCmn.subfr_length);
            for (std::int32_t k = 0; k < enc->sCmn.nb_subfr; ++k) {
                float gain     = ctrl->Gains[k];
                gain           = static_cast<float>(std::sqrt(static_cast<double>(gain * gain + ctrl->ResNrg[k] * invMaxSqrVal)));
                ctrl->Gains[k] = MinFloat(gain, 32767.0f);
            }

            // the gains for the noise shaping quantisation
            std::int32_t gainsQ16[SilkMaxNbSubfr];
            for (std::int32_t k = 0; k < enc->sCmn.nb_subfr; ++k)
                gainsQ16[k] = static_cast<std::int32_t>(ctrl->Gains[k] * 65536.0f);

            // the unquantised gains and the gain index saved
            std::memcpy(ctrl->GainsUnq_Q16, gainsQ16, enc->sCmn.nb_subfr * sizeof(std::int32_t));
            ctrl->lastGainIndexPrev = shape->LastGainIndex;

            // quantised, back to Q0
            GainsQuant(enc->sCmn.indices.GainsIndices, gainsQ16, &shape->LastGainIndex, condCoding == CodeConditionally, enc->sCmn.nb_subfr);
            for (std::int32_t k = 0; k < enc->sCmn.nb_subfr; ++k)
                ctrl->Gains[k] = gainsQ16[k] / 65536.0f;

            // the quantiser offset of voiced frames: larger for a low LTP coding gain or a high tilt (low-pass)
            if (enc->sCmn.indices.signalType == TypeVoiced)
                enc->sCmn.indices.quantOffsetType = ctrl->LTPredCodGain + enc->sCmn.input_tilt_Q15 * (1.0f / 32768.0f) > 1.0f ? 0 : 1;

            // the quantiser boundary adjustment
            const float quantOffset = Quantization_Offsets_Q10[enc->sCmn.indices.signalType >> 1][enc->sCmn.indices.quantOffsetType] / 1024.0f;
            ctrl->Lambda            = LambdaOffset + LambdaDelayedDecisions * enc->sCmn.nStatesDelayedDecision
                + LambdaSpeechAct * enc->sCmn.speech_activity_Q8 * (1.0f / 256.0f) + LambdaInputQuality * ctrl->input_quality
                + LambdaCodingQuality * ctrl->coding_quality + LambdaQuantOffset * quantOffset;
        }

        // silk_LBRR_encode_FLP: the low bitrate redundancy, with all parameters reused and the
        // excitation at a lower bitrate
        void LbrrEncode(SilkEncoderStateFlp* enc, SilkEncoderControlFlp* ctrl, const float xfw[], std::int32_t condCoding) noexcept
        {
            SilkEncoderState* c = &enc->sCmn;
            if (c->LBRR_enabled == 0 || c->speech_activity_Q8 <= FixConst(LbrrSpeechActivityThres, 8))
                return;
            SilkSideInfoIndices* indicesLbrr = &c->indices_LBRR[c->nFramesEncoded];
            c->LBRR_flags[c->nFramesEncoded] = 1;

            // the noise shaping quantiser state and the indices of the regular encoding
            SilkNsqState nsqLbrr;
            std::memcpy(&nsqLbrr, &c->sNSQ, sizeof(SilkNsqState));
            std::memcpy(indicesLbrr, &c->indices, sizeof(SilkSideInfoIndices));

            // the original gains saved
            float tempGains[SilkMaxNbSubfr];
            std::memcpy(tempGains, ctrl->Gains, c->nb_subfr * sizeof(float));

            if (c->nFramesEncoded == 0 || c->LBRR_flags[c->nFramesEncoded - 1] == 0) {
                // the first frame of the packet or the previous one not LBRR coded
                c->LBRRprevLastGainIndex = enc->sShape.LastGainIndex;
                // the gains increased for the LBRR target rate
                indicesLbrr->GainsIndices[0] = static_cast<std::int8_t>(indicesLbrr->GainsIndices[0] + c->LBRR_GainIncreases);
                indicesLbrr->GainsIndices[0] = static_cast<std::int8_t>(MinInt(indicesLbrr->GainsIndices[0], NLevelsQgain - 1));
            }

            // decoded, so that the gains stay in sync with the decoder; back to Q0
            std::int32_t gainsQ16[SilkMaxNbSubfr];
            GainsDequant(gainsQ16, indicesLbrr->GainsIndices, &c->LBRRprevLastGainIndex, condCoding == CodeConditionally, c->nb_subfr);
            for (std::int32_t k = 0; k < c->nb_subfr; ++k)
                ctrl->Gains[k] = gainsQ16[k] * (1.0f / 65536.0f);

            // the noise shaping quantisation
            NsqWrapper(enc, ctrl, indicesLbrr, &nsqLbrr, c->pulses_LBRR[c->nFramesEncoded], xfw);

            // the original gains restored
            std::memcpy(ctrl->Gains, tempGains, c->nb_subfr * sizeof(float));
        }

    } // namespace

    void EncodeDoVad(SilkEncoderStateFlp* enc, std::int32_t activity) noexcept
    {
        const std::int32_t activityThreshold = FixConst(SpeechActivityDtxThres, 8);
        SilkEncoderState*  c                 = &enc->sCmn;

        // the voice activity detection
        VadGetSaQ8(c, c->inputBuf + 1);
        // the Opus VAD inactive and the SILK VAD active: SILK lowered to just below the threshold
        if (activity == VadNoActivity && c->speech_activity_Q8 >= activityThreshold)
            c->speech_activity_Q8 = activityThreshold - 1;

        // the speech activity to the VAD and DTX flags
        if (c->speech_activity_Q8 < activityThreshold) {
            c->indices.signalType = TypeNoVoiceActivity;
            ++c->noSpeechCounter;
            if (c->noSpeechCounter <= NbSpeechFramesBeforeDtx) {
                c->inDTX = 0;
            } else if (c->noSpeechCounter > MaxConsecutiveDtx + NbSpeechFramesBeforeDtx) {
                c->noSpeechCounter = NbSpeechFramesBeforeDtx;
                c->inDTX           = 0;
            }
            c->VAD_flags[c->nFramesEncoded] = 0;
        } else {
            c->noSpeechCounter              = 0;
            c->inDTX                        = 0;
            c->indices.signalType           = TypeUnvoiced;
            c->VAD_flags[c->nFramesEncoded] = 1;
        }
    }

    std::int32_t EncodeFrame(SilkEncoderStateFlp* enc, std::int32_t* nBytesOut, RangeEncoder* rangeEnc, std::int32_t condCoding, std::int32_t maxBits,
                             bool useCbr) noexcept
    {
        SilkEncoderState*     c = &enc->sCmn;
        SilkEncoderControlFlp ctrl;
        float                 resPitch[2 * SilkMaxFrameLength + LaPitchMax];

        // for CBR 5 bits below the budget are close enough, for VBR up to 25% below the cap
        // after an initial bust
        const std::int32_t bitsMargin = useCbr ? 5 : maxBits / 4;

        c->indices.Seed = static_cast<std::int8_t>(c->frameCounter++ & 3);

        // the pointers aligned with the start of the frame and of the pitch LPC residual
        float* xFrame        = enc->x_buf + c->ltp_mem_length;
        float* resPitchFrame = resPitch + c->ltp_mem_length;

        // smooth bandwidth transitions
        LpVariableCutoff(&c->sLP, c->inputBuf + 1, c->frame_length);

        // the new frame to the front of the input buffer
        Short2FloatArray(xFrame + LaShapeMs * c->fs_kHz, c->inputBuf + 1, c->frame_length);
        // a tiny signal against denormals
        for (std::int32_t i = 0; i < 8; ++i)
            xFrame[LaShapeMs * c->fs_kHz + i * (c->frame_length >> 3)] += (1 - (i & 2)) * 1e-6f;

        if (c->prefillFlag == 0) {
            // the pitch lags and the initial LPC analysis, the noise shape analysis, the linear
            // prediction coefficients (LPC + LTP), the gains, the LBRR
            FindPitchLags(enc, &ctrl, resPitch, xFrame, c->arch);
            NoiseShapeAnalysis(enc, &ctrl, resPitchFrame, xFrame);
            FindPredCoefs(enc, &ctrl, resPitchFrame, xFrame, condCoding);
            ProcessGains(enc, &ctrl, condCoding);
            LbrrEncode(enc, &ctrl, xFrame, condCoding);

            // the loop over quantiser and entropy coding that controls the bitrate
            constexpr std::int32_t maxIter            = 6;
            std::int16_t           gainMultQ8         = static_cast<std::int16_t>(FixConst(1, 8));
            bool                   foundLower         = false;
            bool                   foundUpper         = false;
            std::int32_t           gainsId            = GainsId(c->indices.GainsIndices, c->nb_subfr);
            std::int32_t           gainsIdLower       = -1;
            std::int32_t           gainsIdUpper       = -1;
            std::int32_t           nBits              = 0;
            std::int32_t           nBitsLower         = 0;
            std::int32_t           nBitsUpper         = 0;
            std::int32_t           gainMultLower      = 0;
            std::int32_t           gainMultUpper      = 0;
            std::int8_t            lastGainIndexCopy2 = 0;
            std::int32_t           gainsQ16[SilkMaxNbSubfr];
            bool                   gainLock[SilkMaxNbSubfr]     = {};
            std::int16_t           bestGainMult[SilkMaxNbSubfr] = {};
            std::int32_t           bestSum[SilkMaxNbSubfr]      = {};

            // a part of the input state copied
            const RangeEncoder rangeEncCopy = *rangeEnc;
            RangeEncoder       rangeEncCopy2;
            SilkNsqState       nsqCopy[2];
            std::memcpy(&nsqCopy[0], &c->sNSQ, sizeof(SilkNsqState));
            const std::int8_t  seedCopy             = c->indices.Seed;
            const std::int16_t ecPrevLagIndexCopy   = c->ec_prevLagIndex;
            const std::int32_t ecPrevSignalTypeCopy = c->ec_prevSignalType;
            std::uint8_t       ecBufCopy[MaxPacketBytes];

            for (std::int32_t iter = 0;; ++iter) {
                if (gainsId == gainsIdLower) {
                    nBits = nBitsLower;
                } else if (gainsId == gainsIdUpper) {
                    nBits = nBitsUpper;
                } else {
                    // a part of the input state restored
                    if (iter > 0) {
                        *rangeEnc = rangeEncCopy;
                        std::memcpy(&c->sNSQ, &nsqCopy[0], sizeof(SilkNsqState));
                        c->indices.Seed      = seedCopy;
                        c->ec_prevLagIndex   = ecPrevLagIndexCopy;
                        c->ec_prevSignalType = ecPrevSignalTypeCopy;
                    }

                    // the noise shaping quantisation
                    NsqWrapper(enc, &ctrl, &c->indices, &c->sNSQ, c->pulses, xFrame);

                    if (iter == maxIter && !foundLower)
                        rangeEncCopy2 = *rangeEnc;

                    // the parameters and the excitation
                    EncodeIndices(c, rangeEnc, c->nFramesEncoded, false, condCoding);
                    EncodePulses(rangeEnc, c->indices.signalType, c->indices.quantOffsetType, c->pulses, c->frame_length);
                    nBits = rangeEnc->Tell();

                    // still busted after the last iteration: damage control
                    if (iter == maxIter && !foundLower && nBits > maxBits) {
                        *rangeEnc = rangeEncCopy2;
                        // the gains of the last frame kept
                        enc->sShape.LastGainIndex = ctrl.lastGainIndexPrev;
                        for (std::int32_t i = 0; i < c->nb_subfr; ++i)
                            c->indices.GainsIndices[i] = 4;
                        if (condCoding != CodeConditionally)
                            c->indices.GainsIndices[0] = ctrl.lastGainIndexPrev;
                        c->ec_prevLagIndex   = ecPrevLagIndexCopy;
                        c->ec_prevSignalType = ecPrevSignalTypeCopy;
                        // all pulses cleared
                        for (std::int32_t i = 0; i < c->frame_length; ++i)
                            c->pulses[i] = 0;
                        EncodeIndices(c, rangeEnc, c->nFramesEncoded, false, condCoding);
                        EncodePulses(rangeEnc, c->indices.signalType, c->indices.quantOffsetType, c->pulses, c->frame_length);
                        nBits = rangeEnc->Tell();
                    }

                    if (!useCbr && iter == 0 && nBits <= maxBits)
                        break;
                }

                if (iter == maxIter) {
                    if (foundLower && (gainsId == gainsIdLower || nBits > maxBits)) {
                        // the output state of an earlier iteration that met the bitrate budget restored
                        *rangeEnc = rangeEncCopy2;
                        std::memcpy(rangeEnc->Buffer(), ecBufCopy, rangeEncCopy2.RangeBytes());
                        std::memcpy(&c->sNSQ, &nsqCopy[1], sizeof(SilkNsqState));
                        enc->sShape.LastGainIndex = lastGainIndexCopy2;
                    }
                    break;
                }

                if (nBits > maxBits) {
                    if (!foundLower && iter >= 2) {
                        // the rate/distortion trade-off of the quantiser adjusted, the previous
                        // upper results discarded
                        ctrl.Lambda = MaxFloat(ctrl.Lambda * 1.5f, 1.5f);
                        // less dithering can help to hit the target
                        c->indices.quantOffsetType = 0;
                        foundUpper                 = false;
                        gainsIdUpper               = -1;
                    } else {
                        foundUpper    = true;
                        nBitsUpper    = nBits;
                        gainMultUpper = gainMultQ8;
                        gainsIdUpper  = gainsId;
                    }
                } else if (nBits < maxBits - bitsMargin) {
                    foundLower    = true;
                    nBitsLower    = nBits;
                    gainMultLower = gainMultQ8;
                    if (gainsId != gainsIdLower) {
                        gainsIdLower = gainsId;
                        // a part of the output state copied
                        rangeEncCopy2 = *rangeEnc;
                        std::memcpy(ecBufCopy, rangeEnc->Buffer(), rangeEnc->RangeBytes());
                        std::memcpy(&nsqCopy[1], &c->sNSQ, sizeof(SilkNsqState));
                        lastGainIndexCopy2 = enc->sShape.LastGainIndex;
                    }
                } else {
                    // close enough
                    break;
                }

                if (!foundLower && nBits > maxBits) {
                    for (std::int32_t i = 0; i < c->nb_subfr; ++i) {
                        std::int32_t sum = 0;
                        for (std::int32_t j = i * c->subfr_length; j < (i + 1) * c->subfr_length; ++j)
                            sum += std::abs(c->pulses[j]);
                        if (iter == 0 || (sum < bestSum[i] && !gainLock[i])) {
                            bestSum[i]      = sum;
                            bestGainMult[i] = gainMultQ8;
                        } else {
                            gainLock[i] = true;
                        }
                    }
                }

                if (!(foundLower && foundUpper)) {
                    // the gain adjusted along the high-rate rate/distortion curve
                    if (nBits > maxBits)
                        gainMultQ8 = static_cast<std::int16_t>(Min32(1024, gainMultQ8 * 3 / 2));
                    else
                        gainMultQ8 = static_cast<std::int16_t>(Max32(64, gainMultQ8 * 4 / 5));
                } else {
                    // the gain interpolated
                    gainMultQ8
                        = static_cast<std::int16_t>(gainMultLower + ((gainMultUpper - gainMultLower) * (maxBits - nBitsLower)) / (nBitsUpper - nBitsLower));
                    // between 25% and 75% of the old range (gainMultUpper < gainMultLower)
                    if (gainMultQ8 > AddRshift32(gainMultLower, gainMultUpper - gainMultLower, 2))
                        gainMultQ8 = static_cast<std::int16_t>(AddRshift32(gainMultLower, gainMultUpper - gainMultLower, 2));
                    else if (gainMultQ8 < SubRshift32(gainMultUpper, gainMultUpper - gainMultLower, 2))
                        gainMultQ8 = static_cast<std::int16_t>(SubRshift32(gainMultUpper, gainMultUpper - gainMultLower, 2));
                }

                for (std::int32_t i = 0; i < c->nb_subfr; ++i) {
                    const std::int16_t tmp = gainLock[i] ? bestGainMult[i] : gainMultQ8;
                    gainsQ16[i]            = LshiftSat32(Smulwb(ctrl.GainsUnq_Q16[i], tmp), 8);
                }

                // the gains quantised, their identifier
                enc->sShape.LastGainIndex = ctrl.lastGainIndexPrev;
                GainsQuant(c->indices.GainsIndices, gainsQ16, &enc->sShape.LastGainIndex, condCoding == CodeConditionally, c->nb_subfr);
                gainsId = GainsId(c->indices.GainsIndices, c->nb_subfr);

                // the unquantised gains overwritten with the quantised ones, back to Q0
                for (std::int32_t i = 0; i < c->nb_subfr; ++i)
                    ctrl.Gains[i] = gainsQ16[i] / 65536.0f;
            }
        }

        // the input buffer updated
        std::memmove(enc->x_buf, &enc->x_buf[c->frame_length], (c->ltp_mem_length + LaShapeMs * c->fs_kHz) * sizeof(float));

        // no entropy coding: no payload
        if (c->prefillFlag != 0) {
            *nBytesOut = 0;
            return 0;
        }

        // the parameters of the next frame
        c->prevLag        = ctrl.pitchL[c->nb_subfr - 1];
        c->prevSignalType = c->indices.signalType;

        // the payload
        c->first_frame_after_reset = 0;
        *nBytesOut                 = (rangeEnc->Tell() + 7) >> 3;
        return 0;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk
