// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

#include "SilkState.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk {

    // Port of the voice activity detector of SILK (libopus 1.6.1 silk/VAD.c; the SSE4.1 variant
    // of libopus computes the same).

    /// <summary>
    /// silk_VAD_GetSA_Q8: the speech activity level (speech_activity_Q8), the input tilt and the
    /// quality per band of one frame of in.
    /// </summary>
    void VadGetSaQ8(SilkEncoderState* enc, const std::int16_t in[]) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::silk
