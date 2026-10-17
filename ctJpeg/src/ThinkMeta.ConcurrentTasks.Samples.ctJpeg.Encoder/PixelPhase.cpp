// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#include "PixelPhase.h"

#include <cstddef>
#include <cstdint>

#include <Windows.h>

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    // PixelPhaseSse2.cpp / PixelPhaseAvx2.cpp
    bool ComputeBandSse2(const EncoderSetup& setup, const Image& image, std::int32_t rowBegin, std::int32_t rowEnd, std::int32_t colBegin, std::int32_t colEnd,
                         Coefficients& out, PixelScratch& scratch) noexcept;
    bool ComputeBandAvx2(const EncoderSetup& setup, const Image& image, std::int32_t rowBegin, std::int32_t rowEnd, std::int32_t colBegin, std::int32_t colEnd,
                         Coefficients& out, PixelScratch& scratch) noexcept;
    bool PixelKernelSelfTestSse2(std::uint32_t seed) noexcept;
    bool PixelKernelSelfTestAvx2(std::uint32_t seed) noexcept;

    PixelScratch::~PixelScratch()
    {
        if (data_ != nullptr)
            HeapFree(GetProcessHeap(), 0, data_);
    }

    float* PixelScratch::Reserve(std::size_t floats) noexcept
    {
        if (floats <= capacity_)
            return data_;
        if (data_ != nullptr)
            HeapFree(GetProcessHeap(), 0, data_);
        data_     = static_cast<float*>(HeapAlloc(GetProcessHeap(), 0, floats * sizeof(float)));
        capacity_ = data_ != nullptr ? floats : 0;
        return data_;
    }

    bool ComputeCoefficients(const EncoderSetup& setup, const Image& image, SimdLevel simd, std::int32_t rowBegin, std::int32_t rowEnd, std::int32_t colBegin,
                             std::int32_t colEnd, Coefficients& out, PixelScratch& scratch) noexcept
    {
        return simd == SimdLevel::Avx2 ? ComputeBandAvx2(setup, image, rowBegin, rowEnd, colBegin, colEnd, out, scratch)
                                       : ComputeBandSse2(setup, image, rowBegin, rowEnd, colBegin, colEnd, out, scratch);
    }

    bool ComputeCoefficients(const EncoderSetup& setup, const Image& image, SimdLevel simd, std::int32_t rowBegin, std::int32_t rowEnd, Coefficients& out,
                             PixelScratch& scratch) noexcept
    {
        return ComputeCoefficients(setup, image, simd, rowBegin, rowEnd, 0, setup.iMcuCols, out, scratch);
    }

    bool ComputeCoefficients(const EncoderSetup& setup, const Image& image, SimdLevel simd, std::int32_t rowBegin, std::int32_t rowEnd,
                             Coefficients& out) noexcept
    {
        PixelScratch scratch;
        return ComputeCoefficients(setup, image, simd, rowBegin, rowEnd, out, scratch);
    }

    bool PixelKernelSelfTest(SimdLevel simd, std::uint32_t seed) noexcept
    {
        return simd == SimdLevel::Avx2 ? PixelKernelSelfTestAvx2(seed) : PixelKernelSelfTestSse2(seed);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
