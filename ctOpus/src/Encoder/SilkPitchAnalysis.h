// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

#include "SilkState.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk {

    // Port of the pitch analysis of the SILK encoder (libopus 1.6.1 silk/float/find_pitch_lags_FLP.c,
    // pitch_analysis_core_FLP.c).

    /// <summary>
    /// silk_find_pitch_lags_FLP: the LPC residual res of the pitch analysis, the prediction gain,
    /// the pitch lags and the voicing decision (signalType) of the frame x.
    /// </summary>
    void FindPitchLags(SilkEncoderStateFlp* enc, SilkEncoderControlFlp* ctrl, float res[], const float x[], std::int32_t arch) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk
