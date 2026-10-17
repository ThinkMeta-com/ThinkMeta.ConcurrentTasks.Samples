// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "QuantizeTypes.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    struct SessionConfig;
    struct BitstreamState;

    inline constexpr std::int32_t NUMTOCENTRIES = 100;

    /// <summary>
    /// VBR_seek_info_t plus the music CRC: what the Xing/LAME tag frame needs. The seek table
    /// collects the bitrate sum in a bag of 400 entries that halves itself when full.
    /// </summary>
    struct VbrTagState {
        std::int32_t                   sum  = 0; // what we have seen so far
        std::int32_t                   seen = 0; // how many frames we have seen in this chunk
        std::int32_t                   want = 1; // how many frames we want to collect into one chunk
        std::int32_t                   pos  = 0; // actual position in our bag
        std::int32_t                   size = 0; // size of our bag
        std::array<std::int32_t, 400>  bag;
        std::uint32_t                  nVbrNumFrames  = 0;
        std::uint32_t                  nBytesWritten  = 0;
        std::uint32_t                  TotalFrameSize = 0;
        std::uint16_t                  musicCrc       = 0; // nMusicCRC
        std::array<std::uint16_t, 256> crcTable;
    };

    /// <summary>
    /// InitVbrTag: prepares the seek table and writes a zero frame of the tag's size into the
    /// bit stream as a placeholder. Clears config.write_lame_tag if the tag does not fit.
    /// </summary>
    void InitVbrTag(SessionConfig& config, VbrTagState& tag, BitstreamState& bs) noexcept;

    /// <summary>AddVbrFrame: one more frame in the seek table.</summary>
    void AddVbrFrame(VbrTagState& tag, std::int32_t bitrateIndex) noexcept;

    /// <summary>UpdateMusicCRC: the CRC-16 over the MP3 data and its byte count.</summary>
    void AddMusicBytes(VbrTagState& tag, const std::byte* buffer, std::int32_t size) noexcept;

    /// <summary>
    /// lame_get_lametag_frame: the final Xing/LAME tag frame for the start of the file.
    /// Returns its size, 0 if there is no tag, or the size needed if size is too small.
    /// </summary>
    std::size_t GetLameTagFrame(const SessionConfig& config, const VbrTagState& tag, const EncoderResult& result, std::byte* frame, std::size_t size) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
