// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

#include "SilkState.h"

// The constants of the SILK encoder (libopus 1.6.1 silk/define.h, silk/tuning_parameters.h).

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk {

    // define.h
    constexpr std::int32_t NbSpeechFramesBeforeDtx       = 10; // 200 ms
    constexpr std::int32_t MaxConsecutiveDtx             = 20; // 400 ms
    constexpr std::int32_t VadNoActivity                 = 0;
    constexpr std::int32_t TypeNoVoiceActivity           = 0;
    constexpr std::int32_t TypeUnvoiced                  = 1;
    constexpr std::int32_t TypeVoiced                    = 2;
    constexpr std::int32_t CodeIndependently             = 0;
    constexpr std::int32_t CodeIndependentlyNoLtpScaling = 1;
    constexpr std::int32_t CodeConditionally             = 2;
    constexpr std::int32_t MaxFrameLengthMs              = 20;
    constexpr std::int32_t SubFrameLengthMs              = 5;
    constexpr std::int32_t LtpMemLengthMs                = 20;
    constexpr std::int32_t LaPitchMs                     = 2;
    constexpr std::int32_t LaPitchMax                    = LaPitchMs * SilkMaxFsKhz;
    constexpr std::int32_t LaShapeMs                     = 5;
    constexpr std::int32_t FindPitchLpcWinMs             = 20 + (LaPitchMs << 1);
    constexpr std::int32_t FindPitchLpcWinMs2Sf          = 10 + (LaPitchMs << 1);
    constexpr std::int32_t MinLpcOrder                   = 10;
    constexpr std::int32_t MaxDelDecStates               = 4;
    constexpr std::int32_t MinQgainDb                    = 2;
    constexpr std::int32_t MaxQgainDb                    = 88;
    constexpr std::int32_t NLevelsQgain                  = 64;
    constexpr std::int32_t MaxDeltaGainQuant             = 36;
    constexpr std::int32_t MinDeltaGainQuant             = -4;
    constexpr std::int32_t ShellCodecFrameLength         = 16;
    constexpr std::int32_t Log2ShellCodecFrameLength     = 4;
    constexpr std::int32_t MaxNbShellBlocks              = SilkMaxFrameLength / ShellCodecFrameLength;
    constexpr std::int32_t NRateLevels                   = 10;
    constexpr std::int32_t SilkMaxPulses                 = 16;
    constexpr std::int32_t VadInternalSubframesLog2      = 2;
    constexpr std::int32_t VadInternalSubframes          = 1 << VadInternalSubframesLog2;
    constexpr std::int32_t VadNoiseLevelSmoothCoefQ16    = 1024;
    constexpr std::int32_t VadNoiseLevelsBias            = 50;
    constexpr std::int32_t VadNegativeOffsetQ5           = 128; // the sigmoid is 0 at -128
    constexpr std::int32_t VadSnrFactorQ16               = 45000;
    constexpr std::int32_t VadSnrSmoothCoefQ18           = 4096;
    constexpr std::int32_t NlsfQuantMaxAmplitude         = 4;
    constexpr std::int32_t TransitionNb                  = 3;
    constexpr std::int32_t TransitionNa                  = 2;
    constexpr std::int32_t TransitionIntNum              = 5;
    constexpr std::int32_t TransitionFrames              = 5120 / MaxFrameLengthMs;
    constexpr std::int32_t PeMinComplex                  = 0;
    constexpr std::int32_t PeMidComplex                  = 1;
    constexpr std::int32_t PeMaxComplex                  = 2;

    // tuning_parameters.h
    constexpr std::int32_t BitreservoirDecayTimeMs             = 500;
    constexpr float        SpeechActivityDtxThres              = 0.05f;
    constexpr float        LbrrSpeechActivityThres             = 0.3f;
    constexpr std::int32_t MaxBandwidthSwitchDelayMs           = 5000;
    constexpr std::int32_t VariableHpMinCutoffHz               = 60;
    constexpr std::int32_t VariableHpMaxCutoffHz               = 100;
    constexpr float        VariableHpSmthCoef1                 = 0.1f;
    constexpr float        VariableHpMaxDeltaFreq              = 0.4f;
    constexpr float        WarpingMultiplier                   = 0.015f;
    constexpr float        FindPitchWhiteNoiseFraction         = 1e-3f;
    constexpr float        FindPitchBandwidthExpansion         = 0.99f;
    constexpr float        FindLpcCondFac                      = 1e-5f;
    constexpr float        MaxSumLogGainDb                     = 250.0f;
    constexpr float        LtpCorrInvMax                       = 0.03f;
    constexpr float        BgSnrDecrDb                         = 2.0f;
    constexpr float        HarmSnrIncrDb                       = 2.0f;
    constexpr float        EnergyVariationThresholdQntOffset   = 0.6f;
    constexpr float        ShapeWhiteNoiseFraction             = 3e-5f;
    constexpr float        BandwidthExpansion                  = 0.94f;
    constexpr float        HarmonicShaping                     = 0.3f;
    constexpr float        HighRateOrLowQualityHarmonicShaping = 0.2f;
    constexpr float        HpNoiseCoef                         = 0.25f;
    constexpr float        HarmHpNoiseCoef                     = 0.35f;
    constexpr float        LowFreqShaping                      = 4.0f;
    constexpr float        LowQualityLowFreqShapingDecr        = 0.5f;
    constexpr float        SubfrSmthCoef                       = 0.4f;
    constexpr float        LambdaOffset                        = 1.2f;
    constexpr float        LambdaSpeechAct                     = -0.2f;
    constexpr float        LambdaDelayedDecisions              = -0.05f;
    constexpr float        LambdaInputQuality                  = -0.1f;
    constexpr float        LambdaCodingQuality                 = -0.2f;
    constexpr float        LambdaQuantOffset                   = 0.8f;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk
