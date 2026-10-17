// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#include "Markers.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    bool WriteSoi(ByteBuffer& output) noexcept
    {
        const std::uint8_t soi[] = {0xFF, 0xD8};
        return output.Append(soi, sizeof(soi));
    }

    bool WriteEoi(ByteBuffer& output) noexcept
    {
        const std::uint8_t eoi[] = {0xFF, 0xD9};
        return output.Append(eoi, sizeof(eoi));
    }

    bool WriteDqt(ByteBuffer& output, const FrameInfo& frame, const QuantTable* tables, bool& baseline) noexcept
    {
        std::array<std::uint8_t, 4 + NumQuantTables * (1 + 2 * DctSize2)> data; // 520 bytes
        std::size_t                                                       pos = 0;
        data[pos++]                                                           = 0xFF;
        data[pos++]                                                           = 0xDB;
        pos += 2; // length, filled in below

        std::array<bool, NumQuantTables> send{};
        for (std::int32_t c = 0; c < frame.components; ++c)
            send[static_cast<std::size_t>(frame.component[static_cast<std::size_t>(c)].quantTable)] = true;

        baseline = true;
        for (std::int32_t i = 0; i < NumQuantTables; ++i) {
            if (!send[static_cast<std::size_t>(i)])
                continue;
            const QuantTable& table     = tables[i];
            std::int32_t      precision = 0;
            for (const std::uint16_t q : table.values) {
                if (q > 255) {
                    precision = 1;
                    baseline  = false;
                }
            }
            data[pos++] = static_cast<std::uint8_t>((precision << 4) + i);
            for (std::int32_t j = 0; j < DctSize2; ++j) {
                const std::uint16_t val = table.values[NaturalOrder[static_cast<std::size_t>(j)]];
                if (precision)
                    data[pos++] = static_cast<std::uint8_t>(val >> 8);
                data[pos++] = static_cast<std::uint8_t>(val & 0xFF);
            }
        }
        if (pos <= 4)
            return true;
        data[2] = static_cast<std::uint8_t>((pos - 2) >> 8);
        data[3] = static_cast<std::uint8_t>((pos - 2) & 0xFF);
        return output.Append(data.data(), pos);
    }

    bool WriteSof(ByteBuffer& output, const FrameInfo& frame, bool baseline) noexcept
    {
        const std::uint8_t                                  marker    = frame.progressive ? 0xC2 : baseline ? 0xC0 : 0xC1;
        const auto                                          markerLen = static_cast<std::size_t>(8 + 3 * frame.components);
        std::array<std::uint8_t, 2 + 8 + 3 * MaxComponents> data;
        std::size_t                                         pos = 0;
        data[pos++]                                             = 0xFF;
        data[pos++]                                             = marker;
        data[pos++]                                             = static_cast<std::uint8_t>(markerLen >> 8);
        data[pos++]                                             = static_cast<std::uint8_t>(markerLen & 0xFF);
        data[pos++]                                             = 8; // kJpegPrecision
        data[pos++]                                             = static_cast<std::uint8_t>(frame.height >> 8);
        data[pos++]                                             = static_cast<std::uint8_t>(frame.height & 0xFF);
        data[pos++]                                             = static_cast<std::uint8_t>(frame.width >> 8);
        data[pos++]                                             = static_cast<std::uint8_t>(frame.width & 0xFF);
        data[pos++]                                             = static_cast<std::uint8_t>(frame.components);
        for (std::int32_t i = 0; i < frame.components; ++i) {
            const ComponentInfo& comp = frame.component[static_cast<std::size_t>(i)];
            data[pos++]               = comp.id;
            data[pos++]               = static_cast<std::uint8_t>((comp.hSamp << 4) | comp.vSamp);
            data[pos++]               = static_cast<std::uint8_t>(comp.quantTable);
        }
        return output.Append(data.data(), pos);
    }

    bool WriteDht(ByteBuffer& output, const HuffmanTable* tables, const std::uint8_t* slotIds, std::int32_t count) noexcept
    {
        std::size_t markerLen = 2;
        for (std::int32_t t = 0; t < count; ++t) {
            markerLen += HuffmanMaxBits + 1;
            for (std::int32_t j = 0; j <= HuffmanMaxBits; ++j)
                markerLen += tables[t].bits[static_cast<std::size_t>(j)];
        }
        if (markerLen <= 2)
            return true;
        const std::uint8_t head[] = {0xFF, 0xC4, static_cast<std::uint8_t>(markerLen >> 8), static_cast<std::uint8_t>(markerLen & 0xFF)};
        if (!output.Append(head, sizeof(head)))
            return false;
        for (std::int32_t t = 0; t < count; ++t) {
            const HuffmanTable& table = tables[t];
            std::size_t         total = 0;
            for (std::int32_t i = 0; i <= HuffmanMaxBits; ++i)
                total += table.bits[static_cast<std::size_t>(i)];
            if (!output.Append(slotIds[t]) || !output.Append(table.bits.data() + 1, HuffmanMaxBits) || !output.Append(table.values.data(), total))
                return false;
        }
        return true;
    }

    bool WriteSos(ByteBuffer& output, const FrameInfo& frame, const ScanInfo& scan, const std::int32_t* dcSlot, const std::int32_t* acSlot) noexcept
    {
        const auto                                          markerLen = static_cast<std::size_t>(6 + 2 * scan.componentCount);
        std::array<std::uint8_t, 2 + 6 + 2 * MaxComponents> data;
        std::size_t                                         pos = 0;
        data[pos++]                                             = 0xFF;
        data[pos++]                                             = 0xDA;
        data[pos++]                                             = static_cast<std::uint8_t>(markerLen >> 8);
        data[pos++]                                             = static_cast<std::uint8_t>(markerLen & 0xFF);
        data[pos++]                                             = static_cast<std::uint8_t>(scan.componentCount);
        for (std::int32_t i = 0; i < scan.componentCount; ++i) {
            const std::int32_t comp = scan.components[static_cast<std::size_t>(i)];
            data[pos++]             = frame.component[static_cast<std::size_t>(comp)].id;
            data[pos++]             = static_cast<std::uint8_t>((dcSlot[i] << 4) + acSlot[i]);
        }
        data[pos++] = static_cast<std::uint8_t>(scan.ss);
        data[pos++] = static_cast<std::uint8_t>(scan.se);
        data[pos++] = static_cast<std::uint8_t>((scan.ah << 4) | scan.al);
        return output.Append(data.data(), pos);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
