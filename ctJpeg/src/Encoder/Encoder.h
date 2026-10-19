// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#pragma once

#include <cstdint>

#include "ByteBuffer.h"
#include "EncoderOptions.h"
#include "Image.h"
#include "SimdFeatures.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    class Coefficients;
    struct EncoderSetup;

    /// <summary>Outcome of an encode.</summary>
    enum class EncodeStatus : std::int32_t {
        Ok,
        NotImplemented, // a path that is not ported
        OutOfMemory,
        InvalidInput,
        ReadFailed, // the input file could not be read to its end
    };

    /// <summary>
    /// Encodes image into a complete JPEG file (SOI .. EOI), bit-identical to cjpegli on the Highway
    /// target of the given feature set. Sequential reference path (--sequential).
    /// </summary>
    EncodeStatus EncodeJpeg(const Image& image, const EncoderOptions& options, SimdLevel simd, ByteBuffer& output) noexcept;

    /// <summary>Phase 1: the setup of the image and the coefficient planes, not yet filled.</summary>
    EncodeStatus PrepareEncode(const Image& image, const EncoderOptions& options, EncoderSetup& setup, Coefficients& coefficients) noexcept;

    /// <summary>The iMCU rows of one pixel-phase band (sequential path and bands of the parallel one).</summary>
    inline constexpr std::int32_t DefaultBandRows = 8;

    /// <summary>The pixel columns of one pixel-phase strip of the parallel path (rounded up to iMCU columns).</summary>
    inline constexpr std::int32_t DefaultStripWidth = 1024;

    /// <summary>Phases 1 and 2: setup and pixel phase, the quantized coefficients of the whole image.</summary>
    EncodeStatus ComputeAllCoefficients(const Image& image, const EncoderOptions& options, SimdLevel simd, EncoderSetup& setup,
                                        Coefficients& coefficients) noexcept;

    /// <summary>Phase 3: tokenization, Huffman optimization and the JPEG file from the coefficients.</summary>
    EncodeStatus EncodeFromCoefficients(const EncoderSetup& setup, const Coefficients& coefficients, ByteBuffer& output) noexcept;

    /// <summary>Text for a status, for error messages.</summary>
    const wchar_t* EncodeStatusText(EncodeStatus status) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
