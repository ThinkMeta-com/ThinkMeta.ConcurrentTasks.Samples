// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#pragma once

#include <cstdint>

#include "ByteBuffer.h"
#include "JpegTypes.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    // Marker segments as jpegli writes them (bitstream.cc). Every function appends to output and
    // returns false only if the output ran out of memory.

    /// <summary>SOI (FF D8).</summary>
    bool WriteSoi(ByteBuffer& output) noexcept;

    /// <summary>EOI (FF D9).</summary>
    bool WriteEoi(ByteBuffer& output) noexcept;

    /// <summary>
    /// DQT with the tables the components use, in table-index order (EncodeDQT with
    /// write_all_tables = false). A table with a value above 255 is written with 16-bit precision.
    /// baseline is false if any table needs 16 bits.
    /// </summary>
    bool WriteDqt(ByteBuffer& output, const FrameInfo& frame, const QuantTable* tables, bool& baseline) noexcept;

    /// <summary>SOF2 for a progressive frame, otherwise SOF0 (baseline) or SOF1 (EncodeSOF).</summary>
    bool WriteSof(ByteBuffer& output, const FrameInfo& frame, bool baseline) noexcept;

    /// <summary>DHT with count tables; slotIds[i] is the class/index byte (0x00..0x03 DC, 0x10..0x13 AC).</summary>
    bool WriteDht(ByteBuffer& output, const HuffmanTable* tables, const std::uint8_t* slotIds, std::int32_t count) noexcept;

    /// <summary>
    /// SOS for one scan; dcSlot[i] and acSlot[i] are the table indices (0..3) of the i-th component
    /// of the scan (EncodeSOS).
    /// </summary>
    bool WriteSos(ByteBuffer& output, const FrameInfo& frame, const ScanInfo& scan, const std::int32_t* dcSlot, const std::int32_t* acSlot) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
