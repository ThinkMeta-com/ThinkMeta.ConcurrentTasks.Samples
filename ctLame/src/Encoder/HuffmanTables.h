// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <array>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    inline constexpr std::int32_t HuffmanTableCount = 34; // HTN: tables 0..31 and the count1 tables 32, 33

    /// <summary>huffcodetab: one Huffman code table of ISO/IEC 11172-3 Table B.7.</summary>
    struct HuffCodeTab {
        std::uint32_t        xlen;   // max. x-index + 1
        std::uint32_t        linmax; // max number to be stored in linbits
        const std::uint16_t* table;  // codes, array[xlen][ylen]
        const std::uint8_t*  hlen;   // code lengths, array[xlen][ylen]
    };

    /// <summary>ht: the Huffman tables 0..31 and the count1 tables 32 (A) and 33 (B).</summary>
    extern const std::array<HuffCodeTab, HuffmanTableCount> ht;

    /// <summary>Code lengths of the count1 tables A and B (t32l, t33l).</summary>
    extern const std::array<std::uint8_t, 16> t32l;
    extern const std::array<std::uint8_t, 16> t33l;

    /// <summary>
    /// Code lengths of two tables packed into one word, (first << 16) + second, so one lookup
    /// counts both: largetbl (tables 16 and 24), table23 (2 and 3), table56 (5 and 6).
    /// </summary>
    struct HuffmanPairLengths {
        std::array<std::uint32_t, 16 * 16> largetbl;
        std::array<std::uint32_t, 3 * 3>   table23;
        std::array<std::uint32_t, 4 * 4>   table56;
    };

    /// <summary>Computes the packed code lengths from the tables.</summary>
    void InitHuffmanPairLengths(HuffmanPairLengths& lengths) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
