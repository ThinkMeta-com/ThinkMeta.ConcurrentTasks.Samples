// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <array>
#include <cstdint>

#include "Bitstream.h"
#include "FrameData.h"
#include "Mdct.h"
#include "Psy.h"
#include "Quantize.h"
#include "QuantizeTypes.h"
#include "VbrTag.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    struct SessionConfig;

    // The encoder as stages. Each stage reads and writes one FrameData; a stage
    // with state keeps what it carries from frame to frame in its own state and takes the frames
    // in order. Between the stages of one frame the order is input, psy preparation, psy, MDCT,
    // stereo, quantization search, quantization, bit stream. The stages without state (input,
    // psy preparation, MDCT, stereo, quantization search) may run for several frames at once
    //. One dependency crosses stages over frames: the reservoir.
    // QuantizeStage owns it, BitstreamStage of frame N sets main_data_begin that
    // QuantizeStage of frame N + 1 drains (ReservoirFrameEnd).

    /// <summary>What is computed once per session: tables of the psychoacoustic model, the ATH, the quantizer.</summary>
    struct EncoderConstants {
        PsyConstants                       psy;
        AthTables                          ath;
        QuantizeConstants                  quantize;
        MdctKernels::WindowSubbandFunction windowSubband; // of the SIMD level (Encoder::Initialize)
    };

    /// <summary>
    /// lame_encode_mp3_frame's start: points the frame to its input (the window in the sample
    /// stream, as mfbuf holds it) and clears what LAME initializes per frame.
    /// </summary>
    void InputStage(const SessionConfig& config, const ChannelSamples& input, FrameData& frame) noexcept;

    /// <summary>
    /// Stage 1a: the part of the psychoacoustic model without state (PsyPrepare) for both
    /// granules: attack energies, FFTs, spreading. No state; frames in any order.
    /// </summary>
    void PsyPrepStage(const SessionConfig& config, const EncoderConstants& constants, FrameData& frame) noexcept;

    /// <summary>State of the psychoacoustic model stage.</summary>
    struct PsyStageState {
        const EncoderConstants* constants;
        PsyState                psy;
        AthAdjust               ath;
        PsyResult               result;
        bool                    firstFrame; // masking_lower is 1 for the first frame
    };

    /// <summary>
    /// Stage 1b: the psychoacoustic model with state for both granules (PsyResolve: masking,
    /// perceptual entropy, block types) and the ATH adjustment that the frame's quantization
    /// uses. Needs PsyPrepStage of the frame.
    /// </summary>
    void PsyStage(const SessionConfig& config, PsyStageState& state, FrameData& frame) noexcept;

    /// <summary>Stage 2a: the polyphase filterbank of both granules (MdctPolyphaseFrame). No state; frames in any order.</summary>
    void MdctPolyphaseStage(const SessionConfig& config, const EncoderConstants& constants, FrameData& frame) noexcept;

    /// <summary>State of the MDCT carry: what the filterbank of each channel hands to the next frame.</summary>
    struct MdctStageState {
        std::array<std::array<std::array<float, SBLIMIT>, 18>, 2> carry;
    };

    /// <summary>
    /// Stage 2b, for the frames in order: hands the filterbank state of the previous frame to
    /// this one (frame 0, by frame.frameNumber, gets the priming of lame_encode_frame_init) and
    /// keeps this frame's for the next. Needs stage 2a of the frame.
    /// </summary>
    void MdctCarryStage(const SessionConfig& config, MdctStageState& state, FrameData& frame) noexcept;

    /// <summary>Stage 2c: the MDCT with the block types from stage 1. No state; needs stage 2b of the frame.</summary>
    void MdctStage(const SessionConfig& config, FrameData& frame) noexcept;

    /// <summary>Stage 3: M/S or L/R, from the perceptual entropy and the block types. No state.</summary>
    void StereoStage(const SessionConfig& config, FrameData& frame) noexcept;

    /// <summary>
    /// Stage 4a: the quantization without the reservoir (QuantSearch): M/S conversion, allowed
    /// distortion, scalefactor search, quantization "as is". No state; frames in any order.
    /// </summary>
    void QuantSearchStage(const SessionConfig& config, const EncoderConstants& constants, FrameData& frame) noexcept;

    /// <summary>State of the quantization stage: the reservoir and substep shaping.</summary>
    struct QuantizeStageState {
        const EncoderConstants* constants;
        QuantizeState           quantize;
    };

    /// <summary>
    /// Stage 4b: the quantization with the bit reservoir (QuantCommit): the bits the frame may
    /// use, the out-of-bits strategy if needed and the choice of the bitrate. Returns false on
    /// LAME's "INTERNAL ERROR IN VBR NEW CODE".
    /// </summary>
    bool QuantizeStage(const SessionConfig& config, QuantizeStageState& state, FrameData& frame) noexcept;

    /// <summary>State of the bit stream stage: the stream with its headers, the LAME tag data and the statistics.</summary>
    struct BitstreamStageState {
        BitstreamState  bitstream;
        ReservoirState* reservoir; // owned by QuantizeStageState
        VbrTagState     tag;
        EncoderResult   result;
    };

    /// <summary>
    /// Stage 5: formats the frame into the bit stream and hands out the bytes that are complete
    /// (frame.output); updates music CRC, seek table and statistics.
    /// </summary>
    void BitstreamStage(const SessionConfig& config, BitstreamStageState& state, FrameData& frame) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
