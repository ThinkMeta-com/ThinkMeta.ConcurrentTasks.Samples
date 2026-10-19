// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

#include "RangeEncoder.h"
#include "SilkState.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk {

    // Port of the stereo pre-processing of the SILK encoder (libopus 1.6.1 silk/stereo_LR_to_MS.c,
    // stereo_find_predictor.c, stereo_quant_pred.c, stereo_encode_pred.c).

    /// <summary>
    /// silk_stereo_LR_to_MS: left/right (x1, x2, each with 2 samples of history before) to an
    /// adaptive mid/side representation with quantised predictors, the mid/side rates and the
    /// mid-only decision.
    /// </summary>
    void StereoLrToMs(SilkStereoEncState* state, std::int16_t x1[], std::int16_t x2[], std::int8_t ix[2][3], std::int8_t* midOnlyFlag,
                      std::int32_t midSideRatesBps[], std::int32_t totalRateBps, std::int32_t prevSpeechActQ8, bool toMono, std::int32_t fsKhz,
                      std::int32_t frameLength) noexcept;

    /// <summary>silk_stereo_encode_pred.</summary>
    void StereoEncodePred(RangeEncoder* rangeEnc, const std::int8_t ix[2][3]) noexcept;

    /// <summary>silk_stereo_encode_mid_only.</summary>
    void StereoEncodeMidOnly(RangeEncoder* rangeEnc, std::int8_t midOnlyFlag) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk
