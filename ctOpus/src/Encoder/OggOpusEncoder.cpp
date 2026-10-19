// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "OggOpusEncoder.h"
#include "OpusDefines.h"
#include "OpusPacket.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    namespace {

        constexpr std::int32_t LpcPadding       = 120;
        constexpr std::int32_t LpcOrder         = 24;
        constexpr std::int32_t LpcInput         = 480;
        constexpr float        LpcGoertzelConst = 1.99931465f;
        constexpr std::int32_t MaxLookahead     = 96000;
        constexpr std::int32_t BufferExtra      = 24000;
        constexpr std::int32_t BufferSamples    = MaxLookahead + BufferExtra;

        // libopusenc: compute_frame_samples() (src/opusenc.c)
        std::int32_t ComputeFrameSamples(std::int32_t request) noexcept
        {
            if (request <= OPUS_FRAMESIZE_40_MS)
                return 120 << (request - OPUS_FRAMESIZE_2_5_MS);
            return (request - OPUS_FRAMESIZE_2_5_MS - 2) * 960;
        }

        std::int32_t OpusToOpeError(std::int32_t ret) noexcept
        {
            switch (ret) {
                case OPUS_BAD_ARG: return OpeBadArg;
                case OPUS_INTERNAL_ERROR: return OpeInternalError;
                case OPUS_UNIMPLEMENTED: return OpeUnimplemented;
                case OPUS_ALLOC_FAIL: return OpeAllocFail;
                default: return OpeInternalError;
            }
        }

        // libopusenc: vorbis_lpc_from_data() (src/opusenc.c): LPC coefficients by autocorrelation and Levinson-Durbin.
        void LpcFromData(const float* data, float* lpci, std::int32_t n, std::int32_t stride) noexcept
        {
            double aut[LpcOrder + 1];
            double lpc[LpcOrder];

            std::int32_t j = LpcOrder + 1;
            while (j--) {
                double d = 0; // double needed for accumulator depth
                for (std::int32_t i = j; i < n; ++i)
                    d += static_cast<double>(data[i * stride]) * data[(i - j) * stride];
                aut[j] = d;
            }
            // lag windowing
            for (std::int32_t i = 1; i <= LpcOrder; ++i)
                aut[i] -= aut[i] * (0.008f * 0.008f) * i * i;

            double       error   = aut[0] * (1. + 1e-7);
            const double epsilon = 1e-6 * aut[0] + 1e-7;
            for (std::int32_t i = 0; i < LpcOrder; ++i) {
                double r = -aut[i + 1];
                if (error < epsilon) {
                    std::memset(lpc + i, 0, static_cast<std::size_t>(LpcOrder - i) * sizeof(lpc[0]));
                    break;
                }
                for (j = 0; j < i; ++j)
                    r -= lpc[j] * aut[i - j];
                r /= error;
                lpc[i] = r;
                for (j = 0; j < i / 2; ++j) {
                    const double tmp = lpc[j];
                    lpc[j] += r * lpc[i - 1 - j];
                    lpc[i - 1 - j] += r * tmp;
                }
                if (i & 1)
                    lpc[j] += lpc[j] * r;
                error *= 1. - r * r;
            }
            // slightly damp the filter
            const double g    = .999;
            double       damp = g;
            for (j = 0; j < LpcOrder; ++j) {
                lpc[j] *= damp;
                damp *= g;
            }
            for (j = 0; j < LpcOrder; ++j)
                lpci[j] = static_cast<float>(lpc[j]);
        }

        // libopusenc: extend_signal() (src/opusenc.c): continues the before samples ahead of x by after LPC-predicted, windowed samples.
        void ExtendSignal(float* x, std::int32_t before, std::int32_t after, std::int32_t channels) noexcept
        {
            if (after == 0)
                return;
            before = before < LpcInput ? before : LpcInput;
            if (before < 4 * LpcOrder) {
                for (std::int32_t i = 0; i < after * channels; ++i)
                    x[i] = 0;
                return;
            }
            float window[LpcPadding];
            {
                // window by a resonating IIR (Goertzel)
                float       m0 = 1;
                float       m1 = static_cast<float>(.5 * LpcGoertzelConst);
                const float a1 = LpcGoertzelConst;
                window[0]      = 1;
                for (std::int32_t i = 1; i < LpcPadding; ++i) {
                    window[i] = a1 * m0 - m1;
                    m1        = m0;
                    m0        = window[i];
                }
                for (std::int32_t i = 0; i < LpcPadding; ++i)
                    window[i] = static_cast<float>(.5 + .5 * window[i]);
            }
            for (std::int32_t c = 0; c < channels; ++c) {
                float lpc[LpcOrder];
                LpcFromData(x - channels * before + c, lpc, before, channels);
                for (std::int32_t i = 0; i < after; ++i) {
                    float sum = 0;
                    for (std::int32_t j = 0; j < LpcOrder; ++j)
                        sum -= x[(i - j - 1) * channels + c] * lpc[j];
                    x[i * channels + c] = sum;
                }
                for (std::int32_t i = 0; i < after; ++i)
                    x[i * channels + c] *= window[i];
            }
        }

    } // namespace

    void LpcExtend(float* x, std::int32_t before, std::int32_t after, std::int32_t channels) noexcept
    {
        ExtendSignal(x, before, after, channels);
    }

    // libopusenc: ope_strerror() (src/opusenc.c)
    const char* OpeErrorString(std::int32_t error) noexcept
    {
        static const char* const strings[] = {"cannot open file",     "call cannot be made at this point",
                                              "invalid picture file", "invalid icon file (pictures of type 1 MUST be 32x32 PNGs)",
                                              "write failed",         "close failed"};
        if (error == 0)
            return "success";
        if (error >= -10)
            return "unknown error";
        if (error > -30)
            return OpusStrError(error + 10);
        if (error >= OpeCloseFail)
            return strings[-error - 30];
        return "unknown error";
    }

    std::int32_t OggOpusEncoder::FrameSamples(std::int32_t request) noexcept
    {
        return ComputeFrameSamples(request);
    }

    OggOpusEncoder::~OggOpusEncoder()
    {
        // ope_encoder_destroy: a stream that did not end is closed without error check
        if (streamOpen_ && callbacks_.close != nullptr)
            callbacks_.close(callbacks_.user);
    }

    // libopusenc: ope_encoder_create_callbacks() (src/opusenc.c)
    std::int32_t OggOpusEncoder::Create(const Callbacks& callbacks, const OpusTags& comments, std::int32_t rate, std::int32_t channels,
                                        std::int32_t family) noexcept
    {
        if (family != 0 && family != 1 && family != 255 && family != -1)
            return family < -1 || family > 255 ? OpeBadArg : OpeUnimplemented;
        if (channels <= 0 || channels > 255 || rate <= 0)
            return OpeBadArg;
        if (!comment_.CopyFrom(comments))
            return OpeAllocFail;
        streamOpen_             = true;
        serialNoIsSet_          = false;
        streamIsInit_           = false;
        headerIsFrozen_         = false;
        unrecoverable_          = family == -1 ? OpeTooLate : 0;
        rate_                   = rate;
        channels_               = channels;
        frameSize_              = 960;
        frameSizeRequest_       = OPUS_FRAMESIZE_20_MS;
        decisionDelay_          = 96000;
        maxOggDelay_            = 48000;
        commentPadding_         = 512;
        header_.channels        = channels;
        header_.channelMapping  = family;
        header_.inputSampleRate = static_cast<std::uint32_t>(rate);
        header_.gain            = 0;
        if (family != -1) {
            int error     = OPUS_OK;
            int nbStreams = 0;
            int nbCoupled = 0;
            error         = encoder_.InitSurround(48000, channels, family, nbStreams, nbCoupled, header_.streamMap, OPUS_APPLICATION_AUDIO);
            if (error != OPUS_OK) {
                streamOpen_ = false;
                return OpusToOpeError(error);
            }
            header_.nbStreams = nbStreams;
            header_.nbCoupled = nbCoupled;
            OpusCtl(OPUS_SET_EXPERT_FRAME_DURATION_REQUEST, OPUS_FRAMESIZE_20_MS);
        }
        resample_ = rate != 48000;
        if (resample_ && !resampler_.Initialize(channels, static_cast<std::uint32_t>(rate), 48000)) {
            streamOpen_ = false;
            return OpeAllocFail;
        }
        globalGranuleOffset_ = -1;
        currGranule_         = 0;
        writeGranule_        = 0;
        draining_            = false;
        if (!buffer_.Allocate(static_cast<std::size_t>(BufferSamples) * channels)
            || (resample_ && !lpcBuffer_.Allocate(static_cast<std::size_t>(LpcInput + LpcPadding) * channels))) {
            streamOpen_ = false;
            return OpeAllocFail;
        }
        bufferStart_ = bufferEnd_ = 0;
        bufferSamples_            = BufferSamples;
        callbacks_                = callbacks;
        return OpeOk;
    }

    std::int32_t OggOpusEncoder::OpusCtl(std::int32_t request, std::int32_t value) noexcept
    {
        return encoder_.Ctl(request, value);
    }

    std::int32_t OggOpusEncoder::OpusGet(std::int32_t request, std::int32_t& value) noexcept
    {
        return encoder_.Get(request, value);
    }

    std::int32_t OggOpusEncoder::OpusEncode(const float* pcm, std::int32_t analysisFrameSize, std::uint8_t* data, std::int32_t maxDataBytes) noexcept
    {
        return encoder_.EncodeFloat(pcm, analysisFrameSize, data, maxDataBytes);
    }

    std::int32_t OggOpusEncoder::StreamCtl(std::int32_t streamId, std::int32_t request, std::int32_t value) noexcept
    {
        OpusStreamEncoder* stream = encoder_.Stream(streamId);
        return stream != nullptr ? stream->Ctl(request, value) : OPUS_BAD_ARG;
    }

    // libopusenc: ope_encoder_ctl() (src/opusenc.c), the SET requests
    std::int32_t OggOpusEncoder::Ctl(std::int32_t request, std::int32_t value) noexcept
    {
        if (unrecoverable_ != 0)
            return unrecoverable_;
        std::int32_t ret = OpeOk;
        switch (request) {
            case OPUS_SET_APPLICATION_REQUEST:
            case OPUS_SET_BITRATE_REQUEST:
            case OPUS_SET_MAX_BANDWIDTH_REQUEST:
            case OPUS_SET_VBR_REQUEST:
            case OPUS_SET_BANDWIDTH_REQUEST:
            case OPUS_SET_COMPLEXITY_REQUEST:
            case OPUS_SET_INBAND_FEC_REQUEST:
            case OPUS_SET_PACKET_LOSS_PERC_REQUEST:
            case OPUS_SET_DTX_REQUEST:
            case OPUS_SET_VBR_CONSTRAINT_REQUEST:
            case OPUS_SET_FORCE_CHANNELS_REQUEST:
            case OPUS_SET_SIGNAL_REQUEST:
            case OPUS_SET_LSB_DEPTH_REQUEST:
            case OPUS_SET_PREDICTION_DISABLED_REQUEST:
            case OPUS_SET_PHASE_INVERSION_DISABLED_REQUEST: ret = OpusCtl(request, value); break;
            case OPUS_SET_EXPERT_FRAME_DURATION_REQUEST:
                if (value < OPUS_FRAMESIZE_2_5_MS || value > OPUS_FRAMESIZE_120_MS) {
                    ret = OPUS_UNIMPLEMENTED;
                    break;
                }
                ret = OpusCtl(request, value);
                if (ret == OPUS_OK) {
                    frameSize_        = ComputeFrameSamples(value);
                    frameSizeRequest_ = value;
                }
                break;
            case OpeSetDecisionDelayRequest:
                if (value < 0) {
                    ret = OpeBadArg;
                    break;
                }
                decisionDelay_ = value < MaxLookahead ? value : MaxLookahead;
                break;
            case OpeSetMuxingDelayRequest:
                if (value < 0) {
                    ret = OpeBadArg;
                    break;
                }
                maxOggDelay_ = value;
                if (packerCreated_)
                    packer_.SetMuxingDelay(static_cast<std::uint64_t>(maxOggDelay_));
                break;
            case OpeSetCommentPaddingRequest:
                if (value < 0) {
                    ret = OpeBadArg;
                    break;
                }
                commentPadding_ = value;
                break;
            case OpeSetSerialNoRequest:
                if (!streamOpen_ || headerIsFrozen_) {
                    ret = OpeTooLate;
                    break;
                }
                serialNo_      = value;
                serialNoIsSet_ = true;
                break;
            case OpeSetHeaderGainRequest:
                if (!streamOpen_ || headerIsFrozen_) {
                    ret = OpeTooLate;
                    break;
                }
                header_.gain = value;
                break;
            default: ret = OPUS_UNIMPLEMENTED; break;
        }
        if (ret != 0 && (request < 14000 || (ret < 0 && ret >= -10)))
            ret = OpusToOpeError(ret);
        return ret;
    }

    // libopusenc: ope_encoder_ctl() (src/opusenc.c), the GET requests
    std::int32_t OggOpusEncoder::Get(std::int32_t request, std::int32_t& value) noexcept
    {
        if (unrecoverable_ != 0)
            return unrecoverable_;
        std::int32_t ret = OpusGet(request, value);
        if (ret != 0 && (request < 14000 || (ret < 0 && ret >= -10)))
            ret = OpusToOpeError(ret);
        return ret;
    }

    std::int32_t OggOpusEncoder::SetExpertFrameDuration(std::int32_t value) noexcept
    {
        return Ctl(OPUS_SET_EXPERT_FRAME_DURATION_REQUEST, value);
    }

    // libopusenc: output_pages() (src/opusenc.c)
    bool OggOpusEncoder::OutputPages() noexcept
    {
        const std::uint8_t* page   = nullptr;
        std::int32_t        length = 0;
        while (packer_.GetNextPage(page, length)) {
            if (!callbacks_.write(callbacks_.user, page, length))
                return false;
        }
        return true;
    }

    // libopusenc: oe_flush_page() (src/opusenc.c)
    bool OggOpusEncoder::FlushPage() noexcept
    {
        packer_.FlushPage();
        return OutputPages();
    }

    // libopusenc: init_stream() (src/opusenc.c)
    void OggOpusEncoder::InitStream() noexcept
    {
        if (!serialNoIsSet_)
            serialNo_ = std::rand();
        if (!packer_.Initialize(serialNo_)) {
            unrecoverable_ = OpeAllocFail;
            return;
        }
        packerCreated_ = true;
        packer_.SetMuxingDelay(static_cast<std::uint64_t>(maxOggDelay_));
        comment_.Pad(commentPadding_);

        // the preskip at the last moment, when it can no longer change
        if (globalGranuleOffset_ == -1) {
            std::int32_t value = 0;
            header_.preskip    = OpusGet(OPUS_GET_LOOKAHEAD_REQUEST, value) == OPUS_OK ? value : 0;
            globalGranuleOffset_ = header_.preskip;
        }

        const std::int32_t headerSize = header_.BufferSize();
        std::uint8_t*      p          = packer_.GetPacketBuffer(headerSize);
        const std::int32_t packetSize = header_.ToPacket(p, headerSize);
        if (callbacks_.packet != nullptr)
            callbacks_.packet(callbacks_.user, p, packetSize);
        packer_.CommitPacket(packetSize, 0, false);
        if (!FlushPage()) {
            unrecoverable_ = OpeWriteFail;
            return;
        }
        p = packer_.GetPacketBuffer(comment_.Length());
        std::memcpy(p, comment_.Data(), static_cast<std::size_t>(comment_.Length()));
        if (callbacks_.packet != nullptr)
            callbacks_.packet(callbacks_.user, p, comment_.Length());
        packer_.CommitPacket(comment_.Length(), 0, false);
        if (!FlushPage()) {
            unrecoverable_ = OpeWriteFail;
            return;
        }
        streamIsInit_ = true;
    }

    // libopusenc: shift_buffer() (src/opusenc.c)
    void OggOpusEncoder::ShiftBuffer() noexcept
    {
        // keeps LpcInput samples before the start for the LPC extension
        if (bufferStart_ > LpcInput) {
            float* b = buffer_.Data();
            std::memmove(b, b + static_cast<std::size_t>(channels_) * (bufferStart_ - LpcInput),
                         static_cast<std::size_t>(channels_) * (bufferEnd_ - bufferStart_ + LpcInput) * sizeof(float));
            bufferEnd_ -= bufferStart_ - LpcInput;
            bufferStart_ = LpcInput;
        }
    }

    // libopusenc: encode_buffer() (src/opusenc.c)
    void OggOpusEncoder::EncodeBuffer() noexcept
    {
        // round up when converting the granule position, the decoder rounds down
        const std::int64_t endGranule48k = (endGranule_ * 48000 + rate_ - 1) / rate_ + globalGranuleOffset_;
        const std::int32_t maxPacketSize = (1277 * 6 + 2) * header_.nbStreams;
        while (bufferEnd_ - bufferStart_ > frameSize_ + decisionDelay_) {
            if (unrecoverable_ != 0)
                return;
            std::int32_t pred = 0;
            OpusGet(OPUS_GET_PREDICTION_DISABLED_REQUEST, pred);
            // the last packet: not more padding than needed
            if (currGranule_ + frameSize_ >= endGranule48k && draining_ && frameSizeRequest_ > OPUS_FRAMESIZE_20_MS) {
                const auto   minSamples = static_cast<std::int32_t>(endGranule48k - currGranule_);
                std::int32_t request    = OPUS_FRAMESIZE_20_MS;
                while (ComputeFrameSamples(request) < minSamples)
                    ++request;
                SetExpertFrameDuration(request);
            }
            std::uint8_t*      packet = packer_.GetPacketBuffer(maxPacketSize);
            const std::int32_t nbBytes
                = OpusEncode(buffer_.Data() + static_cast<std::size_t>(channels_) * bufferStart_, bufferEnd_ - bufferStart_, packet, maxPacketSize);
            if (nbBytes < 0) {
                unrecoverable_ = OpeInternalError;
                return;
            }
            OpusCtl(OPUS_SET_PREDICTION_DISABLED_REQUEST, pred);
            currGranule_ += frameSize_;

            const bool         endOfStream     = currGranule_ >= endGranule48k;
            const std::int64_t granulePosition = endOfStream ? endGranule48k : currGranule_;
            if (callbacks_.packet != nullptr)
                callbacks_.packet(callbacks_.user, packet, nbBytes);
            packer_.CommitPacket(nbBytes, static_cast<std::uint64_t>(granulePosition), endOfStream);
            if (!(endOfStream ? FlushPage() : OutputPages())) {
                unrecoverable_ = OpeWriteFail;
                return;
            }
            if (endOfStream) {
                streamOpen_ = false;
                if (!callbacks_.close(callbacks_.user)) {
                    unrecoverable_ = OpeCloseFail;
                    return;
                }
                return; // no further stream
            }
            bufferStart_ += frameSize_;
        }
        // at the end of the buffer, move everything back to the front
        if (bufferEnd_ == bufferSamples_)
            ShiftBuffer();
    }

    // libopusenc: ope_encoder_write_float() (src/opusenc.c)
    std::int32_t OggOpusEncoder::WriteFloat(const float* pcm, std::int32_t samplesPerChannel) noexcept
    {
        const std::int32_t channels = channels_;
        if (unrecoverable_ != 0)
            return unrecoverable_;
        headerIsFrozen_ = true;
        if (!streamIsInit_)
            InitStream();
        if (samplesPerChannel < 0)
            return OpeBadArg;
        writeGranule_ += samplesPerChannel;
        endGranule_ = writeGranule_;
        if (resample_) {
            float* lpc = lpcBuffer_.Data();
            if (samplesPerChannel < LpcInput) {
                for (std::int32_t i = 0; i < (LpcInput - samplesPerChannel) * channels; ++i)
                    lpc[i] = lpc[samplesPerChannel * channels + i];
                for (std::int32_t i = 0; i < samplesPerChannel * channels; ++i)
                    lpc[(LpcInput - samplesPerChannel) * channels + i] = pcm[i];
            } else {
                for (std::int32_t i = 0; i < LpcInput * channels; ++i)
                    lpc[i] = pcm[(samplesPerChannel - LpcInput) * channels + i];
            }
        }
        do {
            std::uint32_t inSamples  = 0;
            std::uint32_t outSamples = static_cast<std::uint32_t>(bufferSamples_ - bufferEnd_);
            float*        out        = buffer_.Data() + static_cast<std::size_t>(channels) * bufferEnd_;
            if (resample_) {
                inSamples = static_cast<std::uint32_t>(samplesPerChannel);
                resampler_.ProcessInterleaved(pcm, inSamples, out, outSamples);
            } else {
                const std::uint32_t curr
                    = static_cast<std::uint32_t>(samplesPerChannel) < outSamples ? static_cast<std::uint32_t>(samplesPerChannel) : outSamples;
                std::memcpy(out, pcm, static_cast<std::size_t>(channels) * curr * sizeof(float));
                inSamples = outSamples = curr;
            }
            bufferEnd_ += static_cast<std::int32_t>(outSamples);
            pcm += static_cast<std::size_t>(inSamples) * channels;
            samplesPerChannel -= static_cast<std::int32_t>(inSamples);
            EncodeBuffer();
            if (unrecoverable_ != 0)
                return unrecoverable_;
        } while (samplesPerChannel > 0);
        return OpeOk;
    }

    // libopusenc: ope_encoder_drain() (src/opusenc.c)
    std::int32_t OggOpusEncoder::Drain() noexcept
    {
        if (unrecoverable_ != 0)
            return unrecoverable_;
        if (!streamOpen_)
            return OpeTooLate; // already drained
        const std::int32_t resamplerDrain = resample_ ? resampler_.OutputLatency() : 0;
        const std::int32_t needed         = globalGranuleOffset_ + frameSize_ + resamplerDrain + 1;
        std::int32_t       padSamples     = needed > LpcPadding ? needed : LpcPadding;
        if (!streamIsInit_)
            InitStream();
        ShiftBuffer();
        float* b = buffer_.Data();
        std::memset(b + static_cast<std::size_t>(channels_) * bufferEnd_, 0, static_cast<std::size_t>(padSamples) * channels_ * sizeof(float));
        if (resample_) {
            float* lpc = lpcBuffer_.Data() + static_cast<std::size_t>(LpcInput) * channels_;
            ExtendSignal(lpc, LpcInput, LpcPadding, channels_);
            do {
                std::uint32_t inSamples  = LpcPadding;
                std::uint32_t outSamples = static_cast<std::uint32_t>(padSamples);
                resampler_.ProcessInterleaved(lpc, inSamples, b + static_cast<std::size_t>(channels_) * bufferEnd_, outSamples);
                bufferEnd_ += static_cast<std::int32_t>(outSamples);
                padSamples -= static_cast<std::int32_t>(outSamples);
                // not enough padding yet: continue with zeros
                std::memset(lpc, 0, static_cast<std::size_t>(LpcPadding) * channels_ * sizeof(float));
            } while (padSamples > 0);
        } else {
            ExtendSignal(b + static_cast<std::size_t>(channels_) * bufferEnd_, bufferEnd_, LpcPadding, channels_);
            bufferEnd_ += padSamples;
        }
        decisionDelay_ = 0;
        draining_      = true;
        EncodeBuffer();
        if (unrecoverable_ != 0)
            return unrecoverable_;
        return OpeOk;
    }

    std::int32_t OggOpusEncoder::DrainFrames(std::int32_t lookahead) const noexcept
    {
        const std::int32_t resamplerDrain = resample_ ? resampler_.OutputLatency() : 0;
        const std::int32_t needed         = lookahead + frameSize_ + resamplerDrain + 1;
        return needed > LpcPadding ? needed : LpcPadding;
    }

    std::int32_t OggOpusEncoder::WriteResampled(const float* pcm, std::int32_t frames) noexcept
    {
        if (unrecoverable_ != 0)
            return unrecoverable_;
        headerIsFrozen_ = true;
        if (!streamIsInit_)
            InitStream();
        if (frames < 0)
            return OpeBadArg;
        // as the loop of ope_encoder_write_float: fill to the end of the buffer, encode, repeat
        do {
            const std::int32_t space = bufferSamples_ - bufferEnd_;
            const std::int32_t curr  = frames < space ? frames : space;
            std::memcpy(buffer_.Data() + static_cast<std::size_t>(channels_) * bufferEnd_, pcm, static_cast<std::size_t>(channels_) * curr * sizeof(float));
            bufferEnd_ += curr;
            pcm += static_cast<std::size_t>(channels_) * curr;
            frames -= curr;
            EncodeBuffer();
            if (unrecoverable_ != 0)
                return unrecoverable_;
        } while (frames > 0);
        return OpeOk;
    }

    bool OggOpusEncoder::EnlargeBuffer(std::int32_t extra) noexcept
    {
        // run_analysis reads at most (DETECT_SIZE - 5) * 20 ms; the encoder encodes once the buffer
        // holds more than the frame and the decision delay
        constexpr std::int32_t AnalysisClamp = 95 * 960;
        if (frameSize_ + decisionDelay_ < AnalysisClamp || MaxLookahead + extra <= bufferSamples_)
            return true;
        if (!buffer_.Grow(static_cast<std::size_t>(MaxLookahead + extra) * channels_))
            return false;
        bufferSamples_ = MaxLookahead + extra;
        return true;
    }

    std::int32_t OggOpusEncoder::DrainResampled(const float* pad, std::int32_t frames) noexcept
    {
        if (unrecoverable_ != 0)
            return unrecoverable_;
        if (!streamOpen_)
            return OpeTooLate;
        if (!streamIsInit_)
            InitStream();
        ShiftBuffer();
        std::memcpy(buffer_.Data() + static_cast<std::size_t>(channels_) * bufferEnd_, pad, static_cast<std::size_t>(channels_) * frames * sizeof(float));
        bufferEnd_ += frames;
        decisionDelay_ = 0;
        draining_      = true;
        EncodeBuffer();
        if (unrecoverable_ != 0)
            return unrecoverable_;
        return OpeOk;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
