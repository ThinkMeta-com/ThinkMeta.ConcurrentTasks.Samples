// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#pragma once

#include <array>
#include <cstdint>

#include "EncoderOptions.h"
#include "JpegTypes.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    /// <summary>The scans of progressive level 2: at most 1 + 4 per component (SetDefaultScanScript).</summary>
    inline constexpr std::int32_t MaxScans = 1 + 4 * MaxComponents;

    /// <summary>
    /// Everything jpegli derives before the first pixel: frame layout, quantization tables and
    /// multipliers, zero bias, scan script and entropy contexts (jpegli_set_defaults,
    /// jpegli_set_distance, ProcessCompressionParams, InitQuantizer as cjpegli calls them).
    /// </summary>
    struct EncoderSetup {
        FrameInfo    frame;
        std::int32_t maxH = 1, maxV = 1;               // max_h_samp_factor, max_v_samp_factor
        std::int32_t iMcuCols = 0, iMcuRows = 0;       // total_iMCU_cols, total_iMCU_rows
        std::int32_t xsizeBlocks = 0, ysizeBlocks = 0; // m->xsize_blocks, m->ysize_blocks (iMCU-padded)
        std::array<std::int32_t, MaxComponents> widthInBlocks{}, heightInBlocks{};
        std::array<std::int32_t, MaxComponents> hFactor{}, vFactor{}; // max / component sampling

        bool         ycbcr         = false; // jpeg_color_space == JCS_YCbCr (else grayscale)
        bool         adaptiveQuant = true;  // use_adaptive_quantization after ProcessCompressionParams
        float        distance      = 1.0f;
        std::int32_t yQuant01      = 0;     // quantval[1] of the Y table (dampening of the AQ field)

        std::array<QuantTable, NumQuantTables>                              quant{};
        std::array<std::array<float, DctSize2>, MaxComponents> quantMul{};       // 8 / quantval, natural order
        std::array<std::array<float, DctSize2>, MaxComponents> zeroBiasMul{};    // natural order
        std::array<std::array<float, DctSize2>, MaxComponents> zeroBiasOffset{}; // natural order

        std::int32_t                            scanCount = 0;
        std::array<ScanInfo, MaxScans>          scans{};
        std::array<std::int32_t, MaxScans>      acContextOffset{}; // m->ac_ctx_offset
        std::int32_t                            contextCount = 0;  // m->num_contexts
    };

    /// <summary>jpegli_quality_to_distance.</summary>
    float QualityToDistance(std::int32_t quality) noexcept;

    /// <summary>Fills setup for an image of the given size and component count (1 or 3).</summary>
    bool SetupEncoder(std::int32_t width, std::int32_t height, std::int32_t components, const EncoderOptions& options, EncoderSetup& setup) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
