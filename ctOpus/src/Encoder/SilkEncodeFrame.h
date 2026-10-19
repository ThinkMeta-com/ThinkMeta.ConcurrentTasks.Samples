// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

#include "RangeEncoder.h"
#include "SilkState.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk {

    // Port of the frame level of the SILK encoder (libopus 1.6.1 silk/float/encode_frame_FLP.c,
    // process_gains_FLP.c).

    /// <summary>silk_encode_do_VAD_FLP: the voice activity detection and the VAD/DTX flags of the frame.</summary>
    void EncodeDoVad(SilkEncoderStateFlp* enc, std::int32_t activity) noexcept;

    /// <summary>
    /// silk_encode_frame_FLP: analyses, quantises and encodes one frame (the rate control loop
    /// over the gains for maxBits); with prefillFlag only the buffers are updated.
    /// </summary>
    std::int32_t EncodeFrame(SilkEncoderStateFlp* enc, std::int32_t* nBytesOut, RangeEncoder* rangeEnc, std::int32_t condCoding, std::int32_t maxBits,
                             bool useCbr) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk
