// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <cstdint>

#include "QuantizeTypes.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    struct SessionConfig;

    /// <summary>The bit reservoir: what the frames carry over in main_data_begin (sv_enc.ResvSize, ResvMax).</summary>
    struct ReservoirState {
        std::int32_t size = 0; // ResvSize: bits in the reservoir
        std::int32_t max  = 0; // ResvMax: the largest reservoir the next frame may leave

        /// <summary>
        /// main_data_begin for the next frame in bytes. The bit stream sets it after writing a
        /// frame, ReservoirFrameEnd of the next frame drains it: the one value the quantization
        /// and the bit stream stages share.
        /// </summary>
        std::int32_t mainDataBegin = 0;
    };

    /// <summary>getframebits: bits of a frame at a bitrate index (VBR frames are never padded).</summary>
    std::int32_t FrameBits(const SessionConfig& config, std::int32_t bitrateIndex) noexcept;

    /// <summary>
    /// ResvFrameBegin: updates the reservoir limit for a frame at bitrateIndex. Returns the bits
    /// the frame may use at most and the mean bits per granule.
    /// </summary>
    std::int32_t ReservoirFrameBegin(const SessionConfig& config, ReservoirState& reservoir, SideInfo& side, std::int32_t bitrateIndex,
                                     std::int32_t& meanBits) noexcept;

    /// <summary>
    /// ResvMaxBits: target bits for a granule and the extra bits it may take from the
    /// reservoir. substepShaping is sv_qnt.substep_shaping, whose bit 7 records a full reservoir.
    /// </summary>
    void ReservoirMaxBits(const SessionConfig& config, const ReservoirState& reservoir, std::int32_t& substepShaping, std::int32_t meanBits,
                          std::int32_t& targetBits, std::int32_t& extraBits) noexcept;

    /// <summary>ResvAdjust: takes the bits a granule used from the reservoir.</summary>
    inline void ReservoirAdjust(ReservoirState& reservoir, const GrInfo& gi) noexcept
    {
        reservoir.size -= gi.part2_3_length + gi.part2_length;
    }

    /// <summary>
    /// ResvFrameEnd: adds the frame's bits to the reservoir and drains what does not fit as
    /// stuffing into the ancillary data of the previous and of this frame. Sets the frame's
    /// main_data_begin.
    /// </summary>
    void ReservoirFrameEnd(ReservoirState& reservoir, SideInfo& side, std::int32_t meanBits) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
