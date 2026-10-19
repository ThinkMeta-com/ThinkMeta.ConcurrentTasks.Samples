// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#pragma once

#include <cstdint>

#include "Coefficients.h"
#include "EncoderSetup.h"
#include "Image.h"
#include "SimdFeatures.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    /// <summary>
    /// The window buffers of a pixel-phase band, kept across bands so that a worker does not
    /// allocate (and fault in) fresh memory for every band. One per worker; not thread-safe.
    /// </summary>
    class PixelScratch
    {
    public:
        PixelScratch() noexcept                      = default;
        PixelScratch(const PixelScratch&)            = delete;
        PixelScratch& operator=(const PixelScratch&) = delete;
        ~PixelScratch();

        /// <summary>At least floats floats, contents undefined; nullptr if out of memory.</summary>
        float* Reserve(std::size_t floats) noexcept;

    private:
        float*      data_     = nullptr;
        std::size_t capacity_ = 0;
    };

    /// <summary>
    /// The pixel phase of jpegli for the iMCU rows [rowBegin, rowEnd): color transform, padding,
    /// downsampling, adaptive quantization field, DCT and quantization, written to the blocks of
    /// these rows, with the buffers of scratch. Every row range reads its own context rows (clamped
    /// like jpegli's padding), so disjoint ranges can run in parallel and give the same
    /// coefficients as one range over the whole image. False if out of memory.
    /// jpegli: ProcessiMCURows() (encode.cc) and the functions it calls.
    /// </summary>
    bool ComputeCoefficients(const EncoderSetup& setup, const Image& image, SimdLevel simd, std::int32_t rowBegin, std::int32_t rowEnd, Coefficients& out,
                             PixelScratch& scratch) noexcept;

    /// <summary>
    /// ComputeCoefficients for the iMCU columns [colBegin, colEnd) of the rows [rowBegin, rowEnd):
    /// a tile that reads its own context columns as well, so the tiles of an image can run in
    /// parallel and give the same coefficients as whole rows.
    /// </summary>
    bool ComputeCoefficients(const EncoderSetup& setup, const Image& image, SimdLevel simd, std::int32_t rowBegin, std::int32_t rowEnd, std::int32_t colBegin,
                             std::int32_t colEnd, Coefficients& out, PixelScratch& scratch) noexcept;

    /// <summary>
    /// Runs every SIMD kernel of the pixel phase against its scalar reference
    /// (PixelPhaseReference.inc) on random data of the seed; true if all results are
    /// bit-identical. Not thread-safe.
    /// </summary>
    bool PixelKernelSelfTest(SimdLevel simd, std::uint32_t seed) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
