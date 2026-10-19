// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "Mdct.h"
#include "Psy.h"
#include "QuantizeTypes.h"
#include "Types.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>Samples per channel a frame reads from mfbuf (calcNeeded: the FFT of granule 1 reaches furthest).</summary>
    inline constexpr std::int32_t FrameInputSize = Max(BLKSIZE + FRAMESIZE - FFTOFFSET, 512 + FRAMESIZE - 32);

    /// <summary>Upper bound of a frame's main data: 4 granules and channels of at most 4095 bits.</summary>
    inline constexpr std::int32_t MaxMainDataBytes = (4 * 4095 + 7) / 8;

    /// <summary>Upper bound of the bytes one frame hands out (LAME_MAXMP3BUFFER).</summary>
    inline constexpr std::int32_t MaxFrameBytes = 16384;

    /// <summary>
    /// Everything that belongs to one frame and passes from stage to stage. The
    /// input points into the sample stream, which keeps the window until the frame
    /// is released; what is carried from frame to frame lives in the state of the stages, never
    /// here. The stages fill the fields in this order: input, psychoacoustic model, MDCT, stereo
    /// decision, quantization, bit stream.
    /// </summary>
    struct FrameData {
        // --- input: the frame's PCM as mfbuf holds it, FrameInputSize samples per channel (input[1] is null for mono)
        std::int64_t   frameNumber;
        ChannelSamples input;

        // --- psychoacoustic model without state (PsyPrepare for both granules)
        std::array<PsyPrep, 2> psyPrep;

        // --- psychoacoustic model (L3psycho_anal_vbr for both granules, adjust_ATH)
        std::array<std::array<PsyRatio, 2>, 2> maskingLr;       // masking and energy per granule and channel, L/R
        std::array<std::array<PsyRatio, 2>, 2> maskingMs;       // the same for mid/side (joint stereo only)
        std::array<std::array<float, 2>, 2>    pe;              // perceptual entropy L/R
        std::array<std::array<float, 2>, 2>    peMs;            // perceptual entropy M/S
        std::array<std::array<float, 4>, 2>    energy;          // tot_ener per granule: L, R, M, S
        float                                  athAdjustFactor; // ATH adjustment for the quantization of this frame

        // --- MDCT: the polyphase output of both granules (ahead, no state), the state carried in
        // from the previous frame (resolved in order); the result: side.tt[gr][ch].xr
        std::array<MdctPolyphase, 2>                              polyphase;
        std::array<std::array<std::array<float, SBLIMIT>, 18>, 2> mdctCarry;

        // --- stereo decision
        std::int32_t modeExt; // MPG_MD_LR_LR or MPG_MD_MS_LR

        // --- quantization search (no reservoir): side.tt holds the granules quantized "as is"
        std::array<std::array<std::array<float, 576>, 2>, 2>    xrpow;         // |xr|^(3/4)
        std::array<std::array<std::array<float, SFBMAX>, 2>, 2> xmin;          // allowed distortion per band
        std::array<std::array<GranuleSearch, 2>, 2>             search;        // for the out-of-bits strategy of the commit
        bool                                                    analogSilence; // no band above the ATH

        // --- quantization commit (with the reservoir)
        SideInfo     side;
        std::int32_t bitrateIndex;

        // --- main data coded ahead (PrepareMainData), valid unless the commit changed the granules
        std::array<std::uint8_t, MaxMainDataBytes> mainData;
        std::int32_t                               mainDataBits;
        bool                                       mainDataValid;

        // --- bit stream: the bytes this frame hands out (they may belong to earlier frames' main data)
        std::array<std::byte, MaxFrameBytes> output;
        std::int32_t                         outputSize;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
