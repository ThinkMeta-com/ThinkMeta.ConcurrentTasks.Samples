// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "SilkCoding.h"

#include <cstring>

#include "SilkDefines.h"
#include "SilkMacros.h"
#include "SilkSigProc.h"
#include "SilkTables.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk {

    namespace {

        // gain_quant.c
        constexpr std::int32_t GainOffset      = (MinQgainDb * 128) / 6 + 16 * 128;
        constexpr std::int32_t GainScaleQ16    = (65536 * (NLevelsQgain - 1)) / (((MaxQgainDb - MinQgainDb) * 128) / 6);
        constexpr std::int32_t GainInvScaleQ16 = (65536 * (((MaxQgainDb - MinQgainDb) * 128) / 6)) / (NLevelsQgain - 1);

        // pairwise sums of the pulses; false if a sum exceeds maxPulses
        bool CombineAndCheck(std::int32_t* pulsesComb, const std::int32_t* pulsesIn, std::int32_t maxPulses, std::int32_t len) noexcept
        {
            for (std::int32_t k = 0; k < len; ++k) {
                const std::int32_t sum = pulsesIn[2 * k] + pulsesIn[2 * k + 1];
                if (sum > maxPulses)
                    return false;
                pulsesComb[k] = sum;
            }
            return true;
        }

        void CombinePulses(std::int32_t* out, const std::int32_t* in, std::int32_t len) noexcept
        {
            for (std::int32_t k = 0; k < len; ++k)
                out[k] = in[2 * k] + in[2 * k + 1];
        }

        void EncodeSplit(RangeEncoder* rangeEnc, std::int32_t pChild1, std::int32_t p, const std::uint8_t* shellTable) noexcept
        {
            if (p > 0)
                rangeEnc->EncodeIcdf(pChild1, &shellTable[shell_code_table_offsets[p]], 8);
        }

        // silk_shell_encoder: one shell code frame of 16 non-negative pulse amplitudes
        void ShellEncoder(RangeEncoder* rangeEnc, const std::int32_t* pulses0) noexcept
        {
            // the tree representation
            std::int32_t pulses1[8];
            std::int32_t pulses2[4];
            std::int32_t pulses3[2];
            std::int32_t pulses4[1];
            CombinePulses(pulses1, pulses0, 8);
            CombinePulses(pulses2, pulses1, 4);
            CombinePulses(pulses3, pulses2, 2);
            CombinePulses(pulses4, pulses3, 1);

            EncodeSplit(rangeEnc, pulses3[0], pulses4[0], shell_code_table3);
            EncodeSplit(rangeEnc, pulses2[0], pulses3[0], shell_code_table2);
            EncodeSplit(rangeEnc, pulses1[0], pulses2[0], shell_code_table1);
            EncodeSplit(rangeEnc, pulses0[0], pulses1[0], shell_code_table0);
            EncodeSplit(rangeEnc, pulses0[2], pulses1[1], shell_code_table0);
            EncodeSplit(rangeEnc, pulses1[2], pulses2[1], shell_code_table1);
            EncodeSplit(rangeEnc, pulses0[4], pulses1[2], shell_code_table0);
            EncodeSplit(rangeEnc, pulses0[6], pulses1[3], shell_code_table0);
            EncodeSplit(rangeEnc, pulses2[2], pulses3[1], shell_code_table2);
            EncodeSplit(rangeEnc, pulses1[4], pulses2[2], shell_code_table1);
            EncodeSplit(rangeEnc, pulses0[8], pulses1[4], shell_code_table0);
            EncodeSplit(rangeEnc, pulses0[10], pulses1[5], shell_code_table0);
            EncodeSplit(rangeEnc, pulses1[6], pulses2[3], shell_code_table1);
            EncodeSplit(rangeEnc, pulses0[12], pulses1[6], shell_code_table0);
            EncodeSplit(rangeEnc, pulses0[14], pulses1[7], shell_code_table0);
        }

        // silk_encode_signs: the signs of the excitation
        void EncodeSigns(RangeEncoder* rangeEnc, const std::int8_t pulses[], std::int32_t length, std::int32_t signalType, std::int32_t quantOffsetType,
                         const std::int32_t sumPulses[MaxNbShellBlocks]) noexcept
        {
            std::uint8_t icdf[2];
            icdf[1]                     = 0;
            const std::int8_t*  q       = pulses;
            const std::uint8_t* icdfPtr = &sign_iCDF[Smulbb(7, AddLshift(quantOffsetType, signalType, 1))];
            length                      = (length + ShellCodecFrameLength / 2) >> Log2ShellCodecFrameLength;
            for (std::int32_t i = 0; i < length; ++i) {
                const std::int32_t p = sumPulses[i];
                if (p > 0) {
                    icdf[0] = icdfPtr[Min(p & 0x1F, 6)];
                    for (std::int32_t j = 0; j < ShellCodecFrameLength; ++j) {
                        // the shift maps negative to 0, positive to 1
                        if (q[j] != 0)
                            rangeEnc->EncodeIcdf((q[j] >> 15) + 1, icdf, 8);
                    }
                }
                q += ShellCodecFrameLength;
            }
        }

    } // namespace

    void NlsfUnpack(std::int16_t ecIx[], std::uint8_t predQ8[], const SilkNlsfCb* nlsfCb, std::int32_t cb1Index) noexcept
    {
        const std::uint8_t* ecSel = &nlsfCb->ec_sel[cb1Index * nlsfCb->order / 2];
        for (std::int32_t i = 0; i < nlsfCb->order; i += 2) {
            const std::uint8_t entry = *ecSel++;
            ecIx[i]                  = static_cast<std::int16_t>(Smulbb((entry >> 1) & 7, 2 * NlsfQuantMaxAmplitude + 1));
            predQ8[i]                = nlsfCb->pred_Q8[i + (entry & 1) * (nlsfCb->order - 1)];
            ecIx[i + 1]              = static_cast<std::int16_t>(Smulbb((entry >> 5) & 7, 2 * NlsfQuantMaxAmplitude + 1));
            predQ8[i + 1]            = nlsfCb->pred_Q8[i + ((entry >> 4) & 1) * (nlsfCb->order - 1) + 1];
        }
    }

    void EncodeIndices(SilkEncoderState* enc, RangeEncoder* rangeEnc, std::int32_t frameIndex, bool encodeLbrr, std::int32_t condCoding) noexcept
    {
        const SilkSideInfoIndices* indices = encodeLbrr ? &enc->indices_LBRR[frameIndex] : &enc->indices;

        // the signal type and the quantiser offset
        const std::int32_t typeOffset = 2 * indices->signalType + indices->quantOffsetType;
        if (encodeLbrr || typeOffset >= 2)
            rangeEnc->EncodeIcdf(typeOffset - 2, type_offset_VAD_iCDF, 8);
        else
            rangeEnc->EncodeIcdf(typeOffset, type_offset_no_VAD_iCDF, 8);

        // the gains: the first sub-frame conditionally, or independently in two stages (the MSBs,
        // then 3 LSBs)
        if (condCoding == CodeConditionally) {
            rangeEnc->EncodeIcdf(indices->GainsIndices[0], delta_gain_iCDF, 8);
        } else {
            rangeEnc->EncodeIcdf(indices->GainsIndices[0] >> 3, gain_iCDF[indices->signalType], 8);
            rangeEnc->EncodeIcdf(indices->GainsIndices[0] & 7, uniform8_iCDF, 8);
        }
        // the remaining sub-frames
        for (std::int32_t i = 1; i < enc->nb_subfr; ++i)
            rangeEnc->EncodeIcdf(indices->GainsIndices[i], delta_gain_iCDF, 8);

        // the NLSFs
        const SilkNlsfCb* cb = enc->psNLSF_CB;
        rangeEnc->EncodeIcdf(indices->NLSFIndices[0], &cb->CB1_iCDF[(indices->signalType >> 1) * cb->nVectors], 8);
        std::int16_t ecIx[SilkMaxLpcOrder];
        std::uint8_t predQ8[SilkMaxLpcOrder];
        NlsfUnpack(ecIx, predQ8, cb, indices->NLSFIndices[0]);
        for (std::int32_t i = 0; i < cb->order; ++i) {
            if (indices->NLSFIndices[i + 1] >= NlsfQuantMaxAmplitude) {
                rangeEnc->EncodeIcdf(2 * NlsfQuantMaxAmplitude, &cb->ec_iCDF[ecIx[i]], 8);
                rangeEnc->EncodeIcdf(indices->NLSFIndices[i + 1] - NlsfQuantMaxAmplitude, NLSF_EXT_iCDF, 8);
            } else if (indices->NLSFIndices[i + 1] <= -NlsfQuantMaxAmplitude) {
                rangeEnc->EncodeIcdf(0, &cb->ec_iCDF[ecIx[i]], 8);
                rangeEnc->EncodeIcdf(-indices->NLSFIndices[i + 1] - NlsfQuantMaxAmplitude, NLSF_EXT_iCDF, 8);
            } else {
                rangeEnc->EncodeIcdf(indices->NLSFIndices[i + 1] + NlsfQuantMaxAmplitude, &cb->ec_iCDF[ecIx[i]], 8);
            }
        }
        // the NLSF interpolation factor
        if (enc->nb_subfr == SilkMaxNbSubfr)
            rangeEnc->EncodeIcdf(indices->NLSFInterpCoef_Q2, NLSF_interpolation_factor_iCDF, 8);

        if (indices->signalType == TypeVoiced) {
            // the pitch lag index: a delta if possible
            bool encodeAbsoluteLagIndex = true;
            if (condCoding == CodeConditionally && enc->ec_prevSignalType == TypeVoiced) {
                std::int32_t deltaLagIndex = indices->lagIndex - enc->ec_prevLagIndex;
                if (deltaLagIndex < -8 || deltaLagIndex > 11) {
                    deltaLagIndex = 0;
                } else {
                    deltaLagIndex          = deltaLagIndex + 9;
                    encodeAbsoluteLagIndex = false;
                }
                rangeEnc->EncodeIcdf(deltaLagIndex, pitch_delta_iCDF, 8);
            }
            if (encodeAbsoluteLagIndex) {
                const std::int32_t pitchHighBits = Div32_16(indices->lagIndex, enc->fs_kHz >> 1);
                const std::int32_t pitchLowBits  = indices->lagIndex - Smulbb(pitchHighBits, enc->fs_kHz >> 1);
                rangeEnc->EncodeIcdf(pitchHighBits, pitch_lag_iCDF, 8);
                rangeEnc->EncodeIcdf(pitchLowBits, enc->pitch_lag_low_bits_iCDF, 8);
            }
            enc->ec_prevLagIndex = indices->lagIndex;

            // the contour index
            rangeEnc->EncodeIcdf(indices->contourIndex, enc->pitch_contour_iCDF, 8);

            // the LTP gains: the periodicity index, the codebook indices
            rangeEnc->EncodeIcdf(indices->PERIndex, LTP_per_index_iCDF, 8);
            for (std::int32_t k = 0; k < enc->nb_subfr; ++k)
                rangeEnc->EncodeIcdf(indices->LTPIndex[k], LTP_gain_iCDF_ptrs[indices->PERIndex], 8);

            // the LTP scaling
            if (condCoding == CodeIndependently)
                rangeEnc->EncodeIcdf(indices->LTP_scaleIndex, LTPscale_iCDF, 8);
        }
        enc->ec_prevSignalType = indices->signalType;

        // the seed
        rangeEnc->EncodeIcdf(indices->Seed, uniform4_iCDF, 8);
    }

    void EncodePulses(RangeEncoder* rangeEnc, std::int32_t signalType, std::int32_t quantOffsetType, std::int8_t pulses[], std::int32_t frameLength) noexcept
    {
        std::int32_t pulsesComb[8] = {};

        // the number of shell blocks
        std::int32_t iter = frameLength >> Log2ShellCodecFrameLength;
        if (iter * ShellCodecFrameLength < frameLength) {
            // only for 10 ms at 12 kHz
            ++iter;
            std::memset(&pulses[frameLength], 0, ShellCodecFrameLength * sizeof(std::int8_t));
        }

        // the absolute values of the pulses
        std::int32_t absPulses[MaxNbShellBlocks * ShellCodecFrameLength];
        for (std::int32_t i = 0; i < iter * ShellCodecFrameLength; ++i)
            absPulses[i] = Abs(static_cast<std::int32_t>(pulses[i]));

        // the sum of the pulses per shell code frame, scaled down until it fits
        std::int32_t  sumPulses[MaxNbShellBlocks];
        std::int32_t  nRshifts[MaxNbShellBlocks];
        std::int32_t* absPulsesPtr = absPulses;
        for (std::int32_t i = 0; i < iter; ++i) {
            nRshifts[i] = 0;
            for (;;) {
                // 1+1 -> 2, 2+2 -> 4, 4+4 -> 8, 8+8 -> 16
                std::int32_t scaleDown = CombineAndCheck(pulsesComb, absPulsesPtr, max_pulses_table[0], 8) ? 0 : 1;
                scaleDown += CombineAndCheck(pulsesComb, pulsesComb, max_pulses_table[1], 4) ? 0 : 1;
                scaleDown += CombineAndCheck(pulsesComb, pulsesComb, max_pulses_table[2], 2) ? 0 : 1;
                scaleDown += CombineAndCheck(&sumPulses[i], pulsesComb, max_pulses_table[3], 1) ? 0 : 1;
                if (scaleDown == 0)
                    break;
                // the quantisation signal downscaled
                ++nRshifts[i];
                for (std::int32_t k = 0; k < ShellCodecFrameLength; ++k)
                    absPulsesPtr[k] = absPulsesPtr[k] >> 1;
            }
            absPulsesPtr += ShellCodecFrameLength;
        }

        // the rate level with the fewest bits for the pulses-per-block information
        std::int32_t minSumBitsQ5   = Int32Max;
        std::int32_t rateLevelIndex = 0;
        for (std::int32_t k = 0; k < NRateLevels - 1; ++k) {
            const std::uint8_t* nBits     = pulses_per_block_BITS_Q5[k];
            std::int32_t        sumBitsQ5 = rate_levels_BITS_Q5[signalType >> 1][k];
            for (std::int32_t i = 0; i < iter; ++i)
                sumBitsQ5 += nRshifts[i] > 0 ? nBits[SilkMaxPulses + 1] : nBits[sumPulses[i]];
            if (sumBitsQ5 < minSumBitsQ5) {
                minSumBitsQ5   = sumBitsQ5;
                rateLevelIndex = k;
            }
        }
        rangeEnc->EncodeIcdf(rateLevelIndex, rate_levels_iCDF[signalType >> 1], 8);

        // the sums of the pulses
        const std::uint8_t* cdf = pulses_per_block_iCDF[rateLevelIndex];
        for (std::int32_t i = 0; i < iter; ++i) {
            if (nRshifts[i] == 0) {
                rangeEnc->EncodeIcdf(sumPulses[i], cdf, 8);
            } else {
                rangeEnc->EncodeIcdf(SilkMaxPulses + 1, cdf, 8);
                for (std::int32_t k = 0; k < nRshifts[i] - 1; ++k)
                    rangeEnc->EncodeIcdf(SilkMaxPulses + 1, pulses_per_block_iCDF[NRateLevels - 1], 8);
                rangeEnc->EncodeIcdf(sumPulses[i], pulses_per_block_iCDF[NRateLevels - 1], 8);
            }
        }

        // the shell coding
        for (std::int32_t i = 0; i < iter; ++i) {
            if (sumPulses[i] > 0)
                ShellEncoder(rangeEnc, &absPulses[i * ShellCodecFrameLength]);
        }

        // the LSBs
        for (std::int32_t i = 0; i < iter; ++i) {
            if (nRshifts[i] > 0) {
                const std::int8_t* p   = &pulses[i * ShellCodecFrameLength];
                const std::int32_t nLs = nRshifts[i] - 1;
                for (std::int32_t k = 0; k < ShellCodecFrameLength; ++k) {
                    const std::int32_t absQ = static_cast<std::int8_t>(Abs(static_cast<std::int32_t>(p[k])));
                    for (std::int32_t j = nLs; j > 0; --j)
                        rangeEnc->EncodeIcdf((absQ >> j) & 1, lsb_iCDF, 8);
                    rangeEnc->EncodeIcdf(absQ & 1, lsb_iCDF, 8);
                }
            }
        }

        // the signs
        EncodeSigns(rangeEnc, pulses, frameLength, signalType, quantOffsetType, sumPulses);
    }

    void GainsQuant(std::int8_t ind[SilkMaxNbSubfr], std::int32_t gainQ16[SilkMaxNbSubfr], std::int8_t* prevInd, bool conditional,
                    std::int32_t nbSubfr) noexcept
    {
        for (std::int32_t k = 0; k < nbSubfr; ++k) {
            // to the log scale, scaled, floor()
            ind[k] = static_cast<std::int8_t>(Smulwb(GainScaleQ16, Lin2Log(gainQ16[k]) - GainOffset));
            // rounded towards the previous quantised gain (hysteresis)
            if (ind[k] < *prevInd)
                ++ind[k];
            ind[k] = static_cast<std::int8_t>(Limit(static_cast<std::int32_t>(ind[k]), 0, NLevelsQgain - 1));

            // the delta indices, limited
            if (k == 0 && !conditional) {
                // the full index
                ind[k]   = static_cast<std::int8_t>(Limit(static_cast<std::int32_t>(ind[k]), *prevInd + MinDeltaGainQuant, NLevelsQgain - 1));
                *prevInd = ind[k];
            } else {
                // the delta index
                ind[k] = static_cast<std::int8_t>(ind[k] - *prevInd);
                // the step size doubled for large gain increases, so that the maximum gain level can be reached
                const std::int32_t doubleStepSizeThreshold = 2 * MaxDeltaGainQuant - NLevelsQgain + *prevInd;
                if (ind[k] > doubleStepSizeThreshold)
                    ind[k] = static_cast<std::int8_t>(doubleStepSizeThreshold + ((ind[k] - doubleStepSizeThreshold + 1) >> 1));
                ind[k] = static_cast<std::int8_t>(Limit(static_cast<std::int32_t>(ind[k]), MinDeltaGainQuant, MaxDeltaGainQuant));
                // the deltas accumulated
                if (ind[k] > doubleStepSizeThreshold) {
                    *prevInd = static_cast<std::int8_t>(*prevInd + Lshift(ind[k], 1) - doubleStepSizeThreshold);
                    *prevInd = static_cast<std::int8_t>(MinInt(*prevInd, NLevelsQgain - 1));
                } else {
                    *prevInd = static_cast<std::int8_t>(*prevInd + ind[k]);
                }
                // shifted to make it non-negative
                ind[k] = static_cast<std::int8_t>(ind[k] - MinDeltaGainQuant);
            }

            // scaled, to the linear scale (3967 = 31 in Q7)
            gainQ16[k] = Log2Lin(Min32(Smulwb(GainInvScaleQ16, *prevInd) + GainOffset, 3967));
        }
    }

    void GainsDequant(std::int32_t gainQ16[SilkMaxNbSubfr], const std::int8_t ind[SilkMaxNbSubfr], std::int8_t* prevInd, bool conditional,
                      std::int32_t nbSubfr) noexcept
    {
        for (std::int32_t k = 0; k < nbSubfr; ++k) {
            if (k == 0 && !conditional) {
                // the gain index goes down at most 16 steps (~21.8 dB)
                *prevInd = static_cast<std::int8_t>(MaxInt(ind[k], *prevInd - 16));
            } else {
                // the delta index, accumulated
                const std::int32_t indTmp                  = ind[k] + MinDeltaGainQuant;
                const std::int32_t doubleStepSizeThreshold = 2 * MaxDeltaGainQuant - NLevelsQgain + *prevInd;
                if (indTmp > doubleStepSizeThreshold)
                    *prevInd = static_cast<std::int8_t>(*prevInd + Lshift(indTmp, 1) - doubleStepSizeThreshold);
                else
                    *prevInd = static_cast<std::int8_t>(*prevInd + indTmp);
            }
            *prevInd = static_cast<std::int8_t>(Limit(static_cast<std::int32_t>(*prevInd), 0, NLevelsQgain - 1));

            // scaled, to the linear scale (3967 = 31 in Q7)
            gainQ16[k] = Log2Lin(Min32(Smulwb(GainInvScaleQ16, *prevInd) + GainOffset, 3967));
        }
    }

    std::int32_t GainsId(const std::int8_t ind[SilkMaxNbSubfr], std::int32_t nbSubfr) noexcept
    {
        std::int32_t gainsId = 0;
        for (std::int32_t k = 0; k < nbSubfr; ++k)
            gainsId = AddLshift32(ind[k], gainsId, 8);
        return gainsId;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk
