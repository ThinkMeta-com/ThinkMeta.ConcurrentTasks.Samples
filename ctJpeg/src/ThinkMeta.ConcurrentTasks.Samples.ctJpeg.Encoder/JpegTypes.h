// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#pragma once

#include <array>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    inline constexpr std::int32_t DctSize        = 8;
    inline constexpr std::int32_t DctSize2       = 64;
    inline constexpr std::int32_t MaxComponents  = 4;
    inline constexpr std::int32_t NumQuantTables = 4;
    inline constexpr std::int32_t HuffmanMaxBits = 16;  // kJpegHuffmanMaxBitLength
    inline constexpr std::int32_t HuffmanSymbols = 256; // kJpegHuffmanAlphabetSize

    /// <summary>Zigzag position -> natural (row-major) position (kJPEGNaturalOrder).</summary>
    inline constexpr std::array<std::uint8_t, DctSize2> NaturalOrder = {
        0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  //
        12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28, //
        35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, //
        58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63, //
    };

    /// <summary>A quantization table in natural order (JQUANT_TBL::quantval).</summary>
    struct QuantTable {
        std::array<std::uint16_t, DctSize2> values{};
    };

    /// <summary>A Huffman table as in the DHT marker (JHUFF_TBL): code counts per length and symbols.</summary>
    struct HuffmanTable {
        std::array<std::uint8_t, HuffmanMaxBits + 1> bits{}; // bits[0] unused
        std::array<std::uint8_t, HuffmanSymbols>     values{};
    };

    /// <summary>One component of the frame (the SOF part of jpeg_component_info).</summary>
    struct ComponentInfo {
        std::uint8_t id         = 0; // component_id: 1, 2, 3 for YCbCr, 1 for gray
        std::int32_t hSamp      = 1;
        std::int32_t vSamp      = 1;
        std::int32_t quantTable = 0;
    };

    /// <summary>One scan of the scan script (jpeg_scan_info).</summary>
    struct ScanInfo {
        std::int32_t                            componentCount = 0;
        std::array<std::int32_t, MaxComponents> components{};
        std::int32_t                            ss = 0, se = 0, ah = 0, al = 0;
    };

    /// <summary>The frame header (SOF) fields.</summary>
    struct FrameInfo {
        std::int32_t                             width       = 0;
        std::int32_t                             height      = 0;
        std::int32_t                             components  = 0;
        bool                                     progressive = true;
        std::array<ComponentInfo, MaxComponents> component{};
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
