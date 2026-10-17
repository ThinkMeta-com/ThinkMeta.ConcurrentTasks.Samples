// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "HuffmanTables.h"

#include <array>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

#include "HuffmanTables.inc"

    void InitHuffmanPairLengths(HuffmanPairLengths& lengths) noexcept
    {
        for (std::int32_t i = 0; i < 16 * 16; ++i)
            lengths.largetbl[i] = static_cast<std::uint32_t>(ht[16].hlen[i]) << 16 | ht[24].hlen[i];
        for (std::int32_t i = 0; i < 3 * 3; ++i)
            lengths.table23[i] = static_cast<std::uint32_t>(ht[2].hlen[i]) << 16 | ht[3].hlen[i];
        for (std::int32_t i = 0; i < 4 * 4; ++i)
            lengths.table56[i] = static_cast<std::uint32_t>(ht[5].hlen[i]) << 16 | ht[6].hlen[i];
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
