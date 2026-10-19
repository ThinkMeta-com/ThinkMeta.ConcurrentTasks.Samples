// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#pragma once

#include <array>

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    // Data tables of jpegli's quantizer (lib/jpegli/quant.cc), generated from the source text.

    /// <summary>kBaseQuantMatrixYCbCr: Y, Cb, Cr, 64 each, natural order.</summary>
    extern const std::array<float, 192> BaseQuantMatrixYCbCr;

    /// <summary>kZeroBiasMulYCbCrLQ / HQ: zero-bias multipliers for low and high quality, Y, Cb, Cr.</summary>
    extern const std::array<float, 192> ZeroBiasMulYCbCrLQ;
    extern const std::array<float, 192> ZeroBiasMulYCbCrHQ;

    /// <summary>k420Rescale: chroma rescale with 4:2:0 subsampling.</summary>
    extern const std::array<float, 64> Rescale420;

    /// <summary>kExponent: non-linear distance scaling per coefficient.</summary>
    extern const std::array<float, 64> DistanceExponent;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
