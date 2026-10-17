// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

#include "HeapArray.h"
#include "OpusStreamEncoder.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    /// <summary>
    /// Port of libopus' OpusMSEncoder (src/opus_multistream_encoder.c, src/opus_multistream.c)
    /// for the channel mapping families opusenc uses: 0 (mono/stereo), 1 (Vorbis layouts up to
    /// 7.1, with surround masking analysis and an LFE stream) and 255 (independent mono streams).
    /// Ambisonics (family 2) is not ported.
    /// </summary>
    class OpusMultistreamEncoder
    {
    public:
        OpusMultistreamEncoder() noexcept                                = default;
        OpusMultistreamEncoder(const OpusMultistreamEncoder&)            = delete;
        OpusMultistreamEncoder& operator=(const OpusMultistreamEncoder&) = delete;
        ~OpusMultistreamEncoder() { delete[] streams_; }

        /// <summary>opus_multistream_surround_encoder_init; returns an OPUS error code.</summary>
        std::int32_t InitSurround(std::int32_t fs, std::int32_t channels, std::int32_t mappingFamily, std::int32_t& streams, std::int32_t& coupledStreams,
                                  std::uint8_t* mapping, std::int32_t application) noexcept;

        /// <summary>opus_multistream_encoder_ctl for requests with an int value.</summary>
        std::int32_t Ctl(std::int32_t request, std::int32_t value) noexcept;

        /// <summary>opus_multistream_encoder_ctl for requests with an int result.</summary>
        std::int32_t Get(std::int32_t request, std::int32_t& value) noexcept;

        /// <summary>The stream encoder (OPUS_MULTISTREAM_GET_ENCODER_STATE).</summary>
        /// <summary>The input channels the tonality analysis of a stream downmixes (c2 -1 for a mono stream).</summary>
        void AnalysisChannels(std::int32_t streamId, std::int32_t& c1, std::int32_t& c2) const noexcept
        {
            c1 = streamId < nbCoupled_ ? LeftChannel(streamId) : MonoChannel(streamId);
            c2 = streamId < nbCoupled_ ? RightChannel(streamId) : -1;
        }

        /// <summary>The LSB depth the stream encoders get (opus_multistream_encode_float).</summary>
        static constexpr std::int32_t InputLsbDepth = 24;

        std::int32_t       NbStreams() const noexcept { return nbStreams_; }
        OpusStreamEncoder* Stream(std::int32_t streamId) noexcept { return streamId >= 0 && streamId < nbStreams_ ? &streams_[streamId] : nullptr; }

        /// <summary>opus_multistream_encode_float.</summary>
        std::int32_t EncodeFloat(const float* pcm, std::int32_t analysisFrameSize, std::uint8_t* data, std::int32_t maxDataBytes) noexcept;

    private:
        static constexpr std::int32_t MaxOverlap = 120;

        void         SurroundAnalysis(const float* pcm, float* bandLogE, std::int32_t len) noexcept;
        std::int32_t RateAllocation(std::int32_t* rate, std::int32_t frameSize) noexcept;
        std::int32_t LeftChannel(std::int32_t streamId) const noexcept;
        std::int32_t RightChannel(std::int32_t streamId) const noexcept;
        std::int32_t MonoChannel(std::int32_t streamId) const noexcept;

        OpusStreamEncoder* streams_          = nullptr;
        std::int32_t       nbChannels_       = 0;
        std::int32_t       nbStreams_        = 0;
        std::int32_t       nbCoupled_        = 0;
        std::uint8_t       mapping_[256]     = {};
        std::int32_t       lfeStream_        = -1;
        bool               surround_         = false; // MAPPING_TYPE_SURROUND
        std::int32_t       bitrateBps_       = 0;
        std::int32_t       application_      = 0;
        std::int32_t       fs_               = 0;
        std::int32_t       variableDuration_ = 0;
        HeapArray<float>   windowMem_;  // channels * MaxOverlap
        HeapArray<float>   preemphMem_; // channels
        HeapArray<float>   streamPcm_;  // one stream's frame (EncodeFloat)
        HeapArray<float>   bandSmr_;    // the surround masking of a frame
    };

    /// <summary>frame_size_select of opus_encoder.c.</summary>
    std::int32_t FrameSizeSelect(std::int32_t application, std::int32_t frameSize, std::int32_t variableDuration, std::int32_t fs) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
