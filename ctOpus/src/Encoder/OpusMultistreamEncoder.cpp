// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "OpusMultistreamEncoder.h"
#include "OpusDefines.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

#include "CeltBands.h"
#include "CeltEncoder.h"
#include "CeltEnergy.h"
#include "CeltMath.h"
#include "CeltMdct.h"
#include "CeltMode.h"
#include "CeltSimd.h"
#include "OpusPacket.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    namespace {

        constexpr std::int32_t MsFrameTmp        = 6 * 1275 + 12; // six 20 ms frames
        constexpr std::int32_t MaxEncodingDepth  = 24;
        constexpr std::int32_t OpusSetLfeRequest = 10024;

        constexpr std::int32_t ImaxI(std::int32_t a, std::int32_t b) noexcept
        {
            return a > b ? a : b;
        }
        constexpr std::int32_t IminI(std::int32_t a, std::int32_t b) noexcept
        {
            return a < b ? a : b;
        }
        constexpr std::int32_t BitsToBitrate(std::int32_t bits, std::int32_t fs, std::int32_t frameSize) noexcept
        {
            return bits * (6 * fs / frameSize) / 6;
        }
        constexpr std::int32_t BitrateToBits(std::int32_t bitrate, std::int32_t fs, std::int32_t frameSize) noexcept
        {
            return bitrate * 6 / (6 * fs / frameSize);
        }

        struct VorbisLayout {
            std::int32_t nbStreams;
            std::int32_t nbCoupledStreams;
            std::uint8_t mapping[8];
        };

        // index: channels - 1
        constexpr VorbisLayout VorbisMappings[8] = {
            {1, 0, {0}},                      // 1: mono
            {1, 1, {0, 1}},                   // 2: stereo
            {2, 1, {0, 2, 1}},                // 3: 1-d surround
            {2, 2, {0, 1, 2, 3}},             // 4: quadraphonic surround
            {3, 2, {0, 4, 1, 2, 3}},          // 5: 5-channel surround
            {4, 2, {0, 4, 1, 2, 3, 5}},       // 6: 5.1 surround
            {4, 3, {0, 4, 1, 2, 3, 5, 6}},    // 7: 6.1 surround
            {5, 3, {0, 6, 1, 2, 3, 4, 5, 7}}, // 8: 7.1 surround
        };

        // the position in the mix: 0 not mixed, 1 left, 2 centre, 3 right
        void ChannelPos(std::int32_t channels, std::int32_t pos[8]) noexcept
        {
            if (channels == 4) {
                pos[0] = 1;
                pos[1] = 3;
                pos[2] = 1;
                pos[3] = 3;
            } else if (channels == 3 || channels == 5 || channels == 6) {
                pos[0] = 1;
                pos[1] = 2;
                pos[2] = 3;
                pos[3] = 1;
                pos[4] = 3;
                pos[5] = 0;
            } else if (channels == 7) {
                pos[0] = 1;
                pos[1] = 2;
                pos[2] = 3;
                pos[3] = 1;
                pos[4] = 3;
                pos[5] = 2;
                pos[6] = 0;
            } else if (channels == 8) {
                pos[0] = 1;
                pos[1] = 2;
                pos[2] = 3;
                pos[3] = 1;
                pos[4] = 3;
                pos[5] = 1;
                pos[6] = 3;
                pos[7] = 0;
            }
        }

        // a rough approximation of log2(2^a + 2^b)
        float LogSum(float a, float b) noexcept
        {
            static constexpr float diffTable[17] = {0.5000000f, 0.2924813f, 0.1609640f, 0.0849625f, 0.0437314f, 0.0221971f, 0.0111839f, 0.0056136f, 0.0028123f};
            float                  max           = 0;
            float                  diff          = 0;
            if (a > b) {
                max  = a;
                diff = a - b;
            } else {
                max  = b;
                diff = b - a;
            }
            if (!(diff < 8.f)) // inverted to catch NaNs
                return max;
            const auto  low  = static_cast<std::int32_t>(std::floor(2 * diff));
            const float frac = 2 * diff - low;
            return max + diffTable[low] + frac * (diffTable[low + 1] - diffTable[low]);
        }

    } // namespace

    std::int32_t FrameSizeSelect(std::int32_t application, std::int32_t frameSize, std::int32_t variableDuration, std::int32_t fs) noexcept
    {
        if (frameSize < fs / 400)
            return -1;
        std::int32_t newSize;
        if (variableDuration == OPUS_FRAMESIZE_ARG)
            newSize = frameSize;
        else if (variableDuration >= OPUS_FRAMESIZE_2_5_MS && variableDuration <= OPUS_FRAMESIZE_120_MS) {
            if (variableDuration <= OPUS_FRAMESIZE_40_MS)
                newSize = (fs / 400) << (variableDuration - OPUS_FRAMESIZE_2_5_MS);
            else
                newSize = (variableDuration - OPUS_FRAMESIZE_2_5_MS - 2) * fs / 50;
        } else
            return -1;
        if (newSize > frameSize)
            return -1;
        if (400 * newSize != fs && 200 * newSize != fs && 100 * newSize != fs && 50 * newSize != fs && 25 * newSize != fs && 50 * newSize != 3 * fs
            && 50 * newSize != 4 * fs && 50 * newSize != 5 * fs && 50 * newSize != 6 * fs)
            return -1;
        if (application == OPUS_APPLICATION_RESTRICTED_SILK && newSize < fs / 100)
            return -1;
        return newSize;
    }

    std::int32_t OpusMultistreamEncoder::LeftChannel(std::int32_t streamId) const noexcept
    {
        for (std::int32_t i = 0; i < nbChannels_; ++i) {
            if (mapping_[i] == streamId * 2)
                return i;
        }
        return -1;
    }

    std::int32_t OpusMultistreamEncoder::RightChannel(std::int32_t streamId) const noexcept
    {
        for (std::int32_t i = 0; i < nbChannels_; ++i) {
            if (mapping_[i] == streamId * 2 + 1)
                return i;
        }
        return -1;
    }

    std::int32_t OpusMultistreamEncoder::MonoChannel(std::int32_t streamId) const noexcept
    {
        for (std::int32_t i = 0; i < nbChannels_; ++i) {
            if (mapping_[i] == streamId + nbCoupled_)
                return i;
        }
        return -1;
    }

    std::int32_t OpusMultistreamEncoder::InitSurround(std::int32_t fs, std::int32_t channels, std::int32_t mappingFamily, std::int32_t& streams,
                                                      std::int32_t& coupledStreams, std::uint8_t* mapping, std::int32_t application) noexcept
    {
        if (channels > 255 || channels < 1)
            return OPUS_BAD_ARG;
        std::int32_t lfeStream = -1;
        if (mappingFamily == 0) {
            if (channels == 1) {
                streams        = 1;
                coupledStreams = 0;
                mapping[0]     = 0;
            } else if (channels == 2) {
                streams        = 1;
                coupledStreams = 1;
                mapping[0]     = 0;
                mapping[1]     = 1;
            } else {
                return OPUS_UNIMPLEMENTED;
            }
        } else if (mappingFamily == 1 && channels <= 8 && channels >= 1) {
            streams        = VorbisMappings[channels - 1].nbStreams;
            coupledStreams = VorbisMappings[channels - 1].nbCoupledStreams;
            for (std::int32_t i = 0; i < channels; ++i)
                mapping[i] = VorbisMappings[channels - 1].mapping[i];
            if (channels >= 6)
                lfeStream = streams - 1;
        } else if (mappingFamily == 255) {
            streams        = channels;
            coupledStreams = 0;
            for (std::int32_t i = 0; i < channels; ++i)
                mapping[i] = static_cast<std::uint8_t>(i);
        } else {
            return OPUS_UNIMPLEMENTED;
        }
        const bool surround = channels > 2 && mappingFamily == 1;

        // opus_multistream_encoder_init_impl
        if (coupledStreams > streams || streams < 1 || coupledStreams < 0 || streams > 255 - coupledStreams || streams + coupledStreams > channels)
            return OPUS_BAD_ARG;
        lfeStream_        = lfeStream;
        nbChannels_       = channels;
        nbStreams_        = streams;
        nbCoupled_        = coupledStreams;
        bitrateBps_       = OPUS_AUTO;
        application_      = application;
        fs_               = fs;
        variableDuration_ = OPUS_FRAMESIZE_ARG;
        for (std::int32_t i = 0; i < nbChannels_; ++i)
            mapping_[i] = mapping[i];
        // validate_layout and validate_encoder_layout hold for the layouts above
        delete[] streams_;
        streams_ = new (std::nothrow) OpusStreamEncoder[static_cast<std::size_t>(streams)];
        if (streams_ == nullptr)
            return OPUS_ALLOC_FAIL;
        for (std::int32_t i = 0; i < nbStreams_; ++i) {
            const std::int32_t ret = streams_[i].Init(fs, i < nbCoupled_ ? 2 : 1, application);
            if (ret != OPUS_OK)
                return ret;
            if (i == lfeStream_)
                streams_[i].Ctl(OpusSetLfeRequest, 1);
        }
        surround_ = surround;
        if (surround_) {
            if (!windowMem_.Allocate(static_cast<std::size_t>(channels) * MaxOverlap) || !preemphMem_.Allocate(static_cast<std::size_t>(channels)))
                return OPUS_ALLOC_FAIL;
        }
        return OPUS_OK;
    }

    std::int32_t OpusMultistreamEncoder::Ctl(std::int32_t request, std::int32_t value) noexcept
    {
        std::int32_t ret = OPUS_OK;
        switch (request) {
            case OPUS_SET_BITRATE_REQUEST:
                if (value != OPUS_AUTO && value != OPUS_BITRATE_MAX) {
                    if (value <= 0)
                        return OPUS_BAD_ARG;
                    value = IminI(750000 * nbChannels_, ImaxI(500 * nbChannels_, value));
                }
                bitrateBps_ = value;
                return OPUS_OK;
            case OPUS_SET_LSB_DEPTH_REQUEST:
            case OPUS_SET_COMPLEXITY_REQUEST:
            case OPUS_SET_VBR_REQUEST:
            case OPUS_SET_VBR_CONSTRAINT_REQUEST:
            case OPUS_SET_MAX_BANDWIDTH_REQUEST:
            case OPUS_SET_BANDWIDTH_REQUEST:
            case OPUS_SET_SIGNAL_REQUEST:
            case OPUS_SET_APPLICATION_REQUEST:
            case OPUS_SET_INBAND_FEC_REQUEST:
            case OPUS_SET_PACKET_LOSS_PERC_REQUEST:
            case OPUS_SET_DTX_REQUEST:
            case OpusSetForceModeRequest:
            case OPUS_SET_FORCE_CHANNELS_REQUEST:
            case OPUS_SET_PREDICTION_DISABLED_REQUEST:
            case OPUS_SET_PHASE_INVERSION_DISABLED_REQUEST:
            case OPUS_SET_QEXT_REQUEST:
                // int32 parameters go to every stream
                for (std::int32_t s = 0; s < nbStreams_; ++s) {
                    ret = streams_[s].Ctl(request, value);
                    if (ret != OPUS_OK)
                        break;
                }
                return ret;
            case OPUS_SET_EXPERT_FRAME_DURATION_REQUEST: variableDuration_ = value; return OPUS_OK;
            case OPUS_RESET_STATE:
                if (surround_) {
                    std::memset(preemphMem_.Data(), 0, static_cast<std::size_t>(nbChannels_) * sizeof(float));
                    std::memset(windowMem_.Data(), 0, static_cast<std::size_t>(nbChannels_) * MaxOverlap * sizeof(float));
                }
                for (std::int32_t s = 0; s < nbStreams_; ++s) {
                    ret = streams_[s].Ctl(OPUS_RESET_STATE, 0);
                    if (ret != OPUS_OK)
                        break;
                }
                return ret;
            default: return OPUS_UNIMPLEMENTED;
        }
    }

    std::int32_t OpusMultistreamEncoder::Get(std::int32_t request, std::int32_t& value) noexcept
    {
        switch (request) {
            case OPUS_GET_BITRATE_REQUEST: {
                value = 0;
                for (std::int32_t s = 0; s < nbStreams_; ++s) {
                    std::int32_t rate = 0;
                    streams_[s].Get(request, rate);
                    value += rate;
                }
                return OPUS_OK;
            }
            case OPUS_GET_LSB_DEPTH_REQUEST:
            case OPUS_GET_VBR_REQUEST:
            case OPUS_GET_APPLICATION_REQUEST:
            case OPUS_GET_BANDWIDTH_REQUEST:
            case OPUS_GET_COMPLEXITY_REQUEST:
            case OPUS_GET_PACKET_LOSS_PERC_REQUEST:
            case OPUS_GET_DTX_REQUEST:
            case OpusGetVoiceRatioRequest:
            case OPUS_GET_VBR_CONSTRAINT_REQUEST:
            case OPUS_GET_SIGNAL_REQUEST:
            case OPUS_GET_LOOKAHEAD_REQUEST:
            case OPUS_GET_SAMPLE_RATE_REQUEST:
            case OPUS_GET_INBAND_FEC_REQUEST:
            case OPUS_GET_FORCE_CHANNELS_REQUEST:
            case OPUS_GET_PREDICTION_DISABLED_REQUEST:
            case OPUS_GET_PHASE_INVERSION_DISABLED_REQUEST:
            case OPUS_GET_QEXT_REQUEST: return streams_[0].Get(request, value); // the first stream answers
            case OPUS_GET_FINAL_RANGE_REQUEST: {
                std::uint32_t range = 0;
                for (std::int32_t s = 0; s < nbStreams_; ++s)
                    range ^= streams_[s].FinalRange();
                value = static_cast<std::int32_t>(range);
                return OPUS_OK;
            }
            case OPUS_GET_EXPERT_FRAME_DURATION_REQUEST: value = variableDuration_; return OPUS_OK;
            default: return OPUS_UNIMPLEMENTED;
        }
    }

    void OpusMultistreamEncoder::SurroundAnalysis(const float* pcm, float* bandLogE, std::int32_t len) noexcept
    {
        constexpr std::int32_t overlap   = CeltMode::Overlap;
        const std::int32_t     channels  = nbChannels_;
        const std::int32_t     upsample  = 1; // resampling_factor(48000)
        const std::int32_t     frameSize = len * upsample;
        std::int32_t           pos[8]    = {0};
        float                  bandE[21];
        float                  maskLogE[3][21];
        float                  freq[960];

        // LM = log2(frame_size / 120)
        std::int32_t lm = 0;
        for (lm = 0; lm < CeltMode::MaxLM; ++lm) {
            if (CeltMode::ShortMdctSize << lm == frameSize)
                break;
        }
        const std::int32_t freqSize = CeltMode::ShortMdctSize << lm;

        HeapArray<float> in;
        HeapArray<float> x;
        if (!in.Allocate(static_cast<std::size_t>(frameSize + overlap)) || !x.Allocate(static_cast<std::size_t>(len)))
            return;

        ChannelPos(channels, pos);
        for (std::int32_t c = 0; c < 3; ++c)
            for (std::int32_t i = 0; i < 21; ++i)
                maskLogE[c][i] = -28.f;

        for (std::int32_t c = 0; c < channels; ++c) {
            const std::int32_t nbFrames = frameSize / freqSize;
            std::memcpy(in.Data(), windowMem_.Data() + c * overlap, overlap * sizeof(float));
            for (std::int32_t i = 0; i < len; ++i)
                x[static_cast<std::size_t>(i)] = pcm[i * channels + c];
            CeltPreemphasis(x.Data(), in.Data() + overlap, frameSize, 1, upsample, CeltMode::Preemph, preemphMem_.Data() + c, false);
            {
                const float sum = InnerProd(in.Data(), in.Data(), frameSize + overlap);
                // filters out NaNs and ridiculous signals that could cause NaNs further down
                if (!(sum < 1e18f) || IsNan(sum)) {
                    std::memset(in.Data(), 0, static_cast<std::size_t>(frameSize + overlap) * sizeof(float));
                    preemphMem_[static_cast<std::size_t>(c)] = 0;
                }
            }
            std::memset(bandE, 0, sizeof(bandE));
            for (std::int32_t frame = 0; frame < nbFrames; ++frame) {
                float tmpE[21];
                MdctForward(in.Data() + freqSize * frame, freq, CeltMode::MaxLM - lm, 1);
                ComputeBandEnergies(freq, tmpE, 21, 1, lm);
                // with several frames, the max energy
                for (std::int32_t i = 0; i < 21; ++i)
                    bandE[i] = Max32(bandE[i], tmpE[i]);
            }
            Amp2Log2(21, 21, bandE, bandLogE + 21 * c, 1);
            // a spreading function: -6 dB/band going up, -12 dB/band going down
            for (std::int32_t i = 1; i < 21; ++i)
                bandLogE[21 * c + i] = Max32(bandLogE[21 * c + i], bandLogE[21 * c + i - 1] - 1.f);
            for (std::int32_t i = 19; i >= 0; --i)
                bandLogE[21 * c + i] = Max32(bandLogE[21 * c + i], bandLogE[21 * c + i + 1] - 2.f);
            if (pos[c] == 1) {
                for (std::int32_t i = 0; i < 21; ++i)
                    maskLogE[0][i] = LogSum(maskLogE[0][i], bandLogE[21 * c + i]);
            } else if (pos[c] == 3) {
                for (std::int32_t i = 0; i < 21; ++i)
                    maskLogE[2][i] = LogSum(maskLogE[2][i], bandLogE[21 * c + i]);
            } else if (pos[c] == 2) {
                for (std::int32_t i = 0; i < 21; ++i) {
                    maskLogE[0][i] = LogSum(maskLogE[0][i], bandLogE[21 * c + i] - .5f);
                    maskLogE[2][i] = LogSum(maskLogE[2][i], bandLogE[21 * c + i] - .5f);
                }
            }
            std::memcpy(windowMem_.Data() + c * overlap, in.Data() + frameSize, overlap * sizeof(float));
        }
        for (std::int32_t i = 0; i < 21; ++i)
            maskLogE[1][i] = Min32(maskLogE[0][i], maskLogE[2][i]);
        const float channelOffset = .5f * static_cast<float>(1.442695040888963387 * std::log(static_cast<double>(2.f / (channels - 1))));
        for (std::int32_t c = 0; c < 3; ++c)
            for (std::int32_t i = 0; i < 21; ++i)
                maskLogE[c][i] += channelOffset;
        for (std::int32_t c = 0; c < channels; ++c) {
            if (pos[c] != 0) {
                const float* mask = &maskLogE[pos[c] - 1][0];
                for (std::int32_t i = 0; i < 21; ++i)
                    bandLogE[21 * c + i] = bandLogE[21 * c + i] - mask[i];
            } else {
                for (std::int32_t i = 0; i < 21; ++i)
                    bandLogE[21 * c + i] = 0;
            }
        }
    }

    std::int32_t OpusMultistreamEncoder::RateAllocation(std::int32_t* rate, std::int32_t frameSize) noexcept
    {
        std::int32_t fs = 0;
        streams_[0].Get(OPUS_GET_SAMPLE_RATE_REQUEST, fs);

        // surround_rate_allocation
        const std::int32_t nbLfe       = lfeStream_ != -1 ? 1 : 0;
        const std::int32_t nbCoupled   = nbCoupled_;
        const std::int32_t nbUncoupled = nbStreams_ - nbCoupled - nbLfe;
        const std::int32_t nbNormal    = 2 * nbCoupled + nbUncoupled;

        // enough bits per non-LFE channel for the band energies
        const std::int32_t channelOffset = 40 * ImaxI(50, fs / frameSize);

        std::int32_t bitrate = 0;
        if (bitrateBps_ == OPUS_AUTO)
            bitrate = nbNormal * (channelOffset + fs + 10000) + 8000 * nbLfe;
        else if (bitrateBps_ == OPUS_BITRATE_MAX)
            bitrate = nbNormal * 750000 + nbLfe * 128000;
        else
            bitrate = bitrateBps_;

        // a basic allocation for the LFE, never more than 1/20 of the total for the non-energy
        // part (against problems at really low rates)
        const std::int32_t lfeOffset = IminI(bitrate / 20, 3000) + 15 * ImaxI(50, fs / frameSize);

        // a starting bitrate per stream (coupled or not): the main saving of coupling
        std::int32_t streamOffset = (bitrate - channelOffset * nbNormal - lfeOffset * nbLfe) / nbNormal / 2;
        streamOffset              = ImaxI(0, IminI(20000, streamOffset));

        // coupled streams get twice the mono rate after the offset; the LFE 1/8 (Q8)
        constexpr std::int32_t coupledRatio = 512;
        constexpr std::int32_t lfeRatio     = 32;

        const std::int32_t total       = (nbUncoupled << 8) + coupledRatio * nbCoupled + nbLfe * lfeRatio;
        const auto         channelRate = static_cast<std::int32_t>(
            256 * static_cast<std::int64_t>(bitrate - lfeOffset * nbLfe - streamOffset * (nbCoupled + nbUncoupled) - channelOffset * nbNormal) / total);

        for (std::int32_t i = 0; i < nbStreams_; ++i) {
            if (i < nbCoupled_)
                rate[i] = 2 * channelOffset + ImaxI(0, streamOffset + (channelRate * coupledRatio >> 8));
            else if (i != lfeStream_)
                rate[i] = channelOffset + ImaxI(0, streamOffset + channelRate);
            else
                rate[i] = ImaxI(0, lfeOffset + (channelRate * lfeRatio >> 8));
        }

        std::int32_t rateSum = 0;
        for (std::int32_t i = 0; i < nbStreams_; ++i) {
            rate[i] = ImaxI(rate[i], 500);
            rateSum += rate[i];
        }
        return rateSum;
    }

    std::int32_t OpusMultistreamEncoder::EncodeFloat(const float* pcm, std::int32_t analysisFrameSize, std::uint8_t* data, std::int32_t maxDataBytes) noexcept
    {
        std::int32_t fs  = 0;
        std::int32_t vbr = 0;
        streams_[0].Get(OPUS_GET_SAMPLE_RATE_REQUEST, fs);
        streams_[0].Get(OPUS_GET_VBR_REQUEST, vbr);

        const std::int32_t frameSize = FrameSizeSelect(application_, analysisFrameSize, variableDuration_, fs);
        if (frameSize <= 0)
            return OPUS_BAD_ARG;

        // the smallest packet the encoder can produce; 100 ms needs an extra byte per stream for the ToC
        std::int32_t smallestPacket = nbStreams_ * 2 - 1;
        if (fs / frameSize == 10)
            smallestPacket += nbStreams_;
        if (maxDataBytes < smallestPacket)
            return OPUS_BUFFER_TOO_SMALL;

        // kept from frame to frame (every value read is written first)
        if (!streamPcm_.Grow(static_cast<std::size_t>(2) * frameSize) || !bandSmr_.Grow(static_cast<std::size_t>(21) * nbChannels_))
            return OPUS_ALLOC_FAIL;
        HeapArray<float>& buf     = streamPcm_;
        HeapArray<float>& bandSmr = bandSmr_;
        const bool        masking = surround_ && application_ != OPUS_APPLICATION_RESTRICTED_SILK;
        if (masking)
            SurroundAnalysis(pcm, bandSmr.Data(), frameSize);

        // the bitrate allocation between streams (this could be a lot better)
        std::int32_t       bitrates[256];
        const std::int32_t rateSum = RateAllocation(bitrates, frameSize);

        if (vbr == 0) {
            if (bitrateBps_ == OPUS_AUTO)
                maxDataBytes = IminI(maxDataBytes, (BitrateToBits(rateSum, fs, frameSize) + 4) / 8);
            else if (bitrateBps_ != OPUS_BITRATE_MAX)
                maxDataBytes = IminI(maxDataBytes, ImaxI(smallestPacket, (BitrateToBits(bitrateBps_, fs, frameSize) + 4) / 8));
        }
        for (std::int32_t s = 0; s < nbStreams_; ++s) {
            OpusStreamEncoder& enc = streams_[s];
            enc.Ctl(OPUS_SET_BITRATE_REQUEST, bitrates[s]);
            if (surround_) {
                std::int32_t equivRate = bitrateBps_;
                if (frameSize * 50 < fs)
                    equivRate -= 60 * (fs / frameSize - 50) * nbChannels_;
                if (equivRate > 10000 * nbChannels_)
                    enc.Ctl(OPUS_SET_BANDWIDTH_REQUEST, OPUS_BANDWIDTH_FULLBAND);
                else if (equivRate > 7000 * nbChannels_)
                    enc.Ctl(OPUS_SET_BANDWIDTH_REQUEST, OPUS_BANDWIDTH_SUPERWIDEBAND);
                else if (equivRate > 5000 * nbChannels_)
                    enc.Ctl(OPUS_SET_BANDWIDTH_REQUEST, OPUS_BANDWIDTH_WIDEBAND);
                else
                    enc.Ctl(OPUS_SET_BANDWIDTH_REQUEST, OPUS_BANDWIDTH_NARROWBAND);
                if (s < nbCoupled_) {
                    // stereo CELT on coupled streams preserves the spatial image
                    enc.Ctl(OpusSetForceModeRequest, ModeCeltOnly);
                    enc.Ctl(OPUS_SET_FORCE_CHANNELS_REQUEST, 2);
                }
            }
        }

        // counting the ToC
        std::int32_t tot_size = 0;
        float        bandLogE[42];
        std::uint8_t tmpData[MsFrameTmp];
        for (std::int32_t s = 0; s < nbStreams_; ++s) {
            OpusStreamEncoder& enc       = streams_[s];
            std::int32_t       c1        = 0;
            std::int32_t       c2        = -1;
            const float*       streamPcm = buf.Data();
            if (s < nbCoupled_) {
                const std::int32_t left  = LeftChannel(s);
                const std::int32_t right = RightChannel(s);
                if (nbChannels_ == 2 && left == 0 && right == 1) {
                    // the stream's channels as they are
                    streamPcm = pcm;
                } else {
                    for (std::int32_t i = 0; i < frameSize; ++i) {
                        buf[static_cast<std::size_t>(i) * 2]     = pcm[i * nbChannels_ + left];
                        buf[static_cast<std::size_t>(i) * 2 + 1] = pcm[i * nbChannels_ + right];
                    }
                }
                if (masking) {
                    for (std::int32_t i = 0; i < 21; ++i) {
                        bandLogE[i]      = bandSmr[static_cast<std::size_t>(21 * left + i)];
                        bandLogE[21 + i] = bandSmr[static_cast<std::size_t>(21 * right + i)];
                    }
                }
                c1 = left;
                c2 = right;
            } else {
                const std::int32_t chan = MonoChannel(s);
                for (std::int32_t i = 0; i < frameSize; ++i)
                    buf[static_cast<std::size_t>(i)] = pcm[i * nbChannels_ + chan];
                if (masking) {
                    for (std::int32_t i = 0; i < 21; ++i)
                        bandLogE[i] = bandSmr[static_cast<std::size_t>(21 * chan + i)];
                }
                c1 = chan;
                c2 = -1;
            }
            if (masking)
                enc.SetEnergyMask(bandLogE);
            // bytes left (+ToC); one byte reserved for the last stream, two for the others
            std::int32_t currMax = maxDataBytes - tot_size;
            currMax -= ImaxI(0, 2 * (nbStreams_ - s - 1) - 1);
            // 100 ms: an extra byte per stream for the ToC
            if (fs / frameSize == 10)
                currMax -= nbStreams_ - s - 1;
            currMax = IminI(currMax, MsFrameTmp);
            // the repacketizer adds one or two bytes for self-delimited frames
            if (s != nbStreams_ - 1)
                currMax -= currMax > 253 ? 2 : 1;
            if (vbr == 0 && s == nbStreams_ - 1)
                enc.Ctl(OPUS_SET_BITRATE_REQUEST, BitsToBitrate(currMax * 8, fs, frameSize));
            std::int32_t len = enc.EncodeNative(streamPcm, frameSize, tmpData, currMax, MaxEncodingDepth, pcm, analysisFrameSize, c1, c2, nbChannels_);
            if (len < 0)
                return len;
            // the repacketizer adds the self-delimiting lengths (the encoder may return more than
            // one frame, e.g. 60 ms CELT-only)
            Repacketizer rp;
            rp.Init();
            if (rp.Cat(tmpData, len) != OPUS_OK)
                return OPUS_INTERNAL_ERROR;
            len = rp.OutRange(0, rp.NbFrames(), data, maxDataBytes - tot_size, s != nbStreams_ - 1, vbr == 0 && s == nbStreams_ - 1);
            data += len;
            tot_size += len;
        }
        return tot_size;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
