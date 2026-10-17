// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "SilkNoiseShape.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "SilkDefines.h"
#include "SilkMacros.h"
#include "SilkSigProcFlp.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk {

    namespace {

        constexpr std::int32_t ShapeLpcWinMax = 15 * SilkMaxFsKhz;

        // the gain that gives warped filter coefficients a zero mean log frequency response on a
        // non-warped frequency scale (a minimum-phase monic filter; SILK omits the first, unit
        // coefficient)
        float WarpedGain(const float* coefs, float lambda, std::int32_t order) noexcept
        {
            lambda     = -lambda;
            float gain = coefs[order - 1];
            for (std::int32_t i = order - 2; i >= 0; --i)
                gain = lambda * gain + coefs[i];
            return 1.0f / (1.0f - lambda * gain);
        }

        // the largest absolute coefficient and its index
        float MaxAbsCoef(const float* coefs, std::int32_t order, std::int32_t* ind) noexcept
        {
            float maxabs = -1.0f;
            for (std::int32_t i = 0; i < order; ++i) {
                const float tmp = AbsFloat(coefs[i]);
                if (tmp > maxabs) {
                    maxabs = tmp;
                    *ind   = i;
                }
            }
            return maxabs;
        }

        // to monic warped coefficients; returns the gain
        float WarpedToMonic(float* coefs, float lambda, std::int32_t order) noexcept
        {
            for (std::int32_t i = order - 1; i > 0; --i)
                coefs[i - 1] -= lambda * coefs[i];
            const float gain = (1.0f - lambda * lambda) / (1.0f + lambda * coefs[0]);
            for (std::int32_t i = 0; i < order; ++i)
                coefs[i] *= gain;
            return gain;
        }

        // warped coefficients to monic pseudo-warped ones, the maximum amplitude limited by
        // bandwidth expansion on the true coefficients
        void WarpedTrue2MonicCoefs(float* coefs, float lambda, float limit, std::int32_t order) noexcept
        {
            float        gain = WarpedToMonic(coefs, lambda, order);
            std::int32_t ind  = 0;
            for (std::int32_t iter = 0; iter < 10; ++iter) {
                const float maxabs = MaxAbsCoef(coefs, order, &ind);
                if (maxabs <= limit)
                    return; // within range
                // back to true warped coefficients
                for (std::int32_t i = 1; i < order; ++i)
                    coefs[i - 1] += lambda * coefs[i];
                gain = 1.0f / gain;
                for (std::int32_t i = 0; i < order; ++i)
                    coefs[i] *= gain;
                // bandwidth expansion
                const float chirp = 0.99f - (0.8f + 0.1f * iter) * (maxabs - limit) / (maxabs * (ind + 1));
                BwexpanderFlp(coefs, order, chirp);
                // to monic warped coefficients
                gain = WarpedToMonic(coefs, lambda, order);
            }
        }

        void LimitCoefs(float* coefs, float limit, std::int32_t order) noexcept
        {
            std::int32_t ind = 0;
            for (std::int32_t iter = 0; iter < 10; ++iter) {
                const float maxabs = MaxAbsCoef(coefs, order, &ind);
                if (maxabs <= limit)
                    return; // within range
                // bandwidth expansion
                const float chirp = 0.99f - (0.8f + 0.1f * iter) * (maxabs - limit) / (maxabs * (ind + 1));
                BwexpanderFlp(coefs, order, chirp);
            }
        }

    } // namespace

    void NoiseShapeAnalysis(SilkEncoderStateFlp* enc, SilkEncoderControlFlp* ctrl, const float* pitchRes, const float* x) noexcept
    {
        SilkShapeStateFlp*      shape = &enc->sShape;
        const SilkEncoderState* c     = &enc->sCmn;

        // the start of the first LPC analysis block
        const float* xPtr = x - c->la_shape;

        // the gain control
        float snrAdjDb = c->SNR_dB_Q7 * (1 / 128.0f);
        // the input quality: the average of the quality in the lowest two VAD bands
        ctrl->input_quality = 0.5f * (c->input_quality_bands_Q15[0] + c->input_quality_bands_Q15[1]) * (1.0f / 32768.0f);
        // the coding quality, between 0.0 and 1.0
        ctrl->coding_quality = Sigmoid(0.25f * (snrAdjDb - 20.0f));
        if (c->useCBR == 0) {
            // less coding SNR at low speech activity
            const float b = 1.0f - c->speech_activity_Q8 * (1.0f / 256.0f);
            snrAdjDb -= BgSnrDecrDb * ctrl->coding_quality * (0.5f + 0.5f * ctrl->input_quality) * b * b;
        }
        if (c->indices.signalType == TypeVoiced) {
            // lower gains for periodic signals
            snrAdjDb += HarmSnrIncrDb * enc->LTPCorr;
        } else {
            // unvoiced and low-quality input: the quality follows the SNR setting more slowly
            snrAdjDb += (-0.4f * c->SNR_dB_Q7 * (1 / 128.0f) + 6.0f) * (1.0f - ctrl->input_quality);
        }

        // the sparseness: the quantiser offset
        if (c->indices.signalType == TypeVoiced) {
            // initially 0, process_gains may overrule it
            enc->sCmn.indices.quantOffsetType = 0;
        } else {
            // a sparseness measure from the relative energy fluctuations per 2 ms
            const std::int32_t nSamples        = 2 * c->fs_kHz;
            float              energyVariation = 0.0f;
            float              logEnergyPrev   = 0.0f;
            const float*       pitchResPtr     = pitchRes;
            const std::int32_t nSegs           = Smulbb(SubFrameLengthMs, c->nb_subfr) / 2;
            for (std::int32_t k = 0; k < nSegs; ++k) {
                const float nrg       = static_cast<float>(nSamples) + static_cast<float>(EnergyFlp(pitchResPtr, nSamples));
                const float logEnergy = Log2(nrg);
                if (k > 0)
                    energyVariation += AbsFloat(logEnergy - logEnergyPrev);
                logEnergyPrev = logEnergy;
                pitchResPtr += nSamples;
            }
            enc->sCmn.indices.quantOffsetType = energyVariation > EnergyVariationThresholdQntOffset * (nSegs - 1) ? 0 : 1;
        }

        // the bandwidth expansion: more for signals with a high prediction gain
        float       strength = FindPitchWhiteNoiseFraction * ctrl->predGain; // between 0.0 and 1.0
        const float bwExp    = BandwidthExpansion / (1.0f + strength * strength);
        // slightly more warping in the analysis moves the quantisation noise up in frequency, where it is better masked
        const float warping = static_cast<float>(c->warping_Q16) / 65536.0f + 0.01f * ctrl->coding_quality;

        // the noise shaping AR coefficients and gains
        float xWindowed[ShapeLpcWinMax];
        float autoCorr[SilkMaxShapeLpcOrder + 1];
        float rc[SilkMaxShapeLpcOrder + 1];
        for (std::int32_t k = 0; k < c->nb_subfr; ++k) {
            // the window: a sine slope, a flat part, a cosine slope
            const std::int32_t flatPart  = c->fs_kHz * 3;
            const std::int32_t slopePart = (c->shapeWinLength - flatPart) / 2;
            ApplySineWindowFlp(xWindowed, xPtr, 1, slopePart);
            std::int32_t shift = slopePart;
            std::memcpy(xWindowed + shift, xPtr + shift, flatPart * sizeof(float));
            shift += flatPart;
            ApplySineWindowFlp(xWindowed + shift, xPtr + shift, 2, slopePart);

            // the next LPC analysis block
            xPtr += c->subfr_length;

            if (c->warping_Q16 > 0)
                WarpedAutocorrelationFlp(autoCorr, xWindowed, warping, c->shapeWinLength, c->shapingLPCOrder);
            else
                AutocorrelationFlp(autoCorr, xWindowed, c->shapeWinLength, c->shapingLPCOrder + 1, c->arch);

            // white noise, as a fraction of the energy
            autoCorr[0] += autoCorr[0] * ShapeWhiteNoiseFraction + 1.0f;

            // the correlations to prediction coefficients, the residual energy
            const float nrg = SchurFlp(rc, autoCorr, c->shapingLPCOrder);
            float*      ar  = &ctrl->AR[k * SilkMaxShapeLpcOrder];
            K2aFlp(ar, rc, c->shapingLPCOrder);
            ctrl->Gains[k] = static_cast<float>(std::sqrt(static_cast<double>(nrg)));
            // the gain adjusted for the warping
            if (c->warping_Q16 > 0)
                ctrl->Gains[k] *= WarpedGain(ar, warping, c->shapingLPCOrder);

            // the bandwidth expansion of the synthesis filter shaping
            BwexpanderFlp(ar, c->shapingLPCOrder, bwExp);
            // to monic warped coefficients, the absolute values limited
            if (c->warping_Q16 > 0)
                WarpedTrue2MonicCoefs(ar, warping, 3.999f, c->shapingLPCOrder);
            else
                LimitCoefs(ar, 3.999f, c->shapingLPCOrder);
        }

        // the gains increased at low speech activity
        const float gainMult = static_cast<float>(std::pow(2.0, static_cast<double>(-0.16f * snrAdjDb)));
        const float gainAdd  = static_cast<float>(std::pow(2.0, static_cast<double>(0.16f * MinQgainDb)));
        for (std::int32_t k = 0; k < c->nb_subfr; ++k) {
            ctrl->Gains[k] *= gainMult;
            ctrl->Gains[k] += gainAdd;
        }

        // the low-frequency shaping and the noise tilt: less low-frequency shaping for noisy inputs
        strength = LowFreqShaping * (1.0f + LowQualityLowFreqShapingDecr * (c->input_quality_bands_Q15[0] * (1.0f / 32768.0f) - 1.0f));
        strength *= c->speech_activity_Q8 * (1.0f / 256.0f);
        float tilt = 0.0f;
        if (c->indices.signalType == TypeVoiced) {
            // less low-frequency quantisation noise for periodic signals, depending on the pitch lag
            for (std::int32_t k = 0; k < c->nb_subfr; ++k) {
                const float b      = 0.2f / c->fs_kHz + 3.0f / ctrl->pitchL[k];
                ctrl->LF_MA_shp[k] = -1.0f + b;
                ctrl->LF_AR_shp[k] = 1.0f - b - b * strength;
            }
            tilt = -HpNoiseCoef - (1 - HpNoiseCoef) * HarmHpNoiseCoef * c->speech_activity_Q8 * (1.0f / 256.0f);
        } else {
            const float b      = 1.3f / c->fs_kHz;
            ctrl->LF_MA_shp[0] = -1.0f + b;
            ctrl->LF_AR_shp[0] = 1.0f - b - b * strength * 0.6f;
            for (std::int32_t k = 1; k < c->nb_subfr; ++k) {
                ctrl->LF_MA_shp[k] = ctrl->LF_MA_shp[0];
                ctrl->LF_AR_shp[k] = ctrl->LF_AR_shp[0];
            }
            tilt = -HpNoiseCoef;
        }

        // the harmonic shaping (USE_HARM_SHAPING)
        float harmShapeGain = 0.0f;
        if (c->indices.signalType == TypeVoiced) {
            harmShapeGain = HarmonicShaping;
            // more for high bitrates or noisy input
            harmShapeGain += HighRateOrLowQualityHarmonicShaping * (1.0f - (1.0f - ctrl->coding_quality) * ctrl->input_quality);
            // less for less periodic signals
            harmShapeGain *= static_cast<float>(std::sqrt(static_cast<double>(enc->LTPCorr)));
        }

        // smoothed over the sub-frames
        for (std::int32_t k = 0; k < c->nb_subfr; ++k) {
            shape->HarmShapeGain_smth += SubfrSmthCoef * (harmShapeGain - shape->HarmShapeGain_smth);
            ctrl->HarmShapeGain[k] = shape->HarmShapeGain_smth;
            shape->Tilt_smth += SubfrSmthCoef * (tilt - shape->Tilt_smth);
            ctrl->Tilt[k] = shape->Tilt_smth;
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk
