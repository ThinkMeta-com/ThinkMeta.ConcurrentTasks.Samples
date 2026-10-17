// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

#include "HeapArray.h"
#include "OggPacker.h"
#include "OpusHeaders.h"
#include "OpusMultistreamEncoder.h"
#include "Resampler.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    // libopusenc error codes (opusenc.h)
    constexpr std::int32_t OpeOk            = 0;
    constexpr std::int32_t OpeBadArg        = -11;
    constexpr std::int32_t OpeInternalError = -13;
    constexpr std::int32_t OpeUnimplemented = -15;
    constexpr std::int32_t OpeAllocFail     = -17;
    constexpr std::int32_t OpeCannotOpen    = -30;
    constexpr std::int32_t OpeTooLate       = -31;
    constexpr std::int32_t OpeWriteFail     = -34;
    constexpr std::int32_t OpeCloseFail     = -35;

    // libopusenc-specific encoder requests (opusenc.h)
    constexpr std::int32_t OpeSetDecisionDelayRequest  = 14000;
    constexpr std::int32_t OpeSetMuxingDelayRequest    = 14002;
    constexpr std::int32_t OpeSetCommentPaddingRequest = 14004;
    constexpr std::int32_t OpeSetSerialNoRequest       = 14006;
    constexpr std::int32_t OpeSetHeaderGainRequest     = 14010;

    /// <summary>ope_strerror.</summary>
    const char* OpeErrorString(std::int32_t error) noexcept;

    /// <summary>
    /// extend_signal of libopusenc: continues the before interleaved frames ahead of x by after
    /// LPC-predicted, windowed frames (the drain padding).
    /// </summary>
    void LpcExtend(float* x, std::int32_t before, std::int32_t after, std::int32_t channels) noexcept;

    constexpr std::int32_t LpcExtensionInput  = 480; // LPC_INPUT
    constexpr std::int32_t LpcExtensionLength = 120; // LPC_PADDING

    /// <summary>
    /// Port of libopusenc 0.2.1 (opusenc.c) for what opusenc uses: one Ogg Opus stream written
    /// through callbacks, the input resampled to 48 kHz, packets encoded by the multistream
    /// encoder as soon as the decision delay allows, and the end padded by LPC extension.
    /// </summary>
    class OggOpusEncoder
    {
    public:
        struct Callbacks {
            void* user = nullptr;
            /// <summary>Writes a page; false on failure (OpusEncCallbacks.write).</summary>
            bool (*write)(void* user, const std::uint8_t* data, std::int32_t length) = nullptr;
            /// <summary>Closes the output at the end of the stream; false on failure.</summary>
            bool (*close)(void* user) = nullptr;
            /// <summary>Optional: called for every packet (OPE_SET_PACKET_CALLBACK).</summary>
            void (*packet)(void* user, const std::uint8_t* packet, std::int32_t length) = nullptr;
        };

        OggOpusEncoder() noexcept                        = default;
        OggOpusEncoder(const OggOpusEncoder&)            = delete;
        OggOpusEncoder& operator=(const OggOpusEncoder&) = delete;
        ~OggOpusEncoder();

        /// <summary>ope_encoder_create_callbacks; returns an OPE error code.</summary>
        std::int32_t Create(const Callbacks& callbacks, const OpusTags& comments, std::int32_t rate, std::int32_t channels, std::int32_t family) noexcept;

        /// <summary>ope_encoder_ctl with an integer value: OPUS_SET_* requests passed to the encoder and the OPE_SET_* requests.</summary>
        std::int32_t Ctl(std::int32_t request, std::int32_t value) noexcept;

        /// <summary>ope_encoder_ctl for OPUS_GET_* requests with an integer result.</summary>
        std::int32_t Get(std::int32_t request, std::int32_t& value) noexcept;

        /// <summary>OPUS_SET_EXPERT_FRAME_DURATION through ope_encoder_ctl (also sets the libopusenc frame size).</summary>
        std::int32_t SetExpertFrameDuration(std::int32_t value) noexcept;

        std::int32_t NbStreams() const noexcept { return header_.nbStreams; }
        std::int32_t NbCoupledStreams() const noexcept { return header_.nbCoupled; }
        /// <summary>opus_encoder_ctl on one stream (OPUS_MULTISTREAM_GET_ENCODER_STATE); an OPUS error code.</summary>
        std::int32_t StreamCtl(std::int32_t streamId, std::int32_t request, std::int32_t value) noexcept;

        /// <summary>ope_encoder_write_float: adds interleaved samples at the input rate.</summary>
        std::int32_t WriteFloat(const float* pcm, std::int32_t samplesPerChannel) noexcept;

        /// <summary>ope_encoder_drain: pads, encodes the rest and finishes the stream.</summary>
        std::int32_t Drain() noexcept;

        // --- input resampled elsewhere (the parallel resampler) ---------------------------------

        /// <summary>True if the input rate is not 48 kHz.</summary>
        bool             Resamples() const noexcept { return resample_; }
        const Resampler& GetResampler() const noexcept { return resampler_; }

        /// <summary>
        /// The number of 48 kHz frames ope_encoder_drain adds: the padding after the resampled
        /// input. lookahead is OPUS_GET_LOOKAHEAD after all settings (the stream's preskip).
        /// </summary>
        std::int32_t DrainFrames(std::int32_t lookahead) const noexcept;

        /// <summary>
        /// The input length in input-rate frames (write_granule). Before the input has ended, any
        /// value at least as large as the input so far; libopusenc only uses it for the end of stream.
        /// </summary>
        void SetInputLength(std::int64_t frames) noexcept { writeGranule_ = endGranule_ = frames; }

        /// <summary>Adds 48 kHz frames that the resampler produced from the input (the loop of ope_encoder_write_float).</summary>
        std::int32_t WriteResampled(const float* pcm, std::int32_t frames) noexcept;

        /// <summary>ope_encoder_drain with the resampler's drain output (DrainFrames frames) given.</summary>
        std::int32_t DrainResampled(const float* pad, std::int32_t frames) noexcept;

        /// <summary>
        /// A buffer for extra more frames, so it is moved back to the front less often; only where
        /// that does not change the output: when every analysis before the drain sees at least the
        /// frames run_analysis clamps to. False if out of memory.
        /// </summary>
        bool EnlargeBuffer(std::int32_t extra) noexcept;

        // --- what a chain ahead of the encoder needs (read before the encoding starts) --------

        /// <summary>
        /// The schedule of the encode calls before the input ends: the frame size, the frame
        /// duration request, the decision delay and the input rate.
        /// </summary>
        struct Schedule {
            std::int32_t frameSize;
            std::int32_t frameSizeRequest;
            std::int32_t decisionDelay;
            std::int32_t rate;
        };
        Schedule GetSchedule() const noexcept { return {frameSize_, frameSizeRequest_, decisionDelay_, rate_}; }

        /// <summary>The frames of an OPUS_FRAMESIZE_* request.</summary>
        static std::int32_t FrameSamples(std::int32_t request) noexcept;

        OpusMultistreamEncoder&       Multistream() noexcept { return encoder_; }
        const OpusMultistreamEncoder& Multistream() const noexcept { return encoder_; }
        std::int32_t                  Channels() const noexcept { return channels_; }

    private:
        void         InitStream() noexcept;
        void         EncodeBuffer() noexcept;
        void         ShiftBuffer() noexcept;
        bool         OutputPages() noexcept;
        bool         FlushPage() noexcept;
        std::int32_t OpusCtl(std::int32_t request, std::int32_t value) noexcept;
        std::int32_t OpusGet(std::int32_t request, std::int32_t& value) noexcept;
        std::int32_t OpusEncode(const float* pcm, std::int32_t analysisFrameSize, std::uint8_t* data, std::int32_t maxDataBytes) noexcept;

        Callbacks              callbacks_;
        OpusMultistreamEncoder encoder_;
        OggPacker              packer_;
        bool                   packerCreated_ = false;
        Resampler              resampler_;
        bool                   resample_      = false;
        std::int32_t           unrecoverable_ = 0;
        std::int32_t           rate_          = 0;
        std::int32_t           channels_      = 0;
        HeapArray<float>       buffer_;
        std::int32_t           bufferStart_         = 0;
        std::int32_t           bufferEnd_           = 0;
        std::int32_t           bufferSamples_       = 0; // the frames the buffer holds
        std::int32_t           frameSize_           = 960;
        std::int32_t           frameSizeRequest_    = 0;
        std::int32_t           decisionDelay_       = 96000;
        std::int32_t           maxOggDelay_         = 48000;
        std::int32_t           globalGranuleOffset_ = -1;
        std::int64_t           currGranule_         = 0;
        std::int64_t           writeGranule_        = 0;
        bool                   draining_            = false;
        HeapArray<float>       lpcBuffer_;
        OpusHead               header_;
        std::int32_t           commentPadding_ = 512;

        // the single EncStream
        bool         streamOpen_     = false; // enc->streams != NULL
        bool         serialNoIsSet_  = false;
        std::int32_t serialNo_       = 0;
        bool         streamIsInit_   = false;
        bool         headerIsFrozen_ = false;
        std::int64_t endGranule_     = 0;
        OpusTags     comment_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
