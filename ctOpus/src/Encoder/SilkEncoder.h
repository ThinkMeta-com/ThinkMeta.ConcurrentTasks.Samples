// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

#include "RangeEncoder.h"
#include "SilkState.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk {

    // Port of the API level of the SILK encoder (libopus 1.6.1 silk/enc_API.c, init_encoder.c,
    // control_codec.c, check_control_input.c, control_audio_bandwidth.c, control_SNR.c,
    // HP_variable_cutoff.c). The state is SilkEncoder of SilkState.h.

    constexpr std::int32_t SilkNoError = 0;

    /// <summary>silk_Get_Encoder_Size: mono skips the second channel state.</summary>
    std::int32_t GetEncoderSize(std::int32_t channels) noexcept;

    /// <summary>silk_InitEncoder: resets the state and reads the control structure back.</summary>
    std::int32_t InitEncoder(SilkEncoder* enc, std::int32_t channels, std::int32_t arch, SilkEncControl* encStatus) noexcept;

    /// <summary>
    /// silk_Encode: buffers, resamples and encodes nSamplesIn interleaved float samples at the
    /// API rate (prefillFlag: only fill the buffers with 10 ms of audio).
    /// </summary>
    std::int32_t Encode(SilkEncoder* enc, SilkEncControl* encControl, const float* samplesIn, std::int32_t nSamplesIn, RangeEncoder* rangeEnc,
                        std::int32_t* nBytesOut, std::int32_t prefillFlag, std::int32_t activity) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk
