// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <array>
#include <cstdint>

#include "MdctKernels.h"
#include "Types.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    /// <summary>The 18 x 32 subband samples of one granule, [time][subband].</summary>
    using SubbandSamples = std::array<std::array<float, SBLIMIT>, 18>;

    // the filterbank walks a granule's samples as one block of 18 * 32 values, across the rows
    static_assert(sizeof(SubbandSamples) == 18 * SBLIMIT * sizeof(float), "SubbandSamples must be contiguous");

    /// <summary>The 576 frequency lines of both granules of a frame, [granule][line].</summary>
    using FrameLines = std::array<std::array<float, 576>, 2>;

    /// <summary>The amplitude filter of the 32 subbands (lowpass and highpass).</summary>
    using AmpFilter = std::array<float, 32>;

    /// <summary>The analysis window of the polyphase filterbank (enwindow of newmdct.c), for the kernel tests.</summary>
    const float* MdctAnalysisWindow() noexcept;

    /// <summary>The subband samples of one channel that carry over from granule to granule (sv_enc.sb_sample[ch]).</summary>
    struct MdctChannelState {
        std::array<SubbandSamples, 2> sb_sample = {};
    };

    /// <summary>
    /// Port of mdct_sub48 (newmdct.c) for one channel: polyphase filterbank, lowpass/highpass by
    /// the amplitude filter, MDCT and aliasing reduction for both granules of a frame.
    /// input is the channel's frame input (mfbuf), blockType the block type of each granule.
    /// </summary>
    void MdctChannel(const AmpFilter& ampFilter, MdctChannelState& state, const sample_t* input, const std::array<std::int32_t, 2>& blockType,
                     FrameLines& xr) noexcept;

    /// <summary>The polyphase filterbank output of both granules of a frame, for one channel (before the amplitude filter).</summary>
    struct MdctPolyphase {
        std::array<SubbandSamples, 2> sb;
    };

    /// <summary>
    /// The polyphase filterbank of both granules of a frame for one channel. It has no state,
    /// so it may run ahead, in any order (the output also serves the next frame through
    /// MdctCarry instead of being computed again).
    /// </summary>
    void MdctPolyphaseFrame(const sample_t* input, MdctPolyphase& out, MdctKernels::WindowSubbandFunction windowSubband = MdctKernels::WindowSubband) noexcept;

    /// <summary>
    /// What the filterbank carries into the next frame: granule 1's polyphase output with the
    /// amplitude filter applied the way mdct_sub48 applies it.
    /// </summary>
    void MdctCarry(const AmpFilter& ampFilter, const MdctPolyphase& frame, SubbandSamples& carry) noexcept;

    /// <summary>What the priming of lame_encode_frame_init carries into the first frame.</summary>
    void MdctPrimeCarry(const AmpFilter& ampFilter, const sample_t* input, SubbandSamples& carry) noexcept;

    /// <summary>
    /// mdct_sub48 for one channel of a frame from its polyphase output and the state carried in
    /// from the previous frame (MdctCarry, MdctPrimeCarry for the first frame). Frames may run in
    /// any order once their carry is set.
    /// </summary>
    void MdctFromPolyphase(const AmpFilter& ampFilter, const SubbandSamples& carry, const MdctPolyphase& frame, const std::array<std::int32_t, 2>& blockType,
                           FrameLines& xr) noexcept;

    /// <summary>
    /// The priming of lame_encode_frame_init for one channel: runs the filterbank once with short
    /// blocks over 1152 zeros followed by the start of the first frame's input.
    /// </summary>
    void MdctPrime(const AmpFilter& ampFilter, MdctChannelState& state, const sample_t* input) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
