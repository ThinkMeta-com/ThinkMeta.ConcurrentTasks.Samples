// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <array>
#include <cstdint>

#include "QuantizeTypes.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    struct SessionConfig;

    // Port of takehiro.c: Huffman table choice, bit counting and scalefactor storage.

    /// <summary>huffman_init: the region0/region1 split by number of big values.</summary>
    void InitRegionSplit(const SessionConfig& config, std::array<std::int32_t, 576>& bvScf) noexcept;

    /// <summary>
    /// noquant_count_bits: counts the bits of the quantized values in gi.l3_enc, chooses the
    /// count1 table, the regions and their Huffman tables.
    /// </summary>
    std::int32_t CountBits(const SessionConfig& config, const QuantizeConstants& constants, GrInfo& gi) noexcept;

    /// <summary>best_huffman_divide: tries all region splits and a longer count1 region.</summary>
    void BestHuffmanDivide(const SessionConfig& config, const QuantizeConstants& constants, GrInfo& gi) noexcept;

    /// <summary>
    /// best_scalefac_store: the cheapest way to store the final scalefactors, including scfsi
    /// for the second granule.
    /// </summary>
    void BestScalefacStore(std::int32_t gr, std::int32_t ch, SideInfo& side) noexcept;

    /// <summary>
    /// scale_bitcount (MPEG-1): sets part2_length and scalefac_compress. Returns true if the
    /// scalefactors cannot be stored.
    /// </summary>
    bool ScaleBitcount(GrInfo& gi) noexcept;

    /// <summary>slen1_tab, slen2_tab: scalefactor bit widths by scalefac_compress.</summary>
    extern const std::array<std::int32_t, 16> slen1_tab;
    extern const std::array<std::int32_t, 16> slen2_tab;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
