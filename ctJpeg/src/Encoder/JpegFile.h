// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#pragma once

#include <cstdint>

#include "ByteBuffer.h"
#include "EncoderSetup.h"
#include "Entropy.h"
#include "Markers.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    /// <summary>
    /// jpegli: the file layout of jpegli_finish_compress() (encode.cc): SOI (InitCompress), DQT and SOF
    /// (WriteFrameHeader), per scan DHT/SOS and its data, EOI. cjpegli writes no JFIF/Adobe marker and
    /// no ICC profile for PNM. writeScan(i, output) appends the entropy-coded data of scan i: the
    /// sequential encoder writes it there, the task graph appends what its stages wrote. False if out
    /// of memory or a code is missing.
    /// </summary>
    template <typename WriteScan>
    bool AssembleJpeg(const EncoderSetup& setup, const HuffmanCoding& coding, ByteBuffer& output, WriteScan&& writeScan) noexcept
    {
        bool       baseline = false;
        bool       ok       = WriteSoi(output) && WriteDqt(output, setup.frame, setup.quant.data(), baseline) && WriteSof(output, setup.frame, false);
        std::int32_t nextDhtIndex = 0;
        for (std::int32_t i = 0; ok && i < setup.scanCount; ++i)
            ok = WriteScanHeader(setup, i, coding, nextDhtIndex, output) && writeScan(i, output);
        return ok && WriteEoi(output);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
