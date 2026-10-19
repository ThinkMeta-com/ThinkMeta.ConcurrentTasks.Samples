// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <array>
#include <cstdint>

#include "Types.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    // Port of the MPEG-1 tables of tables.c and quantize_pvt.c and the helpers of util.c.

    /// <summary>Bitrates in kbps by bitrate index (bitrate_table); 0 = free format, -1 = forbidden.</summary>
    extern const std::array<std::int32_t, 16> BitrateTable;

    /// <summary>Sample rates by sample rate index (samplerate_table).</summary>
    extern const std::array<std::int32_t, 4> SamplerateTable;

    /// <summary>scalefac_struct.</summary>
    struct ScalefacStruct {
        std::array<std::int32_t, 1 + SBMAX_l> l;
        std::array<std::int32_t, 1 + SBMAX_s> s;
        std::array<std::int32_t, 1 + PSFB21>  psfb21;
        std::array<std::int32_t, 1 + PSFB12>  psfb12;
    };

    /// <summary>Scalefactor band boundaries by sample rate index (sfBandIndex).</summary>
    extern const std::array<ScalefacStruct, 3> SfBandIndex;

    /// <summary>Nearest MPEG-1 bitrate in kbps (FindNearestBitrate).</summary>
    std::int32_t FindNearestBitrate(std::int32_t bitrateKbps) noexcept;

    /// <summary>Bitrate index of a bitrate in kbps, -1 if there is none (BitrateIndex).</summary>
    std::int32_t BitrateIndex(std::int32_t bitrateKbps) noexcept;

    /// <summary>Sample rate index: 0 = 44.1, 1 = 48, 2 = 32 kHz, -1 otherwise (SmpFrqIndex).</summary>
    std::int32_t SmpFrqIndex(std::int32_t sampleRate) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
