// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include "SilkState.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk {

    // Port of the noise shape analysis of the SILK encoder (libopus 1.6.1
    // silk/float/noise_shape_analysis_FLP.c).

    /// <summary>
    /// silk_noise_shape_analysis_FLP: the noise shaping AR coefficients, the initial gains, the
    /// quantiser offset of unvoiced frames, low-frequency shaping, tilt and harmonic shaping.
    /// </summary>
    void NoiseShapeAnalysis(SilkEncoderStateFlp* enc, SilkEncoderControlFlp* ctrl, const float* pitchRes, const float* x) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk
