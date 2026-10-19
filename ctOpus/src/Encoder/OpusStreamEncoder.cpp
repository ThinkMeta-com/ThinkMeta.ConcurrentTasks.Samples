// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "OpusStreamEncoder.h"
#include "OpusDefines.h"

#include "CeltMath.h"
#include "CeltPitch.h"
#include "CeltSimd.h"
#include "IntMath.h"
#include "OpusPacket.h"
#include "SilkEncoder.h"
#include "SilkSigProc.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    namespace {

        // silk/define.h, silk/tuning_parameters.h
        constexpr std::int32_t NbSpeechFramesBeforeDtx = 10;
        constexpr std::int32_t MaxConsecutiveDtx       = 20;
        constexpr float        DtxActivityThreshold    = 0.1f;
        constexpr std::int32_t VadNoDecision           = -1;
        constexpr std::int32_t TypeNoVoiceActivity     = 0;
        constexpr std::int32_t VariableHpMinCutoffHz   = 60;
        constexpr float        VariableHpSmthCoef2     = 0.015f;
        constexpr float        PseudoSnrThreshold      = 316.23f; // 10^(25/10)
        constexpr std::int32_t MaxEncodingDepth        = 24;
        constexpr float        CeltSigScale            = 32768.f;

        constexpr std::int32_t MonoVoiceBandwidthThresholds[8]   = {9000, 700, 9000, 700, 13500, 1000, 14000, 2000};
        constexpr std::int32_t MonoMusicBandwidthThresholds[8]   = {9000, 700, 9000, 700, 11000, 1000, 12000, 2000};
        constexpr std::int32_t StereoVoiceBandwidthThresholds[8] = {9000, 700, 9000, 700, 13500, 1000, 14000, 2000};
        constexpr std::int32_t StereoMusicBandwidthThresholds[8] = {9000, 700, 9000, 700, 11000, 1000, 12000, 2000};
        constexpr std::int32_t StereoVoiceThreshold              = 19000;
        constexpr std::int32_t StereoMusicThreshold              = 17000;
        constexpr std::int32_t ModeThresholds[2][2]              = {{64000, 10000}, {44000, 10000}}; // [mono/stereo][voice/music]
        constexpr std::int32_t FecThresholds[]                   = {12000, 1000, 14000, 1000, 16000, 1000, 20000, 1000, 22000, 1000};

        // libopus: bits_to_bitrate() (celt/celt.h)
        constexpr std::int32_t BitsToBitrate(std::int32_t bits, std::int32_t fs, std::int32_t frameSize) noexcept
        {
            return bits * (6 * fs / frameSize) / 6;
        }
        // libopus: bitrate_to_bits() (celt/celt.h)
        constexpr std::int32_t BitrateToBits(std::int32_t bitrate, std::int32_t fs, std::int32_t frameSize) noexcept
        {
            return bitrate * 6 / (6 * fs / frameSize);
        }

        // SigProc macros of SILK
        constexpr std::int32_t SilkSmulwb(std::int32_t a32, std::int32_t b32) noexcept
        {
            return static_cast<std::int32_t>((a32 * static_cast<std::int64_t>(static_cast<std::int16_t>(b32))) >> 16);
        }
        constexpr std::int32_t SilkSmlawb(std::int32_t a32, std::int32_t b32, std::int32_t c32) noexcept
        {
            return static_cast<std::int32_t>(a32 + ((b32 * static_cast<std::int64_t>(static_cast<std::int16_t>(c32))) >> 16));
        }
        constexpr std::int32_t SilkSmulww(std::int32_t a32, std::int32_t b32) noexcept
        {
            return static_cast<std::int32_t>((static_cast<std::int64_t>(a32) * b32) >> 16);
        }
        constexpr std::int32_t SilkSmulbb(std::int32_t a32, std::int32_t b32) noexcept
        {
            return static_cast<std::int32_t>(static_cast<std::int16_t>(a32)) * static_cast<std::int32_t>(static_cast<std::int16_t>(b32));
        }
        // SILK_FIX_CONST(C, Q): a float or double C times 2^Q (as float or double), plus 0.5
        constexpr std::int32_t SilkFixConst(float c, std::int32_t q) noexcept
        {
            return static_cast<std::int32_t>(c * static_cast<float>(static_cast<std::int64_t>(1) << q) + 0.5);
        }
        constexpr std::int32_t SilkFixConst(double c, std::int32_t q) noexcept
        {
            return static_cast<std::int32_t>(c * static_cast<double>(static_cast<std::int64_t>(1) << q) + 0.5);
        }

        // libopus: celt_sqrt() (celt/mathops.h)
        float CeltSqrt(float x) noexcept
        {
            return static_cast<float>(std::sqrt(static_cast<double>(x)));
        }
        // libopus: celt_exp2() (celt/mathops.h)
        float CeltExp2(float x) noexcept
        {
            return static_cast<float>(std::exp(0.6931471805599453094 * x));
        }
        // libopus: ABS16() (celt/arch.h)
        float Abs16(float x) noexcept
        {
            return static_cast<float>(std::fabs(static_cast<double>(x)));
        }

        // libopus: gen_toc() (src/opus_encoder.c)
        std::uint8_t GenToc(std::int32_t mode, std::int32_t framerate, std::int32_t bandwidth, std::int32_t channels) noexcept
        {
            std::int32_t period = 0;
            while (framerate < 400) {
                framerate <<= 1;
                ++period;
            }
            std::int32_t toc;
            if (mode == ModeSilkOnly) {
                toc = (bandwidth - OPUS_BANDWIDTH_NARROWBAND) << 5;
                toc |= (period - 2) << 3;
            } else if (mode == ModeCeltOnly) {
                std::int32_t tmp = bandwidth - OPUS_BANDWIDTH_MEDIUMBAND;
                if (tmp < 0)
                    tmp = 0;
                toc = 0x80;
                toc |= tmp << 5;
                toc |= period << 3;
            } else {
                toc = 0x60;
                toc |= (bandwidth - OPUS_BANDWIDTH_SUPERWIDEBAND) << 4;
                toc |= (period - 2) << 3;
            }
            toc |= (channels == 2) << 2;
            return static_cast<std::uint8_t>(toc);
        }

        // libopus: silk_biquad_res() (src/opus_encoder.c) (float): second order ARMA filter, direct form II transposed
        void SilkBiquadRes(const float* in, const std::int32_t* bQ28, const std::int32_t* aQ28, float* s, float* out, std::int32_t len,
                           std::int32_t stride) noexcept
        {
            const float a0 = static_cast<float>(aQ28[0] * (1.f / (static_cast<std::int32_t>(1) << 28)));
            const float a1 = static_cast<float>(aQ28[1] * (1.f / (static_cast<std::int32_t>(1) << 28)));
            const float b0 = static_cast<float>(bQ28[0] * (1.f / (static_cast<std::int32_t>(1) << 28)));
            const float b1 = static_cast<float>(bQ28[1] * (1.f / (static_cast<std::int32_t>(1) << 28)));
            const float b2 = static_cast<float>(bQ28[2] * (1.f / (static_cast<std::int32_t>(1) << 28)));
            for (std::int32_t k = 0; k < len; ++k) {
                const float inval = in[k * stride];
                const float vout  = s[0] + b0 * inval;
                s[0]              = s[1] - vout * a0 + b1 * inval;
                s[1]              = -vout * a1 + b2 * inval + 1e-30f;
                out[k * stride]   = vout;
            }
        }

        // libopus: hp_cutoff() (src/opus_encoder.c)
        void HpCutoff(const float* in, std::int32_t cutoffHz, float* out, float* hpMem, std::int32_t len, std::int32_t channels, std::int32_t fs) noexcept
        {
            std::int32_t       bQ28[3], aQ28[2];
            const std::int32_t fcQ19 = SilkSmulbb(SilkFixConst(1.5 * 3.14159 / 1000, 19), cutoffHz) / (fs / 1000);
            const std::int32_t rQ28  = SilkFixConst(1.0, 28) - SilkFixConst(0.92, 9) * fcQ19;
            bQ28[0]                  = rQ28;
            bQ28[1]                  = static_cast<std::int32_t>(static_cast<std::uint32_t>(-rQ28) << 1);
            bQ28[2]                  = rQ28;
            const std::int32_t rQ22  = rQ28 >> 6;
            aQ28[0]                  = SilkSmulww(rQ22, SilkSmulww(fcQ19, fcQ19) - SilkFixConst(2.0, 22));
            aQ28[1]                  = SilkSmulww(rQ22, rQ22);
            SilkBiquadRes(in, bQ28, aQ28, hpMem, out, len, channels);
            if (channels == 2)
                SilkBiquadRes(in + 1, bQ28, aQ28, hpMem + 2, out + 1, len, channels);
        }

        // libopus: dc_reject() (src/opus_encoder.c)
        void DcReject(const float* in, std::int32_t cutoffHz, float* out, float* hpMem, std::int32_t len, std::int32_t channels, std::int32_t fs) noexcept
        {
            const float coef  = 6.3f * cutoffHz / fs;
            const float coef2 = 1 - coef;
            if (channels == 2) {
                float m0 = hpMem[0];
                float m2 = hpMem[2];
                for (std::int32_t i = 0; i < len; ++i) {
                    const float x0   = in[2 * i + 0];
                    const float x1   = in[2 * i + 1];
                    const float out0 = x0 - m0;
                    const float out1 = x1 - m2;
                    m0               = coef * x0 + 1e-30f + coef2 * m0;
                    m2               = coef * x1 + 1e-30f + coef2 * m2;
                    out[2 * i + 0]   = out0;
                    out[2 * i + 1]   = out1;
                }
                hpMem[0] = m0;
                hpMem[2] = m2;
            } else {
                float m0 = hpMem[0];
                for (std::int32_t i = 0; i < len; ++i) {
                    const float x = in[i];
                    const float y = x - m0;
                    m0            = coef * x + 1e-30f + coef2 * m0;
                    out[i]        = y;
                }
                hpMem[0] = m0;
            }
        }

        // libopus: stereo_fade() (src/opus_encoder.c)
        void StereoFade(const float* in, float* out, float g1, float g2, std::int32_t overlap48, std::int32_t frameSize, std::int32_t channels,
                        const float* window, std::int32_t fs) noexcept
        {
            const std::int32_t inc     = Imax(1, 48000 / fs);
            const std::int32_t overlap = overlap48 / inc;
            g1                         = 1.0f - g1;
            g2                         = 1.0f - g2;
            std::int32_t i             = 0;
            for (; i < overlap; ++i) {
                float w               = window[i * inc];
                w                     = w * w;
                const float g         = w * g2 + (1.0f - w) * g1;
                float       diff      = .5f * (in[i * channels] - in[i * channels + 1]);
                diff                  = g * diff;
                out[i * channels]     = out[i * channels] - diff;
                out[i * channels + 1] = out[i * channels + 1] + diff;
            }
            for (; i < frameSize; ++i) {
                float diff            = .5f * (in[i * channels] - in[i * channels + 1]);
                diff                  = g2 * diff;
                out[i * channels]     = out[i * channels] - diff;
                out[i * channels + 1] = out[i * channels + 1] + diff;
            }
        }

        // libopus: gain_fade() (src/opus_encoder.c)
        void GainFade(const float* in, float* out, float g1, float g2, std::int32_t overlap48, std::int32_t frameSize, std::int32_t channels,
                      const float* window, std::int32_t fs) noexcept
        {
            const std::int32_t inc     = Imax(1, 48000 / fs);
            const std::int32_t overlap = overlap48 / inc;
            if (channels == 1) {
                for (std::int32_t i = 0; i < overlap; ++i) {
                    float w       = window[i * inc];
                    w             = w * w;
                    const float g = w * g2 + (1.0f - w) * g1;
                    out[i]        = g * in[i];
                }
            } else {
                for (std::int32_t i = 0; i < overlap; ++i) {
                    float w        = window[i * inc];
                    w              = w * w;
                    const float g  = w * g2 + (1.0f - w) * g1;
                    out[i * 2]     = g * in[i * 2];
                    out[i * 2 + 1] = g * in[i * 2 + 1];
                }
            }
            std::int32_t c = 0;
            do {
                for (std::int32_t i = overlap; i < frameSize; ++i)
                    out[i * channels + c] = g2 * in[i * channels + c];
            } while (++c < channels);
        }

        // libopus: decide_fec() (src/opus_encoder.c)
        std::int32_t DecideFec(std::int32_t useInBandFec, std::int32_t packetLossPerc, std::int32_t lastFec, std::int32_t mode, std::int32_t& bandwidth,
                               std::int32_t rate) noexcept
        {
            if (useInBandFec == 0 || packetLossPerc == 0 || mode == ModeCeltOnly)
                return 0;
            const std::int32_t origBandwidth = bandwidth;
            for (;;) {
                std::int32_t       lbrrRateThresBps = FecThresholds[2 * (bandwidth - OPUS_BANDWIDTH_NARROWBAND)];
                const std::int32_t hysteresis       = FecThresholds[2 * (bandwidth - OPUS_BANDWIDTH_NARROWBAND) + 1];
                if (lastFec == 1)
                    lbrrRateThresBps -= hysteresis;
                if (lastFec == 0)
                    lbrrRateThresBps += hysteresis;
                lbrrRateThresBps = SilkSmulwb(lbrrRateThresBps * (125 - Imin(packetLossPerc, 25)), SilkFixConst(0.01, 16));
                // up to 5% loss: FEC if the rate allows; above: lower the bandwidth until it does
                if (rate > lbrrRateThresBps)
                    return 1;
                if (packetLossPerc <= 5)
                    return 0;
                if (bandwidth > OPUS_BANDWIDTH_NARROWBAND)
                    --bandwidth;
                else
                    break;
            }
            bandwidth = origBandwidth;
            return 0;
        }

        // libopus: compute_silk_rate_for_hybrid() (src/opus_encoder.c)
        std::int32_t ComputeSilkRateForHybrid(std::int32_t rate, std::int32_t bandwidth, std::int32_t frame20ms, std::int32_t vbr, std::int32_t fec,
                                              std::int32_t channels) noexcept
        {
            static constexpr std::int32_t RateTable[][5] = {{0, 0, 0, 0, 0},
                                                            {12000, 10000, 10000, 11000, 11000},
                                                            {16000, 13500, 13500, 15000, 15000},
                                                            {20000, 16000, 16000, 18000, 18000},
                                                            {24000, 18000, 18000, 21000, 21000},
                                                            {32000, 22000, 22000, 28000, 28000},
                                                            {64000, 38000, 38000, 50000, 50000}};
            rate /= channels; // per channel
            const std::int32_t entry = 1 + frame20ms + 2 * fec;
            const std::int32_t n     = static_cast<std::int32_t>(sizeof(RateTable) / sizeof(RateTable[0]));
            std::int32_t       i     = 1;
            for (; i < n; ++i)
                if (RateTable[i][0] > rate)
                    break;
            std::int32_t silkRate;
            if (i == n) {
                silkRate = RateTable[i - 1][entry];
                silkRate += (rate - RateTable[i - 1][0]) / 2; // half the extra bits to SILK
            } else {
                const std::int32_t lo = RateTable[i - 1][entry];
                const std::int32_t hi = RateTable[i][entry];
                const std::int32_t x0 = RateTable[i - 1][0];
                const std::int32_t x1 = RateTable[i][0];
                silkRate              = (lo * (x1 - rate) + hi * (rate - x0)) / (x1 - x0);
            }
            if (vbr == 0)
                silkRate += 100;
            if (bandwidth == OPUS_BANDWIDTH_SUPERWIDEBAND)
                silkRate += 300;
            silkRate *= channels;
            if (channels == 2 && rate >= 12000)
                silkRate -= 1000;
            return silkRate;
        }

        // libopus: compute_equiv_rate() (src/opus_encoder.c)
        // the equivalent bitrate for 20 ms frames, complexity 10, VBR
        std::int32_t ComputeEquivRate(std::int32_t bitrate, std::int32_t channels, std::int32_t frameRate, std::int32_t vbr, std::int32_t mode,
                                      std::int32_t complexity, std::int32_t loss) noexcept
        {
            std::int32_t equiv = bitrate;
            if (frameRate > 50)
                equiv -= (40 * channels + 20) * (frameRate - 50);
            if (vbr == 0)
                equiv -= equiv / 12;
            equiv = equiv * (90 + complexity) / 100;
            if (mode == ModeSilkOnly || mode == ModeHybrid) {
                if (complexity < 2)
                    equiv = equiv * 4 / 5;
                equiv -= equiv * loss / (6 * loss + 10);
            } else if (mode == ModeCeltOnly) {
                if (complexity < 5)
                    equiv = equiv * 9 / 10;
            } else {
                equiv -= equiv * loss / (12 * loss + 20);
            }
            return equiv;
        }

        // libopus: OPUS_GET_IN_DTX of opus_encoder_ctl() (src/opus_encoder.c)
        // the in-DTX state of the SILK encoder (OPUS_GET_IN_DTX for SILK and hybrid)
        std::int32_t SilkInDtx(const SilkEncoder* enc, std::int32_t nChannelsInternal) noexcept
        {
            std::int32_t value = enc->state_Fxx[0].sCmn.noSpeechCounter >= NbSpeechFramesBeforeDtx ? 1 : 0;
            if (value == 1 && nChannelsInternal == 2 && enc->prev_decode_only_middle == 0)
                value = enc->state_Fxx[1].sCmn.noSpeechCounter >= NbSpeechFramesBeforeDtx ? 1 : 0;
            return value;
        }

        // libopus: decide_dtx_mode() (src/opus_encoder.c)
        bool DecideDtxMode(std::int32_t activity, std::int32_t& nbNoActivityMsQ1, std::int32_t frameSizeMsQ1) noexcept
        {
            if (activity == 0) {
                nbNoActivityMsQ1 += frameSizeMsQ1;
                if (nbNoActivityMsQ1 > NbSpeechFramesBeforeDtx * 20 * 2) {
                    if (nbNoActivityMsQ1 <= (NbSpeechFramesBeforeDtx + MaxConsecutiveDtx) * 20 * 2)
                        return true;
                    nbNoActivityMsQ1 = NbSpeechFramesBeforeDtx * 20 * 2;
                }
            } else {
                nbNoActivityMsQ1 = 0;
            }
            return false;
        }

        // libopus: compute_redundancy_bytes() (src/opus_encoder.c)
        std::int32_t ComputeRedundancyBytes(std::int32_t maxDataBytes, std::int32_t bitrateBps, std::int32_t frameRate, std::int32_t channels) noexcept
        {
            const std::int32_t baseBits        = 40 * channels + 20;
            std::int32_t       redundancyRate  = bitrateBps + baseBits * (200 - frameRate); // equivalent rate for 5 ms frames
            redundancyRate                     = 3 * redundancyRate / 2;
            std::int32_t       redundancyBytes = redundancyRate / 1600;
            const std::int32_t availableBits   = maxDataBytes * 8 - 2 * baseBits;
            const std::int32_t cap             = (availableBits * 240 / (240 + 48000 / frameRate) + baseBits) / 8;
            redundancyBytes                    = Imin(redundancyBytes, cap);
            if (redundancyBytes > 4 + 8 * channels)
                redundancyBytes = Imin(257, redundancyBytes);
            else
                redundancyBytes = 0;
            return redundancyBytes;
        }

    } // namespace

    // libopus: opus_encoder_init() (src/opus_encoder.c)
    std::int32_t OpusStreamEncoder::Init(std::int32_t fs, std::int32_t channels, std::int32_t application) noexcept
    {
        if ((fs != 48000 && fs != 24000 && fs != 16000 && fs != 12000 && fs != 8000) || (channels != 1 && channels != 2)
            || (application != OPUS_APPLICATION_VOIP && application != OPUS_APPLICATION_AUDIO && application != OPUS_APPLICATION_RESTRICTED_LOWDELAY
                && application != OPUS_APPLICATION_RESTRICTED_SILK && application != OPUS_APPLICATION_RESTRICTED_CELT))
            return OPUS_BAD_ARG;

        std::int32_t silkSize = silk::GetEncoderSize(channels);
        if (application == OPUS_APPLICATION_RESTRICTED_CELT)
            silkSize = 0;
        if (!silk_.Allocate(static_cast<std::size_t>(silkSize)))
            return OPUS_ALLOC_FAIL;

        // OPUS_CLEAR of the whole state (the arrays are zeroed by Allocate)
        silkMode_    = {};
        application_ = channels_ = delayCompensation_ = forceChannels_ = signalType_ = 0;
        userBandwidth_ = maxBandwidth_ = userForcedMode_ = voiceRatio_ = fs_ = 0;
        useVbr_ = vbrConstraint_ = variableDuration_ = bitrateBps_ = userBitrateBps_ = 0;
        lsbDepth_ = encoderBuffer_ = lfe_ = arch_ = useDtx_ = fecConfig_ = 0;
        ResetState();

        streamChannels_ = channels_ = channels;
        fs_                         = fs;
        arch_                       = SelectArch();

        std::int32_t ret = 0;
        if (application != OPUS_APPLICATION_RESTRICTED_CELT)
            ret = silk::InitEncoder(Silk(), channels_, arch_, &silkMode_);
        if (ret != 0)
            return OPUS_INTERNAL_ERROR;

        // default SILK parameters
        silkMode_.nChannelsAPI              = channels;
        silkMode_.nChannelsInternal         = channels;
        silkMode_.API_sampleRate            = fs_;
        silkMode_.maxInternalSampleRate     = 16000;
        silkMode_.minInternalSampleRate     = 8000;
        silkMode_.desiredInternalSampleRate = 16000;
        silkMode_.payloadSize_ms            = 20;
        silkMode_.bitRate                   = 25000;
        silkMode_.packetLossPercentage      = 0;
        silkMode_.complexity                = 9;
        silkMode_.useInBandFEC              = 0;
        silkMode_.useDRED                   = 0;
        silkMode_.useDTX                    = 0;
        silkMode_.useCBR                    = 0;
        silkMode_.reducedDependency         = 0;

        if (application != OPUS_APPLICATION_RESTRICTED_SILK) {
            if (celt_.Init(fs, channels, arch_) != OPUS_OK)
                return OPUS_INTERNAL_ERROR;
            celt_.Ctl(CeltSetSignallingRequest, 0);
            celt_.Ctl(OPUS_SET_COMPLEXITY_REQUEST, silkMode_.complexity);
        }

        useVbr_           = 1;
        vbrConstraint_    = 1; // constrained VBR is the default
        userBitrateBps_   = OPUS_AUTO;
        bitrateBps_       = 3000 + fs * channels;
        application_      = application;
        signalType_       = OPUS_AUTO;
        userBandwidth_    = OPUS_AUTO;
        maxBandwidth_     = OPUS_BANDWIDTH_FULLBAND;
        forceChannels_    = OPUS_AUTO;
        userForcedMode_   = OPUS_AUTO;
        voiceRatio_       = -1;
        encoderBuffer_    = application != OPUS_APPLICATION_RESTRICTED_CELT && application != OPUS_APPLICATION_RESTRICTED_SILK ? fs_ / 100 : 0;
        lsbDepth_         = 24;
        variableDuration_ = OPUS_FRAMESIZE_ARG;
        // delay compensation of 4 ms (2.5 ms SILK look-ahead + 1.5 ms resamplers and stereo prediction)
        delayCompensation_    = fs_ / 250;
        hybridStereoWidthQ14_ = 1 << 14;
        prevHbGain_           = 1.0f;
        variableHpSmth2Q15_   = static_cast<std::int32_t>(static_cast<std::uint32_t>(silk::Lin2Log(VariableHpMinCutoffHz)) << 8);
        first_                = 1;
        mode_                 = ModeHybrid;
        bandwidth_            = OPUS_BANDWIDTH_FULLBAND;
        analysis_.Init(fs_);
        return OPUS_OK;
    }

    // libopus: OPUS_RESET_STATE of opus_encoder_ctl() (src/opus_encoder.c)
    void OpusStreamEncoder::ResetState() noexcept
    {
        streamChannels_       = 0;
        hybridStereoWidthQ14_ = 0;
        variableHpSmth2Q15_   = 0;
        prevHbGain_           = 0;
        std::memset(hpMem_, 0, sizeof(hpMem_));
        mode_ = prevMode_ = prevChannels_ = prevFramesize_ = 0;
        bandwidth_ = autoBandwidth_ = silkBwSwitch_ = first_ = 0;
        energyMasking_                                       = nullptr;
        widthMem_                                            = StereoWidthState();
        detectedBandwidth_                                   = 0;
        nbNoActivityMsQ1_                                    = 0;
        peakSignalEnergy_                                    = 0;
        nonfinalFrame_                                       = 0;
        rangeFinal_                                          = 0;
        std::memset(delayBuffer_, 0, sizeof(delayBuffer_));
    }

    // libopus: user_bitrate_to_bitrate() (src/opus_encoder.c)
    std::int32_t OpusStreamEncoder::UserBitrateToBitrate(std::int32_t frameSize, std::int32_t maxDataBytes) const noexcept
    {
        if (frameSize == 0)
            frameSize = fs_ / 400;
        const std::int32_t maxBitrate = BitsToBitrate(maxDataBytes * 8, fs_, frameSize);
        std::int32_t       userBitrate;
        if (userBitrateBps_ == OPUS_AUTO)
            userBitrate = 60 * fs_ / frameSize + fs_ * channels_;
        else if (userBitrateBps_ == OPUS_BITRATE_MAX)
            userBitrate = 1500000;
        else
            userBitrate = userBitrateBps_;
        return Imin(userBitrate, maxBitrate);
    }

    // libopus: compute_stereo_width() (src/opus_encoder.c)
    float OpusStreamEncoder::ComputeStereoWidth(const float* pcm, std::int32_t frameSize) noexcept
    {
        return StereoWidth(pcm, frameSize, fs_, widthMem_);
    }

    // libopus: compute_stereo_width() (src/opus_encoder.c)
    float StereoWidth(const float* pcm, std::int32_t frameSize, std::int32_t fs, StereoWidthState& mem) noexcept
    {
        const std::int32_t frameRate  = fs / frameSize;
        const float        shortAlpha = static_cast<float>(25) * 1.0f / Imax(50, frameRate);
        float              xx = 0, xy = 0, yy = 0;
        // unrolled by 4; 2.5 ms frames at 12 kHz drop the last two samples
        for (std::int32_t i = 0; i < frameSize - 3; i += 4) {
            float x   = pcm[2 * i];
            float y   = pcm[2 * i + 1];
            float pxx = x * x;
            float pxy = x * y;
            float pyy = y * y;
            x         = pcm[2 * i + 2];
            y         = pcm[2 * i + 3];
            pxx += x * x;
            pxy += x * y;
            pyy += y * y;
            x = pcm[2 * i + 4];
            y = pcm[2 * i + 5];
            pxx += x * x;
            pxy += x * y;
            pyy += y * y;
            x = pcm[2 * i + 6];
            y = pcm[2 * i + 7];
            pxx += x * x;
            pxy += x * y;
            pyy += y * y;
            xx += pxx;
            xy += pxy;
            yy += pyy;
        }
        if (!(xx < 1e9f) || xx != xx || !(yy < 1e9f) || yy != yy)
            xy = xx = yy = 0;
        mem.xx += shortAlpha * (xx - mem.xx);
        // rewritten upstream to avoid overflows on abrupt sign changes
        mem.xy = (1.0f - shortAlpha) * mem.xy + shortAlpha * xy;
        mem.yy += shortAlpha * (yy - mem.yy);
        mem.xx = 0 > mem.xx ? 0 : mem.xx;
        mem.xy = 0 > mem.xy ? 0 : mem.xy;
        mem.yy = 0 > mem.yy ? 0 : mem.yy;
        if ((mem.xx > mem.yy ? mem.xx : mem.yy) > 8e-4f) {
            const float sqrtXx = CeltSqrt(mem.xx);
            const float sqrtYy = CeltSqrt(mem.yy);
            const float qrrtXx = CeltSqrt(sqrtXx);
            const float qrrtYy = CeltSqrt(sqrtYy);
            // inter-channel correlation
            mem.xy           = mem.xy < sqrtXx * sqrtYy ? mem.xy : sqrtXx * sqrtYy;
            const float corr = static_cast<float>(mem.xy) / (1e-15f + sqrtXx * sqrtYy);
            // approximate loudness difference
            const float ldiff = 1.0f * Abs16(qrrtXx - qrrtYy) / (1e-15f + qrrtXx + qrrtYy);
            const float s     = CeltSqrt(1.f - corr * corr);
            const float width = (1.0f < s ? 1.0f : s) * ldiff;
            // smoothing over one second, then a peak follower
            mem.smoothedWidth += (width - mem.smoothedWidth) / frameRate;
            const float decayed = mem.maxFollower - .02f / frameRate;
            mem.maxFollower     = decayed > mem.smoothedWidth ? decayed : mem.smoothedWidth;
        }
        const float w = static_cast<float>(20) * mem.maxFollower;
        return 1.0f < w ? 1.0f : w;
    }

    // libopus: compute_frame_energy() (src/opus_encoder.c)
    float OpusStreamEncoder::ComputeFrameEnergy(const float* pcm, std::int32_t frameSize) const noexcept
    {
        const std::int32_t len = frameSize * channels_;
        return InnerProd(pcm, pcm, len) / len;
    }

    float OpusStreamEncoder::FrameEnergy(const float* pcm, std::int32_t frameSize) const noexcept
    {
        if (currentInput_ != nullptr && pcm == currentPcm_ && frameSize == currentInput_->frameSize)
            return currentInput_->energy;
        return ComputeFrameEnergy(pcm, frameSize);
    }

    // libopus: OPUS_SET_ENERGY_MASK of opus_encoder_ctl() (src/opus_encoder.c)
    std::int32_t OpusStreamEncoder::SetEnergyMask(const float* mask) noexcept
    {
        energyMasking_ = mask;
        if (application_ != OPUS_APPLICATION_RESTRICTED_SILK)
            celt_.SetEnergyMask(mask);
        return OPUS_OK;
    }

    // libopus: opus_encoder_ctl() (src/opus_encoder.c), the SET requests and OPUS_RESET_STATE
    std::int32_t OpusStreamEncoder::Ctl(std::int32_t request, std::int32_t value) noexcept
    {
        std::int32_t ret = OPUS_OK;
        switch (request) {
            case OPUS_SET_APPLICATION_REQUEST:
                if (application_ == OPUS_APPLICATION_RESTRICTED_SILK || application_ == OPUS_APPLICATION_RESTRICTED_CELT)
                    return OPUS_BAD_ARG;
                if ((value != OPUS_APPLICATION_VOIP && value != OPUS_APPLICATION_AUDIO && value != OPUS_APPLICATION_RESTRICTED_LOWDELAY)
                    || (first_ == 0 && application_ != value))
                    return OPUS_BAD_ARG;
                application_ = value;
                break;
            case OPUS_SET_BITRATE_REQUEST:
                if (value != OPUS_AUTO && value != OPUS_BITRATE_MAX) {
                    if (value <= 0)
                        return OPUS_BAD_ARG;
                    if (value <= 500)
                        value = 500;
                    else if (value > 750000 * channels_)
                        value = 750000 * channels_;
                }
                userBitrateBps_ = value;
                break;
            case OPUS_SET_FORCE_CHANNELS_REQUEST:
                if ((value < 1 || value > channels_) && value != OPUS_AUTO)
                    return OPUS_BAD_ARG;
                forceChannels_ = value;
                break;
            case OPUS_SET_MAX_BANDWIDTH_REQUEST:
                if (value < OPUS_BANDWIDTH_NARROWBAND || value > OPUS_BANDWIDTH_FULLBAND)
                    return OPUS_BAD_ARG;
                maxBandwidth_                   = value;
                silkMode_.maxInternalSampleRate = maxBandwidth_ == OPUS_BANDWIDTH_NARROWBAND ? 8000
                    : maxBandwidth_ == OPUS_BANDWIDTH_MEDIUMBAND                             ? 12000
                                                                                             : 16000;
                break;
            case OPUS_SET_BANDWIDTH_REQUEST:
                if ((value < OPUS_BANDWIDTH_NARROWBAND || value > OPUS_BANDWIDTH_FULLBAND) && value != OPUS_AUTO)
                    return OPUS_BAD_ARG;
                userBandwidth_                  = value;
                silkMode_.maxInternalSampleRate = userBandwidth_ == OPUS_BANDWIDTH_NARROWBAND ? 8000
                    : userBandwidth_ == OPUS_BANDWIDTH_MEDIUMBAND                             ? 12000
                                                                                              : 16000;
                break;
            case OPUS_SET_DTX_REQUEST:
                if (value < 0 || value > 1)
                    return OPUS_BAD_ARG;
                useDtx_ = value;
                break;
            case OPUS_SET_COMPLEXITY_REQUEST:
                if (value < 0 || value > 10)
                    return OPUS_BAD_ARG;
                silkMode_.complexity = value;
                if (application_ != OPUS_APPLICATION_RESTRICTED_SILK)
                    celt_.Ctl(OPUS_SET_COMPLEXITY_REQUEST, value);
                break;
            case OPUS_SET_INBAND_FEC_REQUEST:
                if (value < 0 || value > 2)
                    return OPUS_BAD_ARG;
                fecConfig_             = value;
                silkMode_.useInBandFEC = value != 0;
                break;
            case OPUS_SET_PACKET_LOSS_PERC_REQUEST:
                if (value < 0 || value > 100)
                    return OPUS_BAD_ARG;
                silkMode_.packetLossPercentage = value;
                if (application_ != OPUS_APPLICATION_RESTRICTED_SILK)
                    celt_.Ctl(OPUS_SET_PACKET_LOSS_PERC_REQUEST, value);
                break;
            case OPUS_SET_VBR_REQUEST:
                if (value < 0 || value > 1)
                    return OPUS_BAD_ARG;
                useVbr_          = value;
                silkMode_.useCBR = 1 - value;
                break;
            case OpusSetVoiceRatioRequest:
                if (value < -1 || value > 100)
                    return OPUS_BAD_ARG;
                voiceRatio_ = value;
                break;
            case OPUS_SET_VBR_CONSTRAINT_REQUEST:
                if (value < 0 || value > 1)
                    return OPUS_BAD_ARG;
                vbrConstraint_ = value;
                break;
            case OPUS_SET_SIGNAL_REQUEST:
                if (value != OPUS_AUTO && value != OPUS_SIGNAL_VOICE && value != OPUS_SIGNAL_MUSIC)
                    return OPUS_BAD_ARG;
                signalType_ = value;
                break;
            case OPUS_SET_LSB_DEPTH_REQUEST:
                if (value < 8 || value > 24)
                    return OPUS_BAD_ARG;
                lsbDepth_ = value;
                break;
            case OPUS_SET_EXPERT_FRAME_DURATION_REQUEST:
                if (value != OPUS_FRAMESIZE_ARG && value != OPUS_FRAMESIZE_2_5_MS && value != OPUS_FRAMESIZE_5_MS && value != OPUS_FRAMESIZE_10_MS
                    && value != OPUS_FRAMESIZE_20_MS && value != OPUS_FRAMESIZE_40_MS && value != OPUS_FRAMESIZE_60_MS && value != OPUS_FRAMESIZE_80_MS
                    && value != OPUS_FRAMESIZE_100_MS && value != OPUS_FRAMESIZE_120_MS)
                    return OPUS_BAD_ARG;
                variableDuration_ = value;
                break;
            case OPUS_SET_PREDICTION_DISABLED_REQUEST:
                if (value > 1 || value < 0)
                    return OPUS_BAD_ARG;
                silkMode_.reducedDependency = value;
                break;
            case OPUS_SET_PHASE_INVERSION_DISABLED_REQUEST:
                if (value < 0 || value > 1)
                    return OPUS_BAD_ARG;
                if (application_ != OPUS_APPLICATION_RESTRICTED_SILK)
                    celt_.Ctl(OPUS_SET_PHASE_INVERSION_DISABLED_REQUEST, value);
                break;
            case OPUS_RESET_STATE: {
                analysis_.Reset();
                ResetState();
                if (application_ != OPUS_APPLICATION_RESTRICTED_SILK)
                    celt_.Reset();
                if (application_ != OPUS_APPLICATION_RESTRICTED_CELT) {
                    SilkEncControl dummy;
                    silk::InitEncoder(Silk(), channels_, arch_, &dummy);
                }
                streamChannels_       = channels_;
                hybridStereoWidthQ14_ = 1 << 14;
                prevHbGain_           = 1.0f;
                first_                = 1;
                mode_                 = ModeHybrid;
                bandwidth_            = OPUS_BANDWIDTH_FULLBAND;
                variableHpSmth2Q15_   = static_cast<std::int32_t>(static_cast<std::uint32_t>(silk::Lin2Log(VariableHpMinCutoffHz)) << 8);
                break;
            }
            case OpusSetForceModeRequest:
                if ((value < ModeSilkOnly || value > ModeCeltOnly) && value != OPUS_AUTO)
                    return OPUS_BAD_ARG;
                userForcedMode_ = value;
                break;
            case 10024: // OPUS_SET_LFE_REQUEST
                lfe_ = value;
                if (application_ != OPUS_APPLICATION_RESTRICTED_SILK)
                    celt_.SetLfe(value);
                break;
            default: ret = OPUS_UNIMPLEMENTED; break;
        }
        return ret;
    }

    // libopus: opus_encoder_ctl() (src/opus_encoder.c), the GET requests
    std::int32_t OpusStreamEncoder::Get(std::int32_t request, std::int32_t& value) noexcept
    {
        switch (request) {
            case OPUS_GET_APPLICATION_REQUEST: value = application_; break;
            case OPUS_GET_BITRATE_REQUEST: value = UserBitrateToBitrate(prevFramesize_, 1276); break;
            case OPUS_GET_FORCE_CHANNELS_REQUEST: value = forceChannels_; break;
            case OPUS_GET_MAX_BANDWIDTH_REQUEST: value = maxBandwidth_; break;
            case OPUS_GET_BANDWIDTH_REQUEST: value = bandwidth_; break;
            case OPUS_GET_DTX_REQUEST: value = useDtx_; break;
            case OPUS_GET_COMPLEXITY_REQUEST: value = silkMode_.complexity; break;
            case OPUS_GET_INBAND_FEC_REQUEST: value = fecConfig_; break;
            case OPUS_GET_PACKET_LOSS_PERC_REQUEST: value = silkMode_.packetLossPercentage; break;
            case OPUS_GET_VBR_REQUEST: value = useVbr_; break;
            case OpusGetVoiceRatioRequest: value = voiceRatio_; break;
            case OPUS_GET_VBR_CONSTRAINT_REQUEST: value = vbrConstraint_; break;
            case OPUS_GET_SIGNAL_REQUEST: value = signalType_; break;
            case OPUS_GET_LOOKAHEAD_REQUEST:
                value = fs_ / 400;
                if (application_ != OPUS_APPLICATION_RESTRICTED_LOWDELAY && application_ != OPUS_APPLICATION_RESTRICTED_CELT)
                    value += delayCompensation_;
                break;
            case OPUS_GET_SAMPLE_RATE_REQUEST: value = fs_; break;
            case OPUS_GET_LSB_DEPTH_REQUEST: value = lsbDepth_; break;
            case OPUS_GET_EXPERT_FRAME_DURATION_REQUEST: value = variableDuration_; break;
            case OPUS_GET_PREDICTION_DISABLED_REQUEST: value = silkMode_.reducedDependency; break;
            case OPUS_GET_PHASE_INVERSION_DISABLED_REQUEST: {
                if (application_ != OPUS_APPLICATION_RESTRICTED_SILK) {
                    value = celt_.PhaseInversionDisabled();
                    return OPUS_OK;
                }
                value = 0;
                break;
            }
            case OPUS_GET_IN_DTX_REQUEST:
                if (silkMode_.useDTX != 0 && (prevMode_ == ModeSilkOnly || prevMode_ == ModeHybrid))
                    value = SilkInDtx(Silk(), silkMode_.nChannelsInternal);
                else if (useDtx_ != 0)
                    value = nbNoActivityMsQ1_ >= NbSpeechFramesBeforeDtx * 20 * 2;
                else
                    value = 0;
                break;
            default: return OPUS_UNIMPLEMENTED;
        }
        return OPUS_OK;
    }

    bool CeltInputChain::Init(const OpusStreamEncoder& encoder) noexcept
    {
        const std::int32_t application = encoder.application_;
        if (application == OPUS_APPLICATION_VOIP || application == OPUS_APPLICATION_RESTRICTED_SILK)
            return false;
        channels_      = encoder.channels_;
        fs_            = encoder.fs_;
        encoderBuffer_ = encoder.encoderBuffer_;
        totalBuffer_ = application == OPUS_APPLICATION_RESTRICTED_LOWDELAY || application == OPUS_APPLICATION_RESTRICTED_CELT ? 0 : encoder.delayCompensation_;
        std::memcpy(hpMem_, encoder.hpMem_, sizeof(hpMem_));
        std::memcpy(delayBuffer_, encoder.delayBuffer_, sizeof(delayBuffer_));
        lsbDepth_ = Imin(MaxEncodingDepth, encoder.lsbDepth_);
        widthMem_ = encoder.widthMem_;
        return channels_ <= 2 && encoderBuffer_ <= 480;
    }

    // libopus: opus_encode_frame_native() (src/opus_encoder.c), the delay buffer and dc_reject() of the CELT input
    const float* CeltInputChain::Next(const float* pcm, std::int32_t frameSize, StreamInputFrame* stage) noexcept
    {
        // the input stage as EncodeNative
        if (stage != nullptr) {
            const std::int32_t len = frameSize * channels_;
            stage->frameSize       = frameSize;
            stage->lsbDepth        = lsbDepth_;
            stage->silence         = IsDigitalSilence(pcm, frameSize, channels_, lsbDepth_);
            stage->energy          = InnerProd(pcm, pcm, len) / len;
            stage->width           = channels_ == 2;
            if (stage->width) {
                stage->widthBefore = widthMem_;
                stage->stereoWidth = StereoWidth(pcm, frameSize, fs_, widthMem_);
                stage->widthAfter  = widthMem_;
            }
            std::memcpy(stage->hpMemBefore, hpMem_, sizeof(hpMem_));
            if (stage->pcm != pcm)
                std::memcpy(stage->pcm, pcm, static_cast<std::size_t>(len) * sizeof(float));
        }
        // as EncodeFrameNative: the delayed samples, then the DC-rejected input
        std::memcpy(pcmBuf_, &delayBuffer_[(encoderBuffer_ - totalBuffer_) * channels_], sizeof(float) * totalBuffer_ * channels_);
        DcReject(pcm, 3, &pcmBuf_[totalBuffer_ * channels_], hpMem_, frameSize, channels_, fs_);
        {
            const float sum = InnerProd(&pcmBuf_[totalBuffer_ * channels_], &pcmBuf_[totalBuffer_ * channels_], frameSize * channels_);
            if (!(sum < 1e9f) || sum != sum) {
                std::memset(&pcmBuf_[totalBuffer_ * channels_], 0, sizeof(float) * frameSize * channels_);
                hpMem_[0] = hpMem_[1] = hpMem_[2] = hpMem_[3] = 0;
            }
        }
        if (stage != nullptr) {
            std::memcpy(stage->hpMemAfter, hpMem_, sizeof(hpMem_));
            std::memcpy(stage->dcRejected, &pcmBuf_[totalBuffer_ * channels_], sizeof(float) * frameSize * channels_);
        }
        if (channels_ * (encoderBuffer_ - (frameSize + totalBuffer_)) > 0) {
            std::memmove(delayBuffer_, &delayBuffer_[channels_ * frameSize], sizeof(float) * channels_ * (encoderBuffer_ - frameSize - totalBuffer_));
            std::memcpy(&delayBuffer_[channels_ * (encoderBuffer_ - frameSize - totalBuffer_)], &pcmBuf_[0],
                        sizeof(float) * (frameSize + totalBuffer_) * channels_);
        } else {
            std::memcpy(delayBuffer_, &pcmBuf_[(frameSize + totalBuffer_ - encoderBuffer_) * channels_], sizeof(float) * encoderBuffer_ * channels_);
        }
        return pcmBuf_;
    }

    bool OpusStreamEncoder::AnalysisEnabled() const noexcept
    {
        return silkMode_.complexity >= 7 && fs_ >= 16000 && fs_ <= 48000 && application_ != OPUS_APPLICATION_RESTRICTED_SILK;
    }

    // libopus: opus_encode_native() (src/opus_encoder.c)
    std::int32_t OpusStreamEncoder::EncodeNative(const float* pcm, std::int32_t frameSize, std::uint8_t* data, std::int32_t outDataBytes, std::int32_t lsbDepth,
                                                 const float* analysisPcm, std::int32_t analysisSize, std::int32_t c1, std::int32_t c2,
                                                 std::int32_t analysisChannels) noexcept
    {
        constexpr std::int32_t PacketSizeCap           = 1276;
        std::int32_t           ret                     = 0;
        std::int32_t           prefill                 = 0;
        bool                   redundancy              = false;
        bool                   celtToSilk              = false;
        bool                   toCelt                  = false;
        std::int32_t           cbrBytes                = -1;
        AnalysisInfo           analysisInfo            = {};
        std::int32_t           analysisReadPosBak      = -1;
        std::int32_t           analysisReadSubframeBak = -1;

        // avoid insane packet sizes here; the real bounds come later
        std::int32_t maxDataBytes = Imin(PacketSizeCap * 6, outDataBytes);
        rangeFinal_               = 0;
        if (frameSize <= 0 || maxDataBytes <= 0)
            return OPUS_BAD_ARG;
        // cannot encode 100 ms in 1 byte
        if (maxDataBytes == 1 && fs_ == frameSize * 10)
            return OPUS_BUFFER_TOO_SMALL;

        const std::int64_t call = callIndex_++;
        currentCall_            = call;
        lsbDepth                = Imin(lsbDepth, lsbDepth_);
        // the input stage from the chain, for exactly this input
        currentInput_ = nullptr;
        currentPcm_   = pcm;
        if (toneSource_ != nullptr) {
            const StreamInputFrame* input = toneSource_->WaitInput(toneStream_, call);
            if (input != nullptr && input->frameSize == frameSize
                && std::memcmp(input->pcm, pcm, sizeof(float) * static_cast<std::size_t>(frameSize * channels_)) == 0)
                currentInput_ = input;
        }
        bool isSilence
            = currentInput_ != nullptr && currentInput_->lsbDepth == lsbDepth ? currentInput_->silence : IsDigitalSilence(pcm, frameSize, channels_, lsbDepth);
        analysisInfo.valid = 0;
        if (AnalysisEnabled()) {
            analysisReadPosBak      = analysis_.ReadPos();
            analysisReadSubframeBak = analysis_.ReadSubframe();
            if (toneSource_ != nullptr) {
                // the chain analysed this call already: only the read
                toneChain_ = &toneSource_->Wait(toneStream_, call, toneView_);
                analysis_.GetInfoFrom(*toneChain_, toneView_, analysisInfo, frameSize);
            } else {
                analysis_.Run(analysisPcm, analysisSize, frameSize, c1, c2, analysisChannels, lsbDepth, analysisInfo);
            }
        } else if (analysis_.Initialized()) {
            analysis_.Reset();
        }

        // reset voice_ratio unless the frame is silent (keep it from the last non-silent frame)
        if (!isSilence)
            voiceRatio_ = -1;
        detectedBandwidth_ = 0;
        if (analysisInfo.valid != 0) {
            if (signalType_ == OPUS_AUTO) {
                float prob;
                if (prevMode_ == 0)
                    prob = analysisInfo.musicProb;
                else if (prevMode_ == ModeCeltOnly)
                    prob = analysisInfo.musicProbMax;
                else
                    prob = analysisInfo.musicProbMin;
                voiceRatio_ = static_cast<std::int32_t>(std::floor(.5 + 100 * (1 - prob)));
            }
            const std::int32_t analysisBandwidth = analysisInfo.bandwidth;
            if (analysisBandwidth <= 12)
                detectedBandwidth_ = OPUS_BANDWIDTH_NARROWBAND;
            else if (analysisBandwidth <= 14)
                detectedBandwidth_ = OPUS_BANDWIDTH_MEDIUMBAND;
            else if (analysisBandwidth <= 16)
                detectedBandwidth_ = OPUS_BANDWIDTH_WIDEBAND;
            else if (analysisBandwidth <= 18)
                detectedBandwidth_ = OPUS_BANDWIDTH_SUPERWIDEBAND;
            else
                detectedBandwidth_ = OPUS_BANDWIDTH_FULLBAND;
        }

        // track the peak signal energy
        if (analysisInfo.valid == 0 || analysisInfo.activityProbability > DtxActivityThreshold) {
            if (!isSilence) {
                const float decayed = 0.999f * peakSignalEnergy_;
                const float energy  = FrameEnergy(pcm, frameSize);
                peakSignalEnergy_   = decayed > energy ? decayed : energy;
            }
        }
        float stereoWidth = 0;
        if (channels_ == 2 && forceChannels_ != 1) {
            if (currentInput_ != nullptr && currentInput_->width && std::memcmp(&currentInput_->widthBefore, &widthMem_, sizeof(StereoWidthState)) == 0) {
                stereoWidth = currentInput_->stereoWidth;
                widthMem_   = currentInput_->widthAfter;
            } else {
                stereoWidth = ComputeStereoWidth(pcm, frameSize);
            }
        }
        bitrateBps_ = UserBitrateToBitrate(frameSize, maxDataBytes);

        std::int32_t frameRate = fs_ / frameSize;
        if (useVbr_ == 0) {
            cbrBytes     = Imin((BitrateToBits(bitrateBps_, fs_, frameSize) + 4) / 8, maxDataBytes);
            bitrateBps_  = BitsToBitrate(cbrBytes * 8, fs_, frameSize);
            maxDataBytes = Imax(1, cbrBytes); // at least one byte
        }
        if (maxDataBytes < 3 || bitrateBps_ < 3 * frameRate * 8 || (frameRate < 50 && (maxDataBytes * frameRate < 300 || bitrateBps_ < 2400))) {
            // too little space to do something useful: a 'PLC' frame
            std::int32_t tocmode        = mode_;
            std::int32_t bw             = bandwidth_ == 0 ? OPUS_BANDWIDTH_NARROWBAND : bandwidth_;
            std::int32_t packetCode     = 0;
            std::int32_t numMultiframes = 0;
            if (tocmode == 0)
                tocmode = ModeSilkOnly;
            if (frameRate > 100)
                tocmode = ModeCeltOnly;
            // 40 ms -> 2 x 20 ms in CELT_ONLY or HYBRID
            if (frameRate == 25 && tocmode != ModeSilkOnly) {
                frameRate  = 50;
                packetCode = 1;
            }
            // >= 60 ms frames
            if (frameRate <= 16) {
                if (outDataBytes == 1 || (tocmode == ModeSilkOnly && frameRate != 10)) {
                    tocmode    = ModeSilkOnly;
                    packetCode = frameRate <= 12;
                    frameRate  = frameRate == 12 ? 25 : 16;
                } else {
                    numMultiframes = 50 / frameRate;
                    frameRate      = 50;
                    packetCode     = 3;
                }
            }
            if (tocmode == ModeSilkOnly && bw > OPUS_BANDWIDTH_WIDEBAND)
                bw = OPUS_BANDWIDTH_WIDEBAND;
            else if (tocmode == ModeCeltOnly && bw == OPUS_BANDWIDTH_MEDIUMBAND)
                bw = OPUS_BANDWIDTH_NARROWBAND;
            else if (tocmode == ModeHybrid && bw <= OPUS_BANDWIDTH_SUPERWIDEBAND)
                bw = OPUS_BANDWIDTH_SUPERWIDEBAND;
            data[0] = GenToc(tocmode, frameRate, bw, streamChannels_);
            data[0] |= static_cast<std::uint8_t>(packetCode);
            ret          = packetCode <= 1 ? 1 : 2;
            maxDataBytes = Imax(maxDataBytes, ret);
            if (packetCode == 3)
                data[1] = static_cast<std::uint8_t>(numMultiframes);
            if (useVbr_ == 0) {
                ret = PacketPad(data, ret, maxDataBytes);
                ret = ret == OPUS_OK ? maxDataBytes : OPUS_INTERNAL_ERROR;
            }
            return ret;
        }
        const std::int32_t maxRate = BitsToBitrate(maxDataBytes * 8, fs_, frameSize);

        // equivalent 20-ms rate for the mode, channel and bandwidth decisions
        std::int32_t equivRate = ComputeEquivRate(bitrateBps_, channels_, fs_ / frameSize, useVbr_, 0, silkMode_.complexity, silkMode_.packetLossPercentage);

        std::int32_t voiceEst; // probability of voice in Q7
        if (signalType_ == OPUS_SIGNAL_VOICE)
            voiceEst = 127;
        else if (signalType_ == OPUS_SIGNAL_MUSIC)
            voiceEst = 0;
        else if (voiceRatio_ >= 0) {
            voiceEst = voiceRatio_ * 327 >> 8;
            if (application_ == OPUS_APPLICATION_AUDIO)
                voiceEst = Imin(voiceEst, 115); // never more than 90% sure of speech
        } else if (application_ == OPUS_APPLICATION_VOIP)
            voiceEst = 115;
        else
            voiceEst = 48;

        if (forceChannels_ != OPUS_AUTO && channels_ == 2) {
            streamChannels_ = forceChannels_;
        } else if (channels_ == 2) {
            // rate-dependent mono/stereo decision
            std::int32_t stereoThreshold = StereoMusicThreshold + ((voiceEst * voiceEst * (StereoVoiceThreshold - StereoMusicThreshold)) >> 14);
            if (streamChannels_ == 2)
                stereoThreshold -= 1000;
            else
                stereoThreshold += 1000;
            streamChannels_ = equivRate > stereoThreshold ? 2 : 1;
        } else {
            streamChannels_ = channels_;
        }
        equivRate = ComputeEquivRate(bitrateBps_, streamChannels_, fs_ / frameSize, useVbr_, 0, silkMode_.complexity, silkMode_.packetLossPercentage);

        // SILK DTX if DTX is on but the generalized DTX cannot be used
        silkMode_.useDTX = useDtx_ != 0 && !(analysisInfo.valid != 0 || isSilence);

        // mode selection by application and signal type
        if (application_ == OPUS_APPLICATION_RESTRICTED_SILK) {
            mode_ = ModeSilkOnly;
        } else if (application_ == OPUS_APPLICATION_RESTRICTED_LOWDELAY || application_ == OPUS_APPLICATION_RESTRICTED_CELT) {
            mode_ = ModeCeltOnly;
        } else if (userForcedMode_ == OPUS_AUTO) {
            // interpolate on the stereo width, then on the speech/music probability
            const auto   modeVoice = static_cast<std::int32_t>((1.0f - stereoWidth) * ModeThresholds[0][0] + stereoWidth * ModeThresholds[1][0]);
            const auto   modeMusic = static_cast<std::int32_t>((1.0f - stereoWidth) * ModeThresholds[1][1] + stereoWidth * ModeThresholds[1][1]);
            std::int32_t threshold = modeMusic + ((voiceEst * voiceEst * (modeVoice - modeMusic)) >> 14);
            if (application_ == OPUS_APPLICATION_VOIP)
                threshold += 8000; // bias towards SILK for VoIP
            // hysteresis
            if (prevMode_ == ModeCeltOnly)
                threshold -= 4000;
            else if (prevMode_ > 0)
                threshold += 4000;
            mode_ = equivRate >= threshold ? ModeCeltOnly : ModeSilkOnly;
            // FEC with enough packet loss: SILK (unless FEC is 2 and we are confident of music)
            if (silkMode_.useInBandFEC != 0 && silkMode_.packetLossPercentage > (128 - voiceEst) >> 4 && (fecConfig_ != 2 || voiceEst > 25))
                mode_ = ModeSilkOnly;
            // voice with DTX but no generalized DTX: SILK's DTX
            if (silkMode_.useDTX != 0 && voiceEst > 100)
                mode_ = ModeSilkOnly;
            // less than 6 kb/s: CELT only
            if (maxDataBytes < BitrateToBits(frameRate > 50 ? 9000 : 6000, fs_, frameSize) / 8)
                mode_ = ModeCeltOnly;
        } else {
            mode_ = userForcedMode_;
        }

        // the frame size the caller asked for
        if (mode_ != ModeCeltOnly && frameSize < fs_ / 100)
            mode_ = ModeCeltOnly;
        if (lfe_ != 0 && application_ != OPUS_APPLICATION_RESTRICTED_SILK)
            mode_ = ModeCeltOnly;

        if (prevMode_ > 0 && ((mode_ != ModeCeltOnly && prevMode_ == ModeCeltOnly) || (mode_ == ModeCeltOnly && prevMode_ != ModeCeltOnly))) {
            redundancy = true;
            celtToSilk = mode_ != ModeCeltOnly;
            if (!celtToSilk) {
                // switch to SILK/hybrid if the frame is 10 ms or more
                if (frameSize >= fs_ / 100) {
                    mode_  = prevMode_;
                    toCelt = true;
                } else {
                    redundancy = false;
                }
            }
        }

        // delay a stereo->mono transition by two frames so SILK can downmix smoothly
        if (streamChannels_ == 1 && prevChannels_ == 2 && silkMode_.toMono == 0 && mode_ != ModeCeltOnly && prevMode_ != ModeCeltOnly) {
            silkMode_.toMono = 1;
            streamChannels_  = 2;
        } else {
            silkMode_.toMono = 0;
        }

        equivRate = ComputeEquivRate(bitrateBps_, streamChannels_, fs_ / frameSize, useVbr_, mode_, silkMode_.complexity, silkMode_.packetLossPercentage);

        if (mode_ != ModeCeltOnly && prevMode_ == ModeCeltOnly) {
            SilkEncControl dummy;
            silk::InitEncoder(Silk(), channels_, arch_, &dummy);
            prefill = 1;
        }

        // automatic (rate-dependent) bandwidth selection
        if (mode_ == ModeCeltOnly || first_ != 0 || silkMode_.allowBandwidthSwitch != 0) {
            const std::int32_t* voiceThresholds;
            const std::int32_t* musicThresholds;
            std::int32_t        bandwidthThresholds[8];
            std::int32_t        bandwidth = OPUS_BANDWIDTH_FULLBAND;
            if (channels_ == 2 && forceChannels_ != 1) {
                voiceThresholds = StereoVoiceBandwidthThresholds;
                musicThresholds = StereoMusicBandwidthThresholds;
            } else {
                voiceThresholds = MonoVoiceBandwidthThresholds;
                musicThresholds = MonoMusicBandwidthThresholds;
            }
            for (std::int32_t i = 0; i < 8; ++i)
                bandwidthThresholds[i] = musicThresholds[i] + ((voiceEst * voiceEst * (voiceThresholds[i] - musicThresholds[i])) >> 14);
            do {
                std::int32_t       threshold  = bandwidthThresholds[2 * (bandwidth - OPUS_BANDWIDTH_MEDIUMBAND)];
                const std::int32_t hysteresis = bandwidthThresholds[2 * (bandwidth - OPUS_BANDWIDTH_MEDIUMBAND) + 1];
                if (first_ == 0) {
                    if (autoBandwidth_ >= bandwidth)
                        threshold -= hysteresis;
                    else
                        threshold += hysteresis;
                }
                if (equivRate >= threshold)
                    break;
            } while (--bandwidth > OPUS_BANDWIDTH_NARROWBAND);
            // no mediumband any more, except on request or in mode transitions
            if (bandwidth == OPUS_BANDWIDTH_MEDIUMBAND)
                bandwidth = OPUS_BANDWIDTH_WIDEBAND;
            bandwidth_ = autoBandwidth_ = bandwidth;
            // no SWB/FB until SILK has fully switched to WB and turned the variable LP filter off
            if (first_ == 0 && mode_ != ModeCeltOnly && silkMode_.inWBmodeWithoutVariableLP == 0 && bandwidth_ > OPUS_BANDWIDTH_WIDEBAND)
                bandwidth_ = OPUS_BANDWIDTH_WIDEBAND;
        }

        if (bandwidth_ > maxBandwidth_)
            bandwidth_ = maxBandwidth_;
        if (userBandwidth_ != OPUS_AUTO)
            bandwidth_ = userBandwidth_;
        // no hybrid at unsafe CBR/max rates
        if (mode_ != ModeCeltOnly && maxRate < 15000)
            bandwidth_ = Imin(bandwidth_, OPUS_BANDWIDTH_WIDEBAND);
        // no bits above the Nyquist rate of the input
        if (fs_ <= 24000 && bandwidth_ > OPUS_BANDWIDTH_SUPERWIDEBAND)
            bandwidth_ = OPUS_BANDWIDTH_SUPERWIDEBAND;
        if (fs_ <= 16000 && bandwidth_ > OPUS_BANDWIDTH_WIDEBAND)
            bandwidth_ = OPUS_BANDWIDTH_WIDEBAND;
        if (fs_ <= 12000 && bandwidth_ > OPUS_BANDWIDTH_MEDIUMBAND)
            bandwidth_ = OPUS_BANDWIDTH_MEDIUMBAND;
        if (fs_ <= 8000 && bandwidth_ > OPUS_BANDWIDTH_NARROWBAND)
            bandwidth_ = OPUS_BANDWIDTH_NARROWBAND;
        // the detected bandwidth reduces the encoded one (conservatively)
        if (detectedBandwidth_ != 0 && userBandwidth_ == OPUS_AUTO) {
            std::int32_t minDetectedBandwidth;
            if (equivRate <= 18000 * streamChannels_ && mode_ == ModeCeltOnly)
                minDetectedBandwidth = OPUS_BANDWIDTH_NARROWBAND;
            else if (equivRate <= 24000 * streamChannels_ && mode_ == ModeCeltOnly)
                minDetectedBandwidth = OPUS_BANDWIDTH_MEDIUMBAND;
            else if (equivRate <= 30000 * streamChannels_)
                minDetectedBandwidth = OPUS_BANDWIDTH_WIDEBAND;
            else if (equivRate <= 44000 * streamChannels_)
                minDetectedBandwidth = OPUS_BANDWIDTH_SUPERWIDEBAND;
            else
                minDetectedBandwidth = OPUS_BANDWIDTH_FULLBAND;
            detectedBandwidth_ = Imax(detectedBandwidth_, minDetectedBandwidth);
            bandwidth_         = Imin(bandwidth_, detectedBandwidth_);
        }
        silkMode_.LBRR_coded = DecideFec(silkMode_.useInBandFEC, silkMode_.packetLossPercentage, silkMode_.LBRR_coded, mode_, bandwidth_, equivRate);
        if (application_ != OPUS_APPLICATION_RESTRICTED_SILK)
            celt_.Ctl(OPUS_SET_LSB_DEPTH_REQUEST, lsbDepth);

        // CELT has no mediumband
        if (mode_ == ModeCeltOnly && bandwidth_ == OPUS_BANDWIDTH_MEDIUMBAND)
            bandwidth_ = OPUS_BANDWIDTH_WIDEBAND;
        if (lfe_ != 0)
            bandwidth_ = OPUS_BANDWIDTH_NARROWBAND;

        std::int32_t currBandwidth = bandwidth_;
        if (application_ == OPUS_APPLICATION_RESTRICTED_SILK && currBandwidth > OPUS_BANDWIDTH_WIDEBAND)
            bandwidth_ = currBandwidth = OPUS_BANDWIDTH_WIDEBAND;
        // the mode for speech; never switch to or from CELT-only here
        if (mode_ == ModeSilkOnly && currBandwidth > OPUS_BANDWIDTH_WIDEBAND)
            mode_ = ModeHybrid;
        if (mode_ == ModeHybrid && currBandwidth <= OPUS_BANDWIDTH_WIDEBAND)
            mode_ = ModeSilkOnly;

        // no frames over 60 ms, and none over 20 ms in hybrid or CELT-only
        if ((frameSize > fs_ / 50 && mode_ != ModeSilkOnly) || frameSize > 3 * fs_ / 50) {
            std::int32_t encFrameSize;
            if (mode_ == ModeSilkOnly) {
                if (frameSize == 2 * fs_ / 25) // 80 ms -> 2x 40 ms
                    encFrameSize = fs_ / 25;
                else if (frameSize == 3 * fs_ / 25) // 120 ms -> 2x 60 ms
                    encFrameSize = 3 * fs_ / 50;
                else // 100 ms -> 5x 20 ms
                    encFrameSize = fs_ / 50;
            } else {
                encFrameSize = fs_ / 50;
            }
            const std::int32_t nbFrames = frameSize / encFrameSize;
            if (analysisReadPosBak != -1) {
                // back to the start of the first frame, to use the analysis one frame at a time
                analysis_.SetRead(analysisReadPosBak, analysisReadSubframeBak);
            }
            // worst cases: 2 frames code 2 with different sizes, more frames code 3 VBR
            const std::int32_t maxHeaderBytes = nbFrames == 2 ? 3 : (2 + (nbFrames - 1) * 2);
            std::int32_t       repacketizeLen;
            if (useVbr_ != 0 || userBitrateBps_ == OPUS_BITRATE_MAX)
                repacketizeLen = outDataBytes;
            else
                repacketizeLen = Imin(cbrBytes, outDataBytes);
            const std::int32_t maxLenSum = nbFrames + repacketizeLen - maxHeaderBytes;

            HeapArray<std::uint8_t> tmpData;
            if (!tmpData.Allocate(static_cast<std::size_t>(maxLenSum > 0 ? maxLenSum : 1)))
                return OPUS_ALLOC_FAIL;
            std::uint8_t* currData = tmpData.Data();
            Repacketizer  rp;
            rp.Init();

            const std::int32_t bakToMono = silkMode_.toMono;
            if (bakToMono != 0)
                forceChannels_ = 1;
            else
                prevChannels_ = streamChannels_;

            std::int32_t totSize  = 0;
            std::int32_t dtxCount = 0;
            for (std::int32_t i = 0; i < nbFrames; ++i) {
                silkMode_.toMono = 0;
                nonfinalFrame_   = i < nbFrames - 1;
                // from SILK/hybrid to CELT: ask for the switch only in the last frame
                const bool   frameToCelt     = toCelt && i == nbFrames - 1;
                const bool   frameRedundancy = redundancy && (frameToCelt || (!toCelt && i == 0));
                std::int32_t currMax         = Imin(BitrateToBits(bitrateBps_, fs_, encFrameSize) / 8, maxLenSum / nbFrames);
                currMax                      = Imin(maxLenSum - totSize, currMax);
                if (analysisReadPosBak != -1) {
                    // the analysis of this frame
                    if (toneSource_ != nullptr)
                        analysis_.GetInfoFrom(*toneChain_, toneView_, analysisInfo, encFrameSize);
                    else
                        analysis_.GetInfo(analysisInfo, encFrameSize);
                }
                isSilence                 = IsDigitalSilence(pcm + i * (channels_ * encFrameSize), encFrameSize, channels_, lsbDepth);
                currentSignal_            = nullptr; // the signal stage is per call, not per sub-frame
                const std::int32_t tmpLen = EncodeFrameNative(pcm + i * (channels_ * encFrameSize), encFrameSize, currData, currMax, &analysisInfo, isSilence,
                                                              frameRedundancy, celtToSilk, prefill, equivRate, frameToCelt);
                if (tmpLen < 0)
                    return OPUS_INTERNAL_ERROR;
                if (tmpLen == 1)
                    ++dtxCount;
                ret = rp.Cat(currData, tmpLen);
                if (ret < 0)
                    return OPUS_INTERNAL_ERROR;
                totSize += tmpLen;
                currData += tmpLen;
            }
            ret = rp.OutRange(0, nbFrames, data, repacketizeLen, false, useVbr_ == 0 && dtxCount != nbFrames);
            if (ret < 0)
                ret = OPUS_INTERNAL_ERROR;
            silkMode_.toMono = bakToMono;
            return ret;
        }
        // the CELT signal stage and front part of this frame, if a chain computed them
        currentSignal_ = toneSource_ != nullptr ? toneSource_->WaitSignal(toneStream_, call) : nullptr;
        currentFront_  = currentSignal_ != nullptr && frontMisses_ < MaxFrontMisses ? toneSource_->WaitFront(toneStream_, call) : nullptr;
        celtCoded_     = false;
        ret            = EncodeFrameNative(pcm, frameSize, data, maxDataBytes, &analysisInfo, isSilence, redundancy, celtToSilk, prefill, equivRate, toCelt);
        // the chain's front parts no longer fit (a miss, or a new tapset decision): again from the real state
        if (currentSignal_ != nullptr && celtCoded_ && frontMisses_ < MaxFrontMisses) {
            if (!celt_.LastFrontUsed()) {
                ++frontMisses_;
                FrontSync sync;
                sync.state        = celt_.GetFrontState();
                sync.params       = celt_.LastFrontParams();
                sync.readPos      = analysis_.ReadPos();
                sync.readSubframe = analysis_.ReadSubframe();
                toneSource_->Resync(toneStream_, call + 1, sync);
            } else {
                frontMisses_ = 0;
            }
        }
        currentSignal_ = nullptr;
        currentFront_  = nullptr;
        currentInput_  = nullptr;
        return ret;
    }

    // libopus: opus_encode_frame_native() (src/opus_encoder.c)
    std::int32_t OpusStreamEncoder::EncodeFrameNative(const float* pcm, std::int32_t frameSize, std::uint8_t* data, std::int32_t origMaxDataBytes,
                                                      const AnalysisInfo* analysisInfo, bool isSilence, bool redundancy, bool celtToSilk, std::int32_t prefill,
                                                      std::int32_t equivRate, bool toCelt) noexcept
    {
        std::int32_t  ret             = 0;
        std::int32_t  startBand       = 0;
        std::int32_t  redundancyBytes = 0; // bytes of the redundancy frame
        std::uint32_t redundantRng    = 0;
        std::int32_t  activity        = VadNoDecision;
        std::int32_t  nBytes          = 0;

        const std::int32_t maxDataBytes = Imin(origMaxDataBytes, 1276);
        rangeFinal_                     = 0;
        const CeltEncoder* celtMode     = nullptr;
        if (application_ != OPUS_APPLICATION_RESTRICTED_SILK)
            celtMode = &celt_;
        std::int32_t       currBandwidth     = bandwidth_;
        const std::int32_t delayCompensation = application_ == OPUS_APPLICATION_RESTRICTED_LOWDELAY || application_ == OPUS_APPLICATION_RESTRICTED_CELT
                || application_ == OPUS_APPLICATION_RESTRICTED_SILK
            ? 0
            : delayCompensation_;
        const std::int32_t totalBuffer       = delayCompensation;
        const std::int32_t frameRate         = fs_ / frameSize;

        if (isSilence) {
            activity = 0;
        } else if (analysisInfo->valid != 0) {
            activity = analysisInfo->activityProbability >= DtxActivityThreshold;
            if (activity == 0) {
                // active anyway if this noise frame is loud enough
                const float noiseEnergy = FrameEnergy(pcm, frameSize);
                activity                = peakSignalEnergy_ < (PseudoSnrThreshold * noiseEnergy);
            }
        } else if (mode_ == ModeCeltOnly) {
            const float noiseEnergy = FrameEnergy(pcm, frameSize);
            // the peak boosted a bit, since we did not just average the active frames
            activity = peakSignalEnergy_ < (PseudoSnrThreshold * (.5f * noiseEnergy));
        }

        // the first frame at a new SILK bandwidth
        if (silkBwSwitch_ != 0) {
            redundancy    = true;
            celtToSilk    = true;
            silkBwSwitch_ = 0;
            prefill       = 2; // prefill without resetting the sampling rate control
        }
        // with CELT, no redundancy, whatever was decided earlier
        if (mode_ == ModeCeltOnly)
            redundancy = false;
        if (redundancy) {
            redundancyBytes = ComputeRedundancyBytes(maxDataBytes, bitrateBps_, frameRate, streamChannels_);
            if (redundancyBytes == 0)
                redundancy = false;
        }
        if (application_ == OPUS_APPLICATION_RESTRICTED_SILK) {
            redundancy      = false;
            redundancyBytes = 0;
        }

        const std::int32_t bitsTarget = Imin(8 * (maxDataBytes - redundancyBytes), BitrateToBits(bitrateBps_, fs_, frameSize)) - 8;

        data += 1;
        RangeEncoder enc;
        enc.Init(data, static_cast<std::uint32_t>(origMaxDataBytes - 1));

        // kept from frame to frame (written before it is read)
        if (!frameInput_.Grow(static_cast<std::size_t>(totalBuffer + frameSize) * channels_))
            return OPUS_INTERNAL_ERROR;
        float* pcmBuf = frameInput_.Data();
        std::memcpy(pcmBuf, &delayBuffer_[(encoderBuffer_ - totalBuffer) * channels_], sizeof(float) * totalBuffer * channels_);

        std::int32_t hpFreqSmth1;
        if (mode_ == ModeCeltOnly)
            hpFreqSmth1 = static_cast<std::int32_t>(static_cast<std::uint32_t>(silk::Lin2Log(VariableHpMinCutoffHz)) << 8);
        else
            hpFreqSmth1 = Silk()->state_Fxx[0].sCmn.variable_HP_smth1_Q15;
        variableHpSmth2Q15_ = SilkSmlawb(variableHpSmth2Q15_, hpFreqSmth1 - variableHpSmth2Q15_, SilkFixConst(VariableHpSmthCoef2, 16));
        // from log scale to Hertz
        const std::int32_t cutoffHz = silk::Log2Lin(variableHpSmth2Q15_ >> 8);

        // the DC rejection (and the check for NaNs) from the chain for the same input and filter state
        const bool dcGiven = application_ != OPUS_APPLICATION_VOIP && currentInput_ != nullptr && pcm == currentPcm_ && frameSize == currentInput_->frameSize
            && std::memcmp(currentInput_->hpMemBefore, hpMem_, sizeof(hpMem_)) == 0;
        if (dcGiven) {
            std::memcpy(&pcmBuf[totalBuffer * channels_], currentInput_->dcRejected, sizeof(float) * frameSize * channels_);
            std::memcpy(hpMem_, currentInput_->hpMemAfter, sizeof(hpMem_));
        } else if (application_ == OPUS_APPLICATION_VOIP) {
            HpCutoff(pcm, cutoffHz, &pcmBuf[totalBuffer * channels_], hpMem_, frameSize, channels_, fs_);
        } else {
            DcReject(pcm, 3, &pcmBuf[totalBuffer * channels_], hpMem_, frameSize, channels_, fs_);
        }
        if (!dcGiven) {
            // filter out NaNs and ridiculous signals that could cause NaNs further down
            const float sum = InnerProd(&pcmBuf[totalBuffer * channels_], &pcmBuf[totalBuffer * channels_], frameSize * channels_);
            if (!(sum < 1e9f) || sum != sum) {
                std::memset(&pcmBuf[totalBuffer * channels_], 0, sizeof(float) * frameSize * channels_);
                hpMem_[0] = hpMem_[1] = hpMem_[2] = hpMem_[3] = 0;
            }
        }

        // SILK processing
        float hbGain = 1.0f;
        if (mode_ != ModeCeltOnly) {
            // distribute the bits between SILK and CELT
            const std::int32_t totalBitRate = BitsToBitrate(bitsTarget, fs_, frameSize);
            if (mode_ == ModeHybrid) {
                silkMode_.bitRate
                    = ComputeSilkRateForHybrid(totalBitRate, currBandwidth, fs_ == 50 * frameSize, useVbr_, silkMode_.LBRR_coded, streamChannels_);
                if (energyMasking_ == nullptr) {
                    // the fewer bits the high band gets, the more it is attenuated
                    const std::int32_t celtRate = totalBitRate - silkMode_.bitRate;
                    hbGain                      = 1.0f - CeltExp2(-celtRate * (1.f / 1024));
                }
            } else {
                silkMode_.bitRate = totalBitRate; // SILK gets all bits
            }

            // surround masking for SILK
            if (energyMasking_ != nullptr && useVbr_ != 0 && lfe_ == 0) {
                float        maskSum = 0;
                std::int32_t end     = 17;
                std::int16_t srate   = 16000;
                if (bandwidth_ == OPUS_BANDWIDTH_NARROWBAND) {
                    end   = 13;
                    srate = 8000;
                } else if (bandwidth_ == OPUS_BANDWIDTH_MEDIUMBAND) {
                    end   = 15;
                    srate = 12000;
                }
                for (std::int32_t c = 0; c < channels_; ++c) {
                    for (std::int32_t i = 0; i < end; ++i) {
                        float mask = energyMasking_[21 * c + i];
                        mask       = mask < .5f ? mask : .5f;
                        mask       = mask > -2.0f ? mask : -2.0f;
                        if (mask > 0)
                            mask = .5f * mask;
                        maskSum += mask;
                    }
                }
                // conservative rate reduction: half the masking
                float masking_depth = maskSum / end * channels_;
                masking_depth += .2f;
                std::int32_t rateOffset = static_cast<std::int32_t>(static_cast<float>(srate) * masking_depth);
                rateOffset              = rateOffset > -2 * silkMode_.bitRate / 3 ? rateOffset : -2 * silkMode_.bitRate / 3;
                // split the rate change between SILK and CELT for hybrid
                if (bandwidth_ == OPUS_BANDWIDTH_SUPERWIDEBAND || bandwidth_ == OPUS_BANDWIDTH_FULLBAND)
                    silkMode_.bitRate += 3 * rateOffset / 5;
                else
                    silkMode_.bitRate += rateOffset;
            }

            silkMode_.payloadSize_ms    = 1000 * frameSize / fs_;
            silkMode_.nChannelsAPI      = channels_;
            silkMode_.nChannelsInternal = streamChannels_;
            if (currBandwidth == OPUS_BANDWIDTH_NARROWBAND)
                silkMode_.desiredInternalSampleRate = 8000;
            else if (currBandwidth == OPUS_BANDWIDTH_MEDIUMBAND)
                silkMode_.desiredInternalSampleRate = 12000;
            else
                silkMode_.desiredInternalSampleRate = 16000;
            // no bandwidth reduction at the lowest rates in hybrid
            silkMode_.minInternalSampleRate = mode_ == ModeHybrid ? 16000 : 8000;
            silkMode_.maxInternalSampleRate = 16000;
            if (mode_ == ModeSilkOnly) {
                std::int32_t effectiveMaxRate = BitsToBitrate(maxDataBytes * 8, fs_, frameSize);
                if (frameRate > 50)
                    effectiveMaxRate = effectiveMaxRate * 2 / 3;
                if (effectiveMaxRate < 8000) {
                    silkMode_.maxInternalSampleRate     = 12000;
                    silkMode_.desiredInternalSampleRate = Imin(12000, silkMode_.desiredInternalSampleRate);
                }
                if (effectiveMaxRate < 7000) {
                    silkMode_.maxInternalSampleRate     = 8000;
                    silkMode_.desiredInternalSampleRate = Imin(8000, silkMode_.desiredInternalSampleRate);
                }
            }
            silkMode_.useCBR = useVbr_ == 0;

            // max bits for SILK, counting the ToC, the redundancy bytes and their signalling
            silkMode_.maxBits = (maxDataBytes - 1) * 8;
            if (redundancy && redundancyBytes >= 2) {
                silkMode_.maxBits -= redundancyBytes * 8 + 1;
                if (mode_ == ModeHybrid)
                    silkMode_.maxBits -= 20;
            }
            if (silkMode_.useCBR != 0) {
                // CBR with non-SILK data: SILK runs VBR with a cap; CELT absorbs the variation
                if (mode_ == ModeHybrid) {
                    // SILK may steal up to 25% of the remaining bits
                    const auto otherBits = static_cast<std::int16_t>(Imax(0, silkMode_.maxBits - silkMode_.bitRate * frameSize / fs_));
                    silkMode_.maxBits    = Imax(0, silkMode_.maxBits - otherBits * 3 / 4);
                    silkMode_.useCBR     = 0;
                }
            } else if (mode_ == ModeHybrid) {
                // constrained VBR: the SILK rate for the max total bits
                const std::int32_t maxBitRate = ComputeSilkRateForHybrid(silkMode_.maxBits * fs_ / frameSize, currBandwidth, fs_ == 50 * frameSize, useVbr_,
                                                                         silkMode_.LBRR_coded, streamChannels_);
                silkMode_.maxBits             = BitrateToBits(maxBitRate, fs_, frameSize);
            }

            if (prefill != 0 && application_ != OPUS_APPLICATION_RESTRICTED_SILK) {
                // a smooth onset for the SILK prefill, so the encoder does not code a discontinuity
                std::int32_t       zero          = 0;
                const std::int32_t prefillOffset = channels_ * (encoderBuffer_ - delayCompensation_ - fs_ / 400);
                GainFade(delayBuffer_ + prefillOffset, delayBuffer_ + prefillOffset, 0, 1.0f, celtMode->ModeOverlap(), fs_ / 400, channels_,
                         celtMode->ModeWindow(), fs_);
                std::memset(delayBuffer_, 0, sizeof(float) * prefillOffset);
                silk::Encode(Silk(), &silkMode_, delayBuffer_, encoderBuffer_, nullptr, &zero, prefill, activity);
                silkMode_.opusCanSwitch = 0; // no second switch in the real encode call
            }

            ret = silk::Encode(Silk(), &silkMode_, pcmBuf + totalBuffer * channels_, frameSize, &enc, &nBytes, 0, activity);
            if (ret != 0)
                return OPUS_INTERNAL_ERROR;

            // SILK's internal bandwidth for the first byte
            if (mode_ == ModeSilkOnly) {
                if (silkMode_.internalSampleRate == 8000)
                    currBandwidth = OPUS_BANDWIDTH_NARROWBAND;
                else if (silkMode_.internalSampleRate == 12000)
                    currBandwidth = OPUS_BANDWIDTH_MEDIUMBAND;
                else if (silkMode_.internalSampleRate == 16000)
                    currBandwidth = OPUS_BANDWIDTH_WIDEBAND;
            }
            silkMode_.opusCanSwitch = silkMode_.switchReady != 0 && nonfinalFrame_ == 0;
            if (activity == VadNoDecision)
                activity = silkMode_.signalType != TypeNoVoiceActivity;
            if (nBytes == 0) {
                rangeFinal_ = 0;
                data[-1]    = GenToc(mode_, fs_ / frameSize, currBandwidth, streamChannels_);
                return 1;
            }
            if (silkMode_.opusCanSwitch != 0) {
                if (application_ != OPUS_APPLICATION_RESTRICTED_SILK) {
                    redundancyBytes = ComputeRedundancyBytes(maxDataBytes, bitrateBps_, frameRate, streamChannels_);
                    redundancy      = redundancyBytes != 0;
                }
                celtToSilk    = false;
                silkBwSwitch_ = 1;
            }
        }

        // CELT processing
        if (application_ != OPUS_APPLICATION_RESTRICTED_SILK) {
            std::int32_t endband = 21;
            switch (currBandwidth) {
                case OPUS_BANDWIDTH_NARROWBAND: endband = 13; break;
                case OPUS_BANDWIDTH_MEDIUMBAND:
                case OPUS_BANDWIDTH_WIDEBAND: endband = 17; break;
                case OPUS_BANDWIDTH_SUPERWIDEBAND: endband = 19; break;
                case OPUS_BANDWIDTH_FULLBAND: endband = 21; break;
                default: break;
            }
            celt_.Ctl(CeltSetEndBandRequest, endband);
            celt_.Ctl(CeltSetChannelsRequest, streamChannels_);
            celt_.Ctl(OPUS_SET_BITRATE_REQUEST, OPUS_BITRATE_MAX);
        }
        if (mode_ != ModeSilkOnly) {
            // prediction may still be disabled later
            const std::int32_t celtPred = silkMode_.reducedDependency != 0 ? 0 : 2;
            celt_.Ctl(CeltSetPredictionRequest, celtPred);
        }

        float tmpPrefill[2 * 48000 / 400];
        if (mode_ != ModeSilkOnly && mode_ != prevMode_ && prevMode_ > 0 && application_ != OPUS_APPLICATION_RESTRICTED_CELT)
            std::memcpy(tmpPrefill, &delayBuffer_[(encoderBuffer_ - totalBuffer - fs_ / 400) * channels_], sizeof(float) * channels_ * fs_ / 400);

        if (channels_ * (encoderBuffer_ - (frameSize + totalBuffer)) > 0) {
            std::memmove(delayBuffer_, &delayBuffer_[channels_ * frameSize], sizeof(float) * channels_ * (encoderBuffer_ - frameSize - totalBuffer));
            std::memcpy(&delayBuffer_[channels_ * (encoderBuffer_ - frameSize - totalBuffer)], &pcmBuf[0],
                        sizeof(float) * (frameSize + totalBuffer) * channels_);
        } else {
            std::memcpy(delayBuffer_, &pcmBuf[(frameSize + totalBuffer - encoderBuffer_) * channels_], sizeof(float) * encoderBuffer_ * channels_);
        }
        // gain_fade and stereo_fade after the buffer copy: they must not affect the SILK part
        if ((prevHbGain_ < 1.0f || hbGain < 1.0f) && celtMode != nullptr)
            GainFade(pcmBuf, pcmBuf, prevHbGain_, hbGain, celtMode->ModeOverlap(), frameSize, channels_, celtMode->ModeWindow(), fs_);
        prevHbGain_ = hbGain;
        if (mode_ != ModeHybrid || streamChannels_ == 1) {
            if (equivRate > 32000)
                silkMode_.stereoWidth_Q14 = 16384;
            else if (equivRate < 16000)
                silkMode_.stereoWidth_Q14 = 0;
            else
                silkMode_.stereoWidth_Q14 = 16384 - 2048 * (32000 - equivRate) / (equivRate - 14000);
        }
        if (energyMasking_ == nullptr && channels_ == 2) {
            // stereo width reduction at low bitrates
            if (hybridStereoWidthQ14_ < (1 << 14) || silkMode_.stereoWidth_Q14 < (1 << 14)) {
                float g1 = hybridStereoWidthQ14_;
                float g2 = static_cast<float>(silkMode_.stereoWidth_Q14);
                g1 *= (1.f / 16384);
                g2 *= (1.f / 16384);
                if (celtMode != nullptr)
                    StereoFade(pcmBuf, pcmBuf, g1, g2, celtMode->ModeOverlap(), frameSize, channels_, celtMode->ModeWindow(), fs_);
                hybridStereoWidthQ14_ = static_cast<std::int16_t>(silkMode_.stereoWidth_Q14);
            }
        }

        if (mode_ != ModeCeltOnly && enc.Tell() + 17 + 20 * (mode_ == ModeHybrid) <= 8 * (maxDataBytes - 1)) {
            // SILK-only infers the redundancy from the length
            if (mode_ == ModeHybrid)
                enc.EncodeBitLogp(redundancy, 12);
            if (redundancy) {
                std::int32_t maxRedundancy;
                enc.EncodeBitLogp(celtToSilk, 1);
                if (mode_ == ModeHybrid) {
                    // the 8 bits of the redundancy length and a few bits for CELT if possible
                    maxRedundancy = (maxDataBytes - 1) - ((enc.Tell() + 8 + 3 + 7) >> 3);
                } else {
                    maxRedundancy = (maxDataBytes - 1) - ((enc.Tell() + 7) >> 3);
                }
                // the same rate for the redundancy as for the rest, up to 257 bytes
                redundancyBytes = Imin(maxRedundancy, redundancyBytes);
                redundancyBytes = Imin(257, Imax(2, redundancyBytes));
                if (mode_ == ModeHybrid)
                    enc.EncodeUint(static_cast<std::uint32_t>(redundancyBytes - 2), 256);
            }
        } else {
            redundancy = false;
        }
        if (!redundancy) {
            silkBwSwitch_   = 0;
            redundancyBytes = 0;
        }
        if (mode_ != ModeCeltOnly)
            startBand = 17;

        std::int32_t nbComprBytes;
        if (mode_ == ModeSilkOnly) {
            ret = (enc.Tell() + 7) >> 3;
            enc.Done();
            nbComprBytes = ret;
        } else {
            nbComprBytes = (maxDataBytes - 1) - redundancyBytes;
            enc.Shrink(static_cast<std::uint32_t>(nbComprBytes));
        }

        if (redundancy || mode_ != ModeSilkOnly)
            celt_.SetAnalysis(*analysisInfo);
        if (mode_ == ModeHybrid)
            celt_.SetSilkInfo(silkMode_.signalType, silkMode_.offset);

        // 5 ms redundant frame for CELT->SILK
        if (redundancy && celtToSilk) {
            celt_.Ctl(CeltSetStartBandRequest, 0);
            celt_.Ctl(OPUS_SET_VBR_REQUEST, 0);
            celt_.Ctl(OPUS_SET_BITRATE_REQUEST, OPUS_BITRATE_MAX);
            const std::int32_t err = celt_.Encode(pcmBuf, fs_ / 200, data + nbComprBytes, redundancyBytes, nullptr);
            if (err < 0)
                return OPUS_INTERNAL_ERROR;
            redundantRng = celt_.FinalRange();
            celt_.Reset();
        }

        if (application_ != OPUS_APPLICATION_RESTRICTED_SILK)
            celt_.Ctl(CeltSetStartBandRequest, startBand);

        data[-1] = 0;
        if (mode_ != ModeSilkOnly) {
            celt_.Ctl(OPUS_SET_VBR_REQUEST, useVbr_);
            if (mode_ == ModeHybrid) {
                if (useVbr_ != 0) {
                    celt_.Ctl(OPUS_SET_BITRATE_REQUEST, bitrateBps_ - silkMode_.bitRate);
                    celt_.Ctl(OPUS_SET_VBR_CONSTRAINT_REQUEST, 0);
                }
            } else if (useVbr_ != 0) {
                celt_.Ctl(OPUS_SET_VBR_REQUEST, 1);
                celt_.Ctl(OPUS_SET_VBR_CONSTRAINT_REQUEST, vbrConstraint_);
                celt_.Ctl(OPUS_SET_BITRATE_REQUEST, bitrateBps_);
            }
            if (mode_ != prevMode_ && prevMode_ > 0 && application_ != OPUS_APPLICATION_RESTRICTED_CELT) {
                std::uint8_t dummy[2];
                celt_.Reset();
                // prefilling
                celt_.Encode(tmpPrefill, fs_ / 400, dummy, 2, nullptr);
                celt_.Ctl(CeltSetPredictionRequest, 0);
            }
            // if false, the budget is already busted: a "PLC frame"
            if (enc.Tell() <= 8 * nbComprBytes) {
                // the next call's inputs as expected now (for a speculation of the CELT encoder)
                // (and of the calls after it, for a cascade of speculations)
                constexpr std::int32_t Ahead       = 1 + CeltSpeculation::MaxAfter;
                NextCeltFrame          next[Ahead] = {};
                bool                   haveNext    = false;
                if (toneSource_ != nullptr && currentSignal_ != nullptr && mode_ == ModeCeltOnly) {
                    ToneAnalysis::View views[Ahead] = {};
                    std::int32_t       count        = 0;
                    while (count < Ahead && toneSource_->PeekCall(toneStream_, currentCall_ + 1 + count, views[count], next[count].signal, next[count].front))
                        ++count;
                    haveNext = count != 0;
                    if (haveNext) {
                        AnalysisInfo infos[Ahead] = {};
                        if (AnalysisEnabled() && toneChain_ != nullptr)
                            analysis_.PeekInfoFrom(*toneChain_, views, infos, count, frameSize);
                        for (std::int32_t i = 0; i < count; ++i) {
                            next[i].pcm      = next[i].signal->pcm;
                            next[i].analysis = infos[i];
                            next[i].after    = i + 1 < count ? &next[i + 1] : nullptr;
                        }
                    }
                }
                celt_.SetNextFrame(haveNext ? &next[0] : nullptr);
                ret = celt_.Encode(pcmBuf, frameSize, nullptr, nbComprBytes, &enc, currentSignal_, currentFront_);
                celt_.SetNextFrame(nullptr);
                celtCoded_ = true;
                if (ret < 0)
                    return OPUS_INTERNAL_ERROR;
                // the CELT->SILK redundancy data to the right place
                if (redundancy && celtToSilk && mode_ == ModeHybrid && nbComprBytes != ret) {
                    std::memmove(data + ret, data + nbComprBytes, static_cast<std::size_t>(redundancyBytes));
                    nbComprBytes = ret + redundancyBytes;
                }
            }
            rangeFinal_ = celt_.FinalRange();
        } else {
            rangeFinal_ = enc.Range();
        }

        // 5 ms redundant frame for SILK->CELT
        if (redundancy && !celtToSilk) {
            std::uint8_t       dummy[2];
            const std::int32_t n2 = fs_ / 200;
            const std::int32_t n4 = fs_ / 400;
            celt_.Reset();
            celt_.Ctl(CeltSetStartBandRequest, 0);
            celt_.Ctl(CeltSetPredictionRequest, 0);
            celt_.Ctl(OPUS_SET_VBR_REQUEST, 0);
            celt_.Ctl(OPUS_SET_BITRATE_REQUEST, OPUS_BITRATE_MAX);
            if (mode_ == ModeHybrid) {
                // shrink the packet to what the encoder actually used
                nbComprBytes = ret;
                enc.Shrink(static_cast<std::uint32_t>(nbComprBytes));
            }
            celt_.Encode(pcmBuf + channels_ * (frameSize - n2 - n4), n4, dummy, 2, nullptr);
            const std::int32_t err = celt_.Encode(pcmBuf + channels_ * (frameSize - n2), n2, data + nbComprBytes, redundancyBytes, nullptr);
            if (err < 0)
                return OPUS_INTERNAL_ERROR;
            redundantRng = celt_.FinalRange();
        }

        // the mode in the first byte
        --data;
        data[0] |= GenToc(mode_, fs_ / frameSize, currBandwidth, streamChannels_);
        rangeFinal_ ^= redundantRng;
        prevMode_      = toCelt ? ModeCeltOnly : mode_;
        prevChannels_  = streamChannels_;
        prevFramesize_ = frameSize;
        first_         = 0;

        // DTX decision
        if (useDtx_ != 0 && silkMode_.useDTX == 0) {
            if (DecideDtxMode(activity, nbNoActivityMsQ1_, 2 * 1000 * frameSize / fs_)) {
                rangeFinal_ = 0;
                data[0]     = GenToc(mode_, fs_ / frameSize, currBandwidth, streamChannels_);
                return 1;
            }
        } else {
            nbNoActivityMsQ1_ = 0;
        }

        // SILK busted its target: tell the decoder to use the PLC
        if (enc.Tell() > (maxDataBytes - 1) * 8) {
            if (maxDataBytes < 2)
                return OPUS_BUFFER_TOO_SMALL;
            data[1]     = 0;
            ret         = 1;
            rangeFinal_ = 0;
        } else if (mode_ == ModeSilkOnly && !redundancy) {
            // LPC-only frames may drop trailing zero bytes (the decoder fills them in)
            while (ret > 2 && data[ret] == 0)
                --ret;
        }
        // count the ToC and the redundancy
        ret += 1 + redundancyBytes;
        if (useVbr_ == 0) {
            if (PacketPad(data, ret, origMaxDataBytes) != OPUS_OK)
                return OPUS_INTERNAL_ERROR;
            ret = origMaxDataBytes;
        }
        return ret;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
