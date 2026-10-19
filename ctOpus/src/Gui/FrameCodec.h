// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstdint>

// The codecs behind one interface, so that the ctOpus constants (OpusDefines.h) and the libopus
// macros (opus.h) never meet in one translation unit.

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::Gui {

    enum class EncoderKind { CtOpus, Libopus };

    enum class Application { RestrictedLowDelay, Audio, Voip };

    struct EncoderSettings {
        std::int32_t channels    = 2;
        Application  application = Application::RestrictedLowDelay;
        std::int32_t bitrate     = 128000;
        std::int32_t complexity  = 10;
        bool         vbr         = true;
    };

    /// <summary>One Opus stream encoder (opus_encode_float semantics, 48 kHz, one frame per call).</summary>
    class FrameEncoder
    {
    public:
        virtual ~FrameEncoder() = default;

        /// <summary>Encodes frameSize interleaved frames into one packet; returns its length or a negative OPUS error.</summary>
        virtual std::int32_t Encode(const float* pcm, std::int32_t frameSize, std::uint8_t* packet, std::int32_t maxBytes) noexcept = 0;

        virtual void SetBitrate(std::int32_t bitrate) noexcept       = 0;
        virtual void SetComplexity(std::int32_t complexity) noexcept = 0;

        /// <summary>OPUS_GET_LOOKAHEAD in samples at 48 kHz.</summary>
        virtual std::int32_t Lookahead() noexcept = 0;

        /// <summary>OPUS_GET_FINAL_RANGE of the last packet.</summary>
        virtual std::uint32_t FinalRange() noexcept = 0;
    };

    /// <summary>The ctOpus encoder under test (OpusStreamEncoder); nullptr on failure.</summary>
    FrameEncoder* CreateCtOpusEncoder(const EncoderSettings& settings) noexcept;

    /// <summary>The libopus encoder (opus_encoder_create) as the comparison engine; nullptr on failure.</summary>
    FrameEncoder* CreateLibopusEncoder(const EncoderSettings& settings) noexcept;

    inline FrameEncoder* CreateEncoder(EncoderKind kind, const EncoderSettings& settings) noexcept
    {
        return kind == EncoderKind::CtOpus ? CreateCtOpusEncoder(settings) : CreateLibopusEncoder(settings);
    }

    /// <summary>The libopus decoder for monitoring the encoded stream.</summary>
    class FrameDecoder
    {
    public:
        virtual ~FrameDecoder() = default;

        /// <summary>Decodes a packet (nullptr: packet loss concealment); returns the frames decoded or a negative OPUS error.</summary>
        virtual std::int32_t Decode(const std::uint8_t* packet, std::int32_t bytes, float* pcm, std::int32_t maxFrames) noexcept = 0;
    };

    FrameDecoder* CreateLibopusDecoder(std::int32_t channels) noexcept;

    /// <summary>The version string of the linked libopus.</summary>
    const char* LibopusVersion() noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::Gui
