// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <cstddef>
#include <cstdint>

#include "FrameData.h"
#include "Profiling.h"
#include "QuantizeTypes.h"
#include "SimdFeatures.h"
#include "Stages.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    struct SessionConfig;

    /// <summary>
    /// The encoder, port of lame_encode_mp3_frame (encoder.c) and the parts of lame.c around it.
    /// It owns the constants and the state of all stages (Stages.h). A frame goes through four
    /// calls: Prepare (input, psychoacoustic model without state), Resolve (psychoacoustic model
    /// with state), Search (MDCT, stereo decision, quantization without the reservoir) and
    /// Encode (quantization with the reservoir, bit stream). Resolve and Encode take the frames
    /// in order; Prepare and Search have no state and may run for several frames at once, in any
    /// order. The calls may run in different tasks. EncodeFrame does all
    /// four on a FrameData of its own. It is large, so it is meant to live on the heap.
    /// </summary>
    class Encoder
    {
    public:
        /// <summary>
        /// The end of lame_init_params: lame_init_bitstream (with InitVbrTag), iteration_init and
        /// psymodel_init. Like LAME these may still change the configuration (write_lame_tag,
        /// msfix). The configuration must outlive the encoder. simd selects the bit-identical
        /// SIMD kernels; the caller makes sure the CPU supports the level (ChooseSimdLevel).
        /// </summary>
        void Initialize(SessionConfig& config, SimdLevel simd = SimdLevel::Scalar) noexcept;

        /// <summary>
        /// Bytes in the bit stream that are not frames: the placeholder of the LAME tag. LAME
        /// hands them out before the first frame without counting them as music.
        /// </summary>
        std::int32_t TakeHeaderBytes(std::byte* buffer, std::int32_t size) noexcept;

        /// <summary>
        /// The start of lame_encode_mp3_frame without state: the input and the psychoacoustic
        /// model's preparation, the polyphase filterbank. input is the window of each channel
        /// (mfbuf), which must stay valid until Resolve of the frame has run. frame.frameNumber
        /// must be set. Reads only the constants.
        /// </summary>
        void Prepare(const ChannelSamples& input, FrameData& frame) const noexcept;

        /// <summary>The psychoacoustic model with state and the filterbank state carried between frames, for prepared frames in order.</summary>
        void Resolve(FrameData& frame) noexcept;

        /// <summary>
        /// MDCT, stereo decision and quantization search of a resolved frame. Reads only the
        /// constants, so it may run for several frames at once, in any order.
        /// </summary>
        void Search(FrameData& frame) const noexcept;

        /// <summary>
        /// The end of lame_encode_mp3_frame, for searched frames in order: quantization with the
        /// reservoir and bit stream. The bytes the frame hands out are in frame.output. False on
        /// LAME's "INTERNAL ERROR IN VBR NEW CODE".
        /// </summary>
        bool Encode(FrameData& frame) noexcept;

        /// <summary>
        /// lame_encode_mp3_frame: Prepare, Resolve, Search and Encode on the encoder's own FrameData. Returns the
        /// bytes written to buffer, -1 if buffer is too small, -2 on LAME's "INTERNAL ERROR IN
        /// VBR NEW CODE".
        /// </summary>
        std::int32_t EncodeFrame(const ChannelSamples& input, std::byte* buffer, std::int32_t size) noexcept;

        /// <summary>The end of lame_encode_flush: completes the last frames with ancillary data.</summary>
        std::int32_t Flush(std::byte* buffer, std::int32_t size) noexcept;

        /// <summary>encoder_padding, known once the sample stream has been flushed.</summary>
        void SetEncoderPadding(std::int32_t padding) noexcept { bitstream_.result.encoder_padding = padding; }

        /// <summary>lame_get_lametag_frame: the final tag frame, see GetLameTagFrame.</summary>
        std::size_t GetLameTagFrame(std::byte* buffer, std::size_t size) const noexcept;

        const EncoderResult& Result() const noexcept { return bitstream_.result; }

        /// <summary>Times the stages into profile (CTLAME_PROFILE); null switches it off.</summary>
        void SetProfile(StageProfile* profile) noexcept { profile_ = profile; }

    private:
        const SessionConfig* config_  = nullptr;
        StageProfile*        profile_ = nullptr;

        EncoderConstants    constants_;
        PsyStageState       psy_;
        MdctStageState      mdct_;
        QuantizeStageState  quantize_;
        BitstreamStageState bitstream_;
        FrameData           frame_;
        std::int64_t        frames_ = 0; // frames through EncodeFrame
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
