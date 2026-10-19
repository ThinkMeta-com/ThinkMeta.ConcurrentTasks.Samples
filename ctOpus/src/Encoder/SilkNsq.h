// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

#include "SilkState.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk {

    // Port of the noise shaping quantisation of the SILK encoder (libopus 1.6.1 silk/NSQ.c,
    // NSQ.h, NSQ_del_dec.c, LPC_analysis_filter.c, the NSQ wrapper of float/wrappers_FLP.c; the
    // SSE4.1/AVX2 variants of libopus compute the same).

    /// <summary>
    /// silk_NSQ_wrapper_FLP: the control parameters to fixed point, then the noise shaping
    /// quantisation of the frame x (with delayed decision for more than one state or warping).
    /// </summary>
    void NsqWrapper(SilkEncoderStateFlp* enc, SilkEncoderControlFlp* ctrl, SilkSideInfoIndices* indices, SilkNsqState* nsq, std::int8_t pulses[],
                    const float x[]) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk
