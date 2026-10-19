// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include <cstdint>
#include <new>

#include "FrameCodec.h"
#include "OpusDefines.h"
#include "OpusStreamEncoder.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::Gui {

    namespace {

        std::int32_t ApplicationOf(Application application) noexcept
        {
            switch (application) {
                case Application::Audio: return OPUS_APPLICATION_AUDIO;
                case Application::Voip: return OPUS_APPLICATION_VOIP;
                default: return OPUS_APPLICATION_RESTRICTED_LOWDELAY;
            }
        }

        // OpusStreamEncoder driven like opus_encode_float: 24-bit LSB depth, the whole input as the
        // analysis input, downmixed over all channels (c2 = -2).
        class CtOpusEncoder final : public FrameEncoder
        {
        public:
            bool Init(const EncoderSettings& settings) noexcept
            {
                channels_ = settings.channels;
                if (encoder_.Init(48000, settings.channels, ApplicationOf(settings.application)) != OPUS_OK)
                    return false;
                return encoder_.Ctl(OPUS_SET_BITRATE_REQUEST, settings.bitrate) == OPUS_OK
                    && encoder_.Ctl(OPUS_SET_COMPLEXITY_REQUEST, settings.complexity) == OPUS_OK
                    && encoder_.Ctl(OPUS_SET_VBR_REQUEST, settings.vbr ? 1 : 0) == OPUS_OK;
            }

            std::int32_t Encode(const float* pcm, std::int32_t frameSize, std::uint8_t* packet, std::int32_t maxBytes) noexcept override
            {
                return encoder_.EncodeNative(pcm, frameSize, packet, maxBytes, 24, pcm, frameSize, 0, -2, channels_);
            }

            void SetBitrate(std::int32_t bitrate) noexcept override { encoder_.Ctl(OPUS_SET_BITRATE_REQUEST, bitrate); }
            void SetComplexity(std::int32_t complexity) noexcept override { encoder_.Ctl(OPUS_SET_COMPLEXITY_REQUEST, complexity); }

            std::int32_t Lookahead() noexcept override
            {
                std::int32_t value = 0;
                encoder_.Get(OPUS_GET_LOOKAHEAD_REQUEST, value);
                return value;
            }

            std::uint32_t FinalRange() noexcept override { return encoder_.FinalRange(); }

        private:
            OpusStreamEncoder encoder_;
            std::int32_t      channels_ = 2;
        };

    } // namespace

    FrameEncoder* CreateCtOpusEncoder(const EncoderSettings& settings) noexcept
    {
        auto* encoder = new (std::nothrow) CtOpusEncoder;
        if (encoder != nullptr && !encoder->Init(settings)) {
            delete encoder;
            encoder = nullptr;
        }
        return encoder;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::Gui
