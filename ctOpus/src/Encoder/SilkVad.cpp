// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "SilkVad.h"

#include "SilkDefines.h"
#include "SilkMacros.h"
#include "SilkSigProc.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk {

    namespace {

        // the noise level estimation
        void VadGetNoiseLevels(const std::int32_t pX[SilkVadNBands], SilkVadState* vad) noexcept
        {
            // initially faster smoothing
            std::int32_t minCoef = 0;
            if (vad->counter < 1000) { // 1000 = 20 s
                minCoef = Div32_16(Int16Max, (vad->counter >> 4) + 1);
                ++vad->counter;
            }
            for (std::int32_t k = 0; k < SilkVadNBands; ++k) {
                // the old noise level estimate of the band
                std::int32_t nl = vad->NL[k];
                // plus the bias
                const std::int32_t nrg = AddPosSat32(pX[k], vad->NoiseLevelBias[k]);
                // the inverted energy
                const std::int32_t invNrg = Div32(Int32Max, nrg);
                // less update when the band energy is high
                std::int32_t coef = 0;
                if (nrg > Lshift(nl, 3))
                    coef = VadNoiseLevelSmoothCoefQ16 >> 3;
                else if (nrg < nl)
                    coef = VadNoiseLevelSmoothCoefQ16;
                else
                    coef = Smulwb(Smulww(invNrg, nl), VadNoiseLevelSmoothCoefQ16 << 1);
                coef = MaxInt(coef, minCoef);
                // the smoothed inverse energy
                vad->inv_NL[k] = Smlawb(vad->inv_NL[k], invNrg - vad->inv_NL[k], coef);
                // the noise level by inverting again, with 7 bits of head room
                nl         = Div32(Int32Max, vad->inv_NL[k]);
                nl         = Min(nl, 0x00FFFFFF);
                vad->NL[k] = nl;
            }
        }

    } // namespace

    void VadGetSaQ8(SilkEncoderState* enc, const std::int16_t in[]) noexcept
    {
        // the weights of the tilt measure
        static constexpr std::int32_t tiltWeights[SilkVadNBands] = {30000, 6000, -12000, -12000};
        SilkVadState*                 vad                        = &enc->sVAD;

        // filter and decimate into 4 bands:
        //  0       L      3L       L              3L                             5L
        //          -      --       -              --                             --
        //          8       8       2               4                              4
        //  [0-1 kHz| temp. |1-2 kHz|    2-4 kHz    |            4-8 kHz           |
        // arranged for the minimal (frame_length / 4) extra scratch space while downsampling
        const std::int32_t decimatedFramelength1 = enc->frame_length >> 1;
        const std::int32_t decimatedFramelength2 = enc->frame_length >> 2;
        std::int32_t       decimatedFramelength  = enc->frame_length >> 3;
        std::int32_t       xOffset[SilkVadNBands];
        xOffset[0] = 0;
        xOffset[1] = decimatedFramelength + decimatedFramelength2;
        xOffset[2] = xOffset[1] + decimatedFramelength;
        xOffset[3] = xOffset[2] + decimatedFramelength2;
        // xOffset[3] + decimatedFramelength1 = 5/4 frame_length
        std::int16_t x[SilkMaxFrameLength * 5 / 4];

        // 0-8 kHz to 0-4 kHz and 4-8 kHz
        AnaFiltBank1(in, &vad->AnaState[0], x, &x[xOffset[3]], enc->frame_length);
        // 0-4 kHz to 0-2 kHz and 2-4 kHz
        AnaFiltBank1(x, &vad->AnaState1[0], x, &x[xOffset[2]], decimatedFramelength1);
        // 0-2 kHz to 0-1 kHz and 1-2 kHz
        AnaFiltBank1(x, &vad->AnaState2[0], x, &x[xOffset[1]], decimatedFramelength2);

        // a high-pass filter (differentiator) on the lowest band
        x[decimatedFramelength - 1]   = static_cast<std::int16_t>(x[decimatedFramelength - 1] >> 1);
        const std::int16_t hpStateTmp = x[decimatedFramelength - 1];
        for (std::int32_t i = decimatedFramelength - 1; i > 0; --i) {
            x[i - 1] = static_cast<std::int16_t>(x[i - 1] >> 1);
            x[i]     = static_cast<std::int16_t>(x[i] - x[i - 1]);
        }
        x[0]         = static_cast<std::int16_t>(x[0] - vad->HPstate);
        vad->HPstate = hpStateTmp;

        // the energy of each band
        std::int32_t xnrg[SilkVadNBands];
        std::int32_t sumSquared = 0;
        for (std::int32_t b = 0; b < SilkVadNBands; ++b) {
            // the decimated frame length in the non-uniformly divided bands, in sub-frames
            decimatedFramelength                 = enc->frame_length >> MinInt(SilkVadNBands - b, SilkVadNBands - 1);
            const std::int32_t decSubframeLength = decimatedFramelength >> VadInternalSubframesLog2;
            std::int32_t       decSubframeOffset = 0;
            // starting with the summed energy of the last sub-frame
            xnrg[b] = vad->XnrgSubfr[b];
            for (std::int32_t s = 0; s < VadInternalSubframes; ++s) {
                sumSquared = 0;
                for (std::int32_t i = 0; i < decSubframeLength; ++i) {
                    // less than decSubframeLength * (int16 min / 8)^2: no overflow
                    const std::int32_t xTmp = x[xOffset[b] + i + decSubframeOffset] >> 3;
                    sumSquared              = Smlabb(sumSquared, xTmp, xTmp);
                }
                // the saturated sum; half of the look-ahead sub-frame
                if (s < VadInternalSubframes - 1)
                    xnrg[b] = AddPosSat32(xnrg[b], sumSquared);
                else
                    xnrg[b] = AddPosSat32(xnrg[b], sumSquared >> 1);
                decSubframeOffset += decSubframeLength;
            }
            vad->XnrgSubfr[b] = sumSquared;
        }

        // the noise estimation
        VadGetNoiseLevels(&xnrg[0], vad);

        // the signal-plus-noise to noise ratio
        std::int32_t nrgToNoiseRatioQ8[SilkVadNBands];
        sumSquared             = 0;
        std::int32_t inputTilt = 0;
        for (std::int32_t b = 0; b < SilkVadNBands; ++b) {
            const std::int32_t speechNrg = xnrg[b] - vad->NL[b];
            if (speechNrg > 0) {
                // a division with sufficient resolution
                if ((xnrg[b] & 0xFF800000) == 0)
                    nrgToNoiseRatioQ8[b] = Div32(Lshift(xnrg[b], 8), vad->NL[b] + 1);
                else
                    nrgToNoiseRatioQ8[b] = Div32(xnrg[b], (vad->NL[b] >> 8) + 1);
                // in the log domain
                std::int32_t snrQ7 = Lin2Log(nrgToNoiseRatioQ8[b]) - 8 * 128;
                // the sum of squares, Q14
                sumSquared = Smlabb(sumSquared, snrQ7, snrQ7);
                // the tilt measure; the SNR scaled down for small band speech energies
                if (speechNrg < (static_cast<std::int32_t>(1) << 20))
                    snrQ7 = Smulwb(Lshift(SqrtApprox(speechNrg), 6), snrQ7);
                inputTilt = Smlawb(inputTilt, tiltWeights[b], snrQ7);
            } else {
                nrgToNoiseRatioQ8[b] = 256;
            }
        }

        // the mean of the squares (Q14), the root, in dB (Q7)
        sumSquared                  = Div32_16(sumSquared, SilkVadNBands);
        const std::int32_t pSnrDbQ7 = static_cast<std::int16_t>(3 * SqrtApprox(sumSquared));

        // the speech probability
        std::int32_t saQ15 = SigmQ15(Smulwb(VadSnrFactorQ16, pSnrDbQ7) - VadNegativeOffsetQ5);

        // the frequency tilt
        enc->input_tilt_Q15 = Lshift(SigmQ15(inputTilt) - 16384, 1);

        // the sigmoid output scaled by the power levels: the signal-without-noise energies,
        // higher bands weighted more
        std::int32_t speechNrg = 0;
        for (std::int32_t b = 0; b < SilkVadNBands; ++b)
            speechNrg += (b + 1) * ((xnrg[b] - vad->NL[b]) >> 4);
        if (enc->frame_length == 20 * enc->fs_kHz)
            speechNrg = speechNrg >> 1;
        if (speechNrg <= 0) {
            saQ15 = saQ15 >> 1;
        } else if (speechNrg < 16384) {
            speechNrg = Lshift32(speechNrg, 16);
            speechNrg = SqrtApprox(speechNrg);
            saQ15     = Smulwb(32768 + speechNrg, saQ15);
        }

        // the speech activity in Q8
        enc->speech_activity_Q8 = MinInt(saQ15 >> 7, Uint8Max);

        // the energy level and SNR estimation: the smoothing coefficient
        std::int32_t smoothCoefQ16 = Smulwb(VadSnrSmoothCoefQ18, Smulwb(saQ15, saQ15));
        if (enc->frame_length == 10 * enc->fs_kHz)
            smoothCoefQ16 >>= 1;
        for (std::int32_t b = 0; b < SilkVadNBands; ++b) {
            // the smoothed energy-to-noise ratio of the band
            vad->NrgRatioSmth_Q8[b] = Smlawb(vad->NrgRatioSmth_Q8[b], nrgToNoiseRatioQ8[b] - vad->NrgRatioSmth_Q8[b], smoothCoefQ16);
            // the SNR in dB
            const std::int32_t snrQ7 = 3 * (Lin2Log(vad->NrgRatioSmth_Q8[b]) - 8 * 128);
            // quality = sigmoid(0.25 * (SNR_dB - 16))
            enc->input_quality_bands_Q15[b] = SigmQ15((snrQ7 - 16 * 128) >> 4);
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk
