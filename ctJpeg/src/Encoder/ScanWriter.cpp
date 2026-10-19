// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#include "ScanWriter.h"

#include <cstddef>
#include <cstdint>

#include "BitWriter.h"
#include "Markers.h"

// Port of the scan part of jpegli's bitstream.cc: WriteScanHeader, WriteScanData.

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    namespace {

        // DHT slots per table class: the first scan brings all DC tables and at most this many AC tables.
        constexpr std::int32_t TableSlots = 4;

    } // namespace

    bool WriteScanData(const EncoderSetup& setup, std::int32_t scanIndex, const ScanTokens& tokens, const HuffmanCoding& coding, ByteBuffer& output) noexcept
    {
        BitWriter bw(output);
        WriteTokenParts(setup, scanIndex, tokens, 0, tokens.partCount, ChunkCursor{}, coding, bw);
        bw.JumpToByteBoundary();
        return bw.Healthy();
    }

    bool WriteScanHeader(const EncoderSetup& setup, std::int32_t scanIndex, const HuffmanCoding& coding, std::int32_t& nextDhtIndex,
                         ByteBuffer& output) noexcept
    {
        const ScanInfo& si     = setup.scans[static_cast<std::size_t>(scanIndex)];
        std::int32_t    numDht = 0;
        if (scanIndex == 0) {
            // all DC tables and at most 4 AC tables before the first scan
            for (std::int32_t i = 0, numAc = 0; i < coding.tableCount; ++i) {
                if (coding.slotIds[static_cast<std::size_t>(i)] >= AcSlotBase && numAc++ >= TableSlots)
                    break;
                ++numDht;
            }
        } else if (si.ss > 0) {
            // progressive AC scans need at most one new table
            if (coding.contextMap[static_cast<std::size_t>(setup.acContextOffset[static_cast<std::size_t>(scanIndex)])] == nextDhtIndex)
                numDht = 1;
        }
        if (numDht > 0) {
            if (!WriteDht(output, coding.tables.data() + nextDhtIndex, coding.slotIds.data() + nextDhtIndex, numDht))
                return false;
            nextDhtIndex += numDht;
        }
        std::int32_t dcSlot[MaxComponents], acSlot[MaxComponents];
        for (std::int32_t i = 0; i < si.componentCount; ++i) {
            const std::int32_t c         = si.components[static_cast<std::size_t>(i)];
            const std::int32_t acContext = setup.acContextOffset[static_cast<std::size_t>(scanIndex)] + i;
            dcSlot[i]                    = coding.slotIds[coding.contextMap[static_cast<std::size_t>(c)]];
            acSlot[i]                    = coding.slotIds[coding.contextMap[static_cast<std::size_t>(acContext)]] - AcSlotBase;
        }
        return WriteSos(output, setup.frame, si, dcSlot, acSlot);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
