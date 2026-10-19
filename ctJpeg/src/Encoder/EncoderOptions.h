// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#pragma once

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    /// <summary>--chroma_subsampling of cjpegli; Default is no subsampling (all components 1x1).</summary>
    enum class ChromaSubsampling : std::int32_t { Default, Yuv444, Yuv420 };

    /// <summary>
    /// The cjpegli settings ctJpeg ports: quality (-q) or Butteraugli
    /// distance (-d), chroma subsampling. Adaptive quantization, optimized Huffman codes and
    /// progressive level 2 are always on, as in cjpegli's defaults.
    /// </summary>
    struct EncoderOptions {
        bool              useQuality  = false; // -q given: quality is mapped to a distance
        std::int32_t      quality     = 90;    // -q 1..100
        float             distance    = 1.0f;  // -d 0..25 (cjpegli default 1.0)
        ChromaSubsampling subsampling = ChromaSubsampling::Default;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
