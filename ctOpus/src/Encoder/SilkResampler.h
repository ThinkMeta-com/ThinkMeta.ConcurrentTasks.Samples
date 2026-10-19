// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

#include "SilkState.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk {

    // Port of the SILK resampler of libopus 1.6.1 (silk/resampler.c, resampler_private_*.c,
    // resampler_down2.c, resampler_down2_3.c).

    /// <summary>silk_resampler_init: the state for fsHzIn to fsHzOut (forEnc: the encoder's delay table); 0 or -1.</summary>
    std::int32_t ResamplerInit(SilkResamplerState* s, std::int32_t fsHzIn, std::int32_t fsHzOut, bool forEnc) noexcept;

    /// <summary>silk_resampler: at least 1 ms of input.</summary>
    std::int32_t Resample(SilkResamplerState* s, std::int16_t* out, const std::int16_t* in, std::int32_t inLen) noexcept;

    /// <summary>silk_resampler_down2: downsampling by 2 (state [2]).</summary>
    void ResamplerDown2(std::int32_t* s, std::int16_t* out, const std::int16_t* in, std::int32_t inLen) noexcept;

    /// <summary>silk_resampler_down2_3: downsampling by 2/3, low quality (state [6]).</summary>
    void ResamplerDown2_3(std::int32_t* s, std::int16_t* out, const std::int16_t* in, std::int32_t inLen) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk
