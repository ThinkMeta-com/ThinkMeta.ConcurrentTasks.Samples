// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

// The pixel phase for the avx2 feature set (compiled with /arch:AVX2).
#include "PixelPhaseImpl.inc"

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    bool ComputeBandAvx2(const EncoderSetup& setup, const Image& image, std::int32_t rowBegin, std::int32_t rowEnd, std::int32_t colBegin, std::int32_t colEnd,
                         Coefficients& out, PixelScratch& scratch) noexcept
    {
        return ComputeBand<SimdLevel::Avx2>(setup, image, rowBegin, rowEnd, colBegin, colEnd, out, scratch);
    }

    bool PixelKernelSelfTestAvx2(std::uint32_t seed) noexcept
    {
        return KernelSelfTest<SimdLevel::Avx2>(seed);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
