// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include <cstdint>
#include <new>

#include <opus.h>

#include "FrameCodec.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::Gui {

    namespace {

        int ApplicationOf(Application application) noexcept
        {
            switch (application) {
                case Application::Audio: return OPUS_APPLICATION_AUDIO;
                case Application::Voip: return OPUS_APPLICATION_VOIP;
                default: return OPUS_APPLICATION_RESTRICTED_LOWDELAY;
            }
        }

        class LibopusEncoder final : public FrameEncoder
        {
        public:
            ~LibopusEncoder() override
            {
                if (encoder_ != nullptr)
                    opus_encoder_destroy(encoder_);
            }

            bool Init(const EncoderSettings& settings) noexcept
            {
                int error = OPUS_OK;
                encoder_  = opus_encoder_create(48000, settings.channels, ApplicationOf(settings.application), &error);
                if (encoder_ == nullptr || error != OPUS_OK)
                    return false;
                return opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(settings.bitrate)) == OPUS_OK
                    && opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(settings.complexity)) == OPUS_OK
                    && opus_encoder_ctl(encoder_, OPUS_SET_VBR(settings.vbr ? 1 : 0)) == OPUS_OK;
            }

            std::int32_t Encode(const float* pcm, std::int32_t frameSize, std::uint8_t* packet, std::int32_t maxBytes) noexcept override
            {
                return opus_encode_float(encoder_, pcm, frameSize, packet, maxBytes);
            }

            void SetBitrate(std::int32_t bitrate) noexcept override { opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate)); }
            void SetComplexity(std::int32_t complexity) noexcept override { opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(complexity)); }

            std::int32_t Lookahead() noexcept override
            {
                opus_int32 value = 0;
                opus_encoder_ctl(encoder_, OPUS_GET_LOOKAHEAD(&value));
                return value;
            }

            std::uint32_t FinalRange() noexcept override
            {
                opus_uint32 value = 0;
                opus_encoder_ctl(encoder_, OPUS_GET_FINAL_RANGE(&value));
                return value;
            }

        private:
            OpusEncoder* encoder_ = nullptr;
        };

        class LibopusDecoder final : public FrameDecoder
        {
        public:
            ~LibopusDecoder() override
            {
                if (decoder_ != nullptr)
                    opus_decoder_destroy(decoder_);
            }

            bool Init(std::int32_t channels) noexcept
            {
                int error = OPUS_OK;
                decoder_  = opus_decoder_create(48000, channels, &error);
                return decoder_ != nullptr && error == OPUS_OK;
            }

            std::int32_t Decode(const std::uint8_t* packet, std::int32_t bytes, float* pcm, std::int32_t maxFrames) noexcept override
            {
                return opus_decode_float(decoder_, packet, packet != nullptr ? bytes : 0, pcm, maxFrames, 0);
            }

        private:
            OpusDecoder* decoder_ = nullptr;
        };

    } // namespace

    FrameEncoder* CreateLibopusEncoder(const EncoderSettings& settings) noexcept
    {
        auto* encoder = new (std::nothrow) LibopusEncoder;
        if (encoder != nullptr && !encoder->Init(settings)) {
            delete encoder;
            encoder = nullptr;
        }
        return encoder;
    }

    FrameDecoder* CreateLibopusDecoder(std::int32_t channels) noexcept
    {
        auto* decoder = new (std::nothrow) LibopusDecoder;
        if (decoder != nullptr && !decoder->Init(channels)) {
            delete decoder;
            decoder = nullptr;
        }
        return decoder;
    }

    const char* LibopusVersion() noexcept
    {
        return opus_get_version_string();
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::Gui
