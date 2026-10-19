// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

#include "RangeEncoder.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    // Port of the pyramid vector quantiser of libopus 1.6.1 (celt/vq.c, celt/x86/vq_sse2.c,
    // celt/cwrs.c), float build, encoder side.

    /// <summary>alg_quant: K pulses for the N-dimensional band X; with resynth X becomes the quantised vector times gain.</summary>
    std::uint32_t AlgQuant(float* x, std::int32_t n, std::int32_t k, std::int32_t spread, std::int32_t b, RangeEncoder* enc, float gain, bool resynth) noexcept;

    /// <summary>renormalise_vector: X to the norm gain.</summary>
    void RenormaliseVector(float* x, std::int32_t n, float gain) noexcept;

    /// <summary>stereo_itheta: the angle between mid and side (or X and Y) in Q30 of a quarter turn.</summary>
    std::int32_t StereoItheta(const float* x, const float* y, bool stereo, std::int32_t n) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
