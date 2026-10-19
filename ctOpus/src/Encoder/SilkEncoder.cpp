// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "SilkEncoder.h"

#include <cstring>

#include "CeltMath.h"
#include "SilkCoding.h"
#include "SilkDefines.h"
#include "SilkEncodeFrame.h"
#include "SilkMacros.h"
#include "SilkResampler.h"
#include "SilkSigProc.h"
#include "SilkStereo.h"
#include "SilkTables.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk {

    namespace {

        // errors.h
        constexpr std::int32_t EncInputInvalidNoOfSamples  = -101;
        constexpr std::int32_t EncFsNotSupported           = -102;
        constexpr std::int32_t EncPacketSizeNotSupported   = -103;
        constexpr std::int32_t EncInvalidLossRate          = -105;
        constexpr std::int32_t EncInvalidComplexitySetting = -106;
        constexpr std::int32_t EncInvalidInbandFecSetting  = -107;
        constexpr std::int32_t EncInvalidDtxSetting        = -108;
        constexpr std::int32_t EncInvalidCbrSetting        = -109;
        constexpr std::int32_t EncInvalidNumberOfChannels  = -111;

        constexpr std::int32_t MaxApiSamples60Ms = 6 * 480; // 60 ms at 48 kHz

        // FLOAT2INT16 (RES2INT16 of the float build)
        inline std::int16_t Res2Int16(float a) noexcept
        {
            float x = a * 32768.f;
            x       = x > -32768 ? x : -32768;
            x       = x < 32767 ? x : 32767;
            return static_cast<std::int16_t>(Opus::Float2Int(x));
        }

        std::int32_t VadInit(SilkVadState* vad) noexcept
        {
            std::memset(vad, 0, sizeof(SilkVadState));
            // approximate pink noise levels (psd proportional to the inverse of the frequency)
            for (std::int32_t b = 0; b < SilkVadNBands; ++b)
                vad->NoiseLevelBias[b] = Max32(Div32_16(VadNoiseLevelsBias, b + 1), 1);
            for (std::int32_t b = 0; b < SilkVadNBands; ++b) {
                vad->NL[b]     = 100 * vad->NoiseLevelBias[b];
                vad->inv_NL[b] = Div32(Int32Max, vad->NL[b]);
            }
            vad->counter = 15;
            // the smoothed energy-to-noise ratio: 100 * 256 is 20 dB SNR
            for (std::int32_t b = 0; b < SilkVadNBands; ++b)
                vad->NrgRatioSmth_Q8[b] = 100 * 256;
            return 0;
        }

        std::int32_t InitEncoderState(SilkEncoderStateFlp* enc, std::int32_t arch) noexcept
        {
            std::memset(enc, 0, sizeof(SilkEncoderStateFlp));
            enc->sCmn.arch                  = arch;
            enc->sCmn.variable_HP_smth1_Q15 = Lshift(Lin2Log(FixConst(VariableHpMinCutoffHz, 16)) - (16 << 7), 8);
            enc->sCmn.variable_HP_smth2_Q15 = enc->sCmn.variable_HP_smth1_Q15;
            // deactivates LSF interpolation and pitch prediction
            enc->sCmn.first_frame_after_reset = 1;
            return VadInit(&enc->sCmn.sVAD);
        }

        std::int32_t QueryEncoder(const SilkEncoder* enc, SilkEncControl* encStatus) noexcept
        {
            const SilkEncoderStateFlp* s         = enc->state_Fxx;
            encStatus->nChannelsAPI              = enc->nChannelsAPI;
            encStatus->nChannelsInternal         = enc->nChannelsInternal;
            encStatus->API_sampleRate            = s[0].sCmn.API_fs_Hz;
            encStatus->maxInternalSampleRate     = s[0].sCmn.maxInternal_fs_Hz;
            encStatus->minInternalSampleRate     = s[0].sCmn.minInternal_fs_Hz;
            encStatus->desiredInternalSampleRate = s[0].sCmn.desiredInternal_fs_Hz;
            encStatus->payloadSize_ms            = s[0].sCmn.PacketSize_ms;
            encStatus->bitRate                   = s[0].sCmn.TargetRate_bps;
            encStatus->packetLossPercentage      = s[0].sCmn.PacketLoss_perc;
            encStatus->complexity                = s[0].sCmn.Complexity;
            encStatus->useInBandFEC              = s[0].sCmn.useInBandFEC;
            encStatus->useDTX                    = s[0].sCmn.useDTX;
            encStatus->useCBR                    = s[0].sCmn.useCBR;
            encStatus->internalSampleRate        = Smulbb(s[0].sCmn.fs_kHz, 1000);
            encStatus->allowBandwidthSwitch      = s[0].sCmn.allow_bandwidth_switch;
            encStatus->inWBmodeWithoutVariableLP = s[0].sCmn.fs_kHz == 16 && s[0].sCmn.sLP.mode == 0;
            return SilkNoError;
        }

        std::int32_t CheckControlInput(const SilkEncControl* c) noexcept
        {
            const auto validApi = [](std::int32_t r) { return r == 8000 || r == 12000 || r == 16000 || r == 24000 || r == 32000 || r == 44100 || r == 48000; };
            const auto validInt = [](std::int32_t r) { return r == 8000 || r == 12000 || r == 16000; };
            if (!validApi(c->API_sampleRate) || !validInt(c->desiredInternalSampleRate) || !validInt(c->maxInternalSampleRate)
                || !validInt(c->minInternalSampleRate) || c->minInternalSampleRate > c->desiredInternalSampleRate
                || c->maxInternalSampleRate < c->desiredInternalSampleRate || c->minInternalSampleRate > c->maxInternalSampleRate)
                return EncFsNotSupported;
            if (c->payloadSize_ms != 10 && c->payloadSize_ms != 20 && c->payloadSize_ms != 40 && c->payloadSize_ms != 60)
                return EncPacketSizeNotSupported;
            if (c->packetLossPercentage < 0 || c->packetLossPercentage > 100)
                return EncInvalidLossRate;
            if (c->useDTX < 0 || c->useDTX > 1)
                return EncInvalidDtxSetting;
            if (c->useCBR < 0 || c->useCBR > 1)
                return EncInvalidCbrSetting;
            if (c->useInBandFEC < 0 || c->useInBandFEC > 1)
                return EncInvalidInbandFecSetting;
            if (c->nChannelsAPI < 1 || c->nChannelsAPI > SilkEncoderNumChannels)
                return EncInvalidNumberOfChannels;
            if (c->nChannelsInternal < 1 || c->nChannelsInternal > SilkEncoderNumChannels)
                return EncInvalidNumberOfChannels;
            if (c->nChannelsInternal > c->nChannelsAPI)
                return EncInvalidNumberOfChannels;
            if (c->complexity < 0 || c->complexity > 10)
                return EncInvalidComplexitySetting;
            return SilkNoError;
        }

        // the internal sampling rate
        std::int32_t ControlAudioBandwidth(SilkEncoderState* e, SilkEncControl* c) noexcept
        {
            std::int32_t origKhz = e->fs_kHz;
            // a bandwidth-switching reset needs the last sampling rate
            if (origKhz == 0)
                origKhz = e->sLP.saved_fs_kHz;
            std::int32_t fsKhz = origKhz;
            std::int32_t fsHz  = Smulbb(fsKhz, 1000);
            if (fsHz == 0) {
                // just initialised
                fsHz  = Min(e->desiredInternal_fs_Hz, e->API_fs_Hz);
                fsKhz = Div32_16(fsHz, 1000);
            } else if (fsHz > e->API_fs_Hz || fsHz > e->maxInternal_fs_Hz || fsHz < e->minInternal_fs_Hz) {
                // not above the external rate or the maximum, not below the minimum
                fsHz  = e->API_fs_Hz;
                fsHz  = Min(fsHz, e->maxInternal_fs_Hz);
                fsHz  = Max(fsHz, e->minInternal_fs_Hz);
                fsKhz = Div32_16(fsHz, 1000);
            } else {
                // the state machine of the internal rate switching
                if (e->sLP.transition_frame_no >= TransitionFrames)
                    e->sLP.mode = 0; // end of the transition phase
                if (e->allow_bandwidth_switch != 0 || c->opusCanSwitch != 0) {
                    if (Smulbb(origKhz, 1000) > e->desiredInternal_fs_Hz) {
                        // switch down
                        if (e->sLP.mode == 0) {
                            // a new transition, with a reset transition filter
                            e->sLP.transition_frame_no = TransitionFrames;
                            std::memset(e->sLP.In_LP_State, 0, sizeof(e->sLP.In_LP_State));
                        }
                        if (c->opusCanSwitch != 0) {
                            // no transition phase, a lower rate
                            e->sLP.mode = 0;
                            fsKhz       = origKhz == 16 ? 12 : 8;
                        } else {
                            if (e->sLP.transition_frame_no <= 0) {
                                c->switchReady = 1;
                                // room for redundancy
                                c->maxBits -= c->maxBits * 5 / (c->payloadSize_ms + 5);
                            } else {
                                e->sLP.mode = -2; // down, at double speed
                            }
                        }
                    } else if (Smulbb(origKhz, 1000) < e->desiredInternal_fs_Hz) {
                        // switch up
                        if (c->opusCanSwitch != 0) {
                            fsKhz                      = origKhz == 8 ? 12 : 16;
                            e->sLP.transition_frame_no = 0;
                            std::memset(e->sLP.In_LP_State, 0, sizeof(e->sLP.In_LP_State));
                            e->sLP.mode = 1;
                        } else {
                            if (e->sLP.mode == 0) {
                                c->switchReady = 1;
                                // room for redundancy
                                c->maxBits -= c->maxBits * 5 / (c->payloadSize_ms + 5);
                            } else {
                                e->sLP.mode = 1;
                            }
                        }
                    } else {
                        if (e->sLP.mode < 0)
                            e->sLP.mode = 1;
                    }
                }
            }
            return fsKhz;
        }

        std::int32_t SetupResamplers(SilkEncoderStateFlp* enc, std::int32_t fsKhz) noexcept
        {
            std::int32_t ret = SilkNoError;
            if (enc->sCmn.fs_kHz != fsKhz || enc->sCmn.prev_API_fs_Hz != enc->sCmn.API_fs_Hz) {
                if (enc->sCmn.fs_kHz == 0) {
                    // the resampler of enc_API.c from API_fs_Hz to fs_kHz
                    ret += ResamplerInit(&enc->sCmn.resampler_state, enc->sCmn.API_fs_Hz, fsKhz * 1000, true);
                } else {
                    const std::int32_t bufLengthMs   = Lshift(enc->sCmn.nb_subfr * 5, 1) + LaShapeMs;
                    const std::int32_t oldBufSamples = bufLengthMs * enc->sCmn.fs_kHz;
                    const std::int32_t newBufSamples = bufLengthMs * fsKhz;
                    std::int16_t       xBufFix[2 * SilkMaxFrameLength + SilkLaShapeMax];
                    Float2ShortArray(xBufFix, enc->x_buf, oldBufSamples);

                    // a temporary resampler of the x_buf data to API_fs_Hz
                    SilkResamplerState tempResamplerState;
                    ret += ResamplerInit(&tempResamplerState, Smulbb(enc->sCmn.fs_kHz, 1000), enc->sCmn.API_fs_Hz, false);
                    const std::int32_t apiBufSamples = bufLengthMs * Div32_16(enc->sCmn.API_fs_Hz, 1000);
                    std::int16_t       xBufApiFsHz[45 * 48];
                    ret += Resample(&tempResamplerState, xBufApiFsHz, xBufFix, oldBufSamples);

                    // the resampler of enc_API.c, its state corrected by resampling the buffered data
                    ret += ResamplerInit(&enc->sCmn.resampler_state, enc->sCmn.API_fs_Hz, Smulbb(fsKhz, 1000), true);
                    ret += Resample(&enc->sCmn.resampler_state, xBufFix, xBufApiFsHz, apiBufSamples);
                    Short2FloatArray(enc->x_buf, xBufFix, newBufSamples);
                }
            }
            enc->sCmn.prev_API_fs_Hz = enc->sCmn.API_fs_Hz;
            return ret;
        }

        std::int32_t SetupFs(SilkEncoderStateFlp* enc, std::int32_t fsKhz, std::int32_t packetSizeMs) noexcept
        {
            std::int32_t      ret = SilkNoError;
            SilkEncoderState& c   = enc->sCmn;
            // the packet size
            if (packetSizeMs != c.PacketSize_ms) {
                if (packetSizeMs != 10 && packetSizeMs != 20 && packetSizeMs != 40 && packetSizeMs != 60)
                    ret = EncPacketSizeNotSupported;
                if (packetSizeMs <= 10) {
                    c.nFramesPerPacket     = 1;
                    c.nb_subfr             = packetSizeMs == 10 ? 2 : 1;
                    c.frame_length         = Smulbb(packetSizeMs, fsKhz);
                    c.pitch_LPC_win_length = Smulbb(FindPitchLpcWinMs2Sf, fsKhz);
                    c.pitch_contour_iCDF   = c.fs_kHz == 8 ? pitch_contour_10_ms_NB_iCDF : pitch_contour_10_ms_iCDF;
                } else {
                    c.nFramesPerPacket     = Div32_16(packetSizeMs, MaxFrameLengthMs);
                    c.nb_subfr             = SilkMaxNbSubfr;
                    c.frame_length         = Smulbb(20, fsKhz);
                    c.pitch_LPC_win_length = Smulbb(FindPitchLpcWinMs, fsKhz);
                    c.pitch_contour_iCDF   = c.fs_kHz == 8 ? pitch_contour_NB_iCDF : pitch_contour_iCDF;
                }
                c.PacketSize_ms  = packetSizeMs;
                c.TargetRate_bps = 0; // triggers a new SNR computation
            }

            // the internal sampling rate
            if (c.fs_kHz != fsKhz) {
                // reset part of the state
                std::memset(&enc->sShape, 0, sizeof(enc->sShape));
                std::memset(&c.sNSQ, 0, sizeof(c.sNSQ));
                std::memset(c.prev_NLSFq_Q15, 0, sizeof(c.prev_NLSFq_Q15));
                std::memset(&c.sLP.In_LP_State, 0, sizeof(c.sLP.In_LP_State));
                c.inputBufIx     = 0;
                c.nFramesEncoded = 0;
                c.TargetRate_bps = 0; // triggers a new SNR computation

                // non-zero parameters
                c.prevLag                 = 100;
                c.first_frame_after_reset = 1;
                enc->sShape.LastGainIndex = 10;
                c.sNSQ.lagPrev            = 100;
                c.sNSQ.prev_gain_Q16      = 65536;
                c.prevSignalType          = TypeNoVoiceActivity;

                c.fs_kHz = fsKhz;
                if (c.fs_kHz == 8)
                    c.pitch_contour_iCDF = c.nb_subfr == SilkMaxNbSubfr ? pitch_contour_NB_iCDF : pitch_contour_10_ms_NB_iCDF;
                else
                    c.pitch_contour_iCDF = c.nb_subfr == SilkMaxNbSubfr ? pitch_contour_iCDF : pitch_contour_10_ms_iCDF;
                if (c.fs_kHz == 8 || c.fs_kHz == 12) {
                    c.predictLPCOrder = MinLpcOrder;
                    c.psNLSF_CB       = &NLSF_CB_NB_MB;
                } else {
                    c.predictLPCOrder = SilkMaxLpcOrder;
                    c.psNLSF_CB       = &NLSF_CB_WB;
                }
                c.subfr_length         = SubFrameLengthMs * fsKhz;
                c.frame_length         = Smulbb(c.subfr_length, c.nb_subfr);
                c.ltp_mem_length       = Smulbb(LtpMemLengthMs, fsKhz);
                c.la_pitch             = Smulbb(LaPitchMs, fsKhz);
                c.max_pitch_lag        = Smulbb(18, fsKhz);
                c.pitch_LPC_win_length = c.nb_subfr == SilkMaxNbSubfr ? Smulbb(FindPitchLpcWinMs, fsKhz) : Smulbb(FindPitchLpcWinMs2Sf, fsKhz);
                if (c.fs_kHz == 16)
                    c.pitch_lag_low_bits_iCDF = uniform8_iCDF;
                else if (c.fs_kHz == 12)
                    c.pitch_lag_low_bits_iCDF = uniform6_iCDF;
                else
                    c.pitch_lag_low_bits_iCDF = uniform4_iCDF;
            }
            return ret;
        }

        struct ComplexitySettings {
            std::int32_t pitchEstimationComplexity;
            double       pitchEstimationThreshold;
            std::int32_t pitchEstimationLPCOrder;
            std::int32_t shapingLPCOrder;
            std::int32_t laShapeMs;
            std::int32_t nStatesDelayedDecision;
            std::int32_t useInterpolatedNLSFs;
            std::int32_t nlsfMsvqSurvivors;
            bool         warping;
        };

        std::int32_t SetupComplexity(SilkEncoderState* e, std::int32_t complexity) noexcept
        {
            static constexpr ComplexitySettings settings[7] = {
                {PeMinComplex, 0.8, 6, 12, 3, 1, 0, 2, false},                // < 1
                {PeMidComplex, 0.76, 8, 14, 5, 1, 0, 3, false},               // < 2
                {PeMinComplex, 0.8, 6, 12, 3, 2, 0, 2, false},                // < 3
                {PeMidComplex, 0.76, 8, 14, 5, 2, 0, 4, false},               // < 4
                {PeMidComplex, 0.74, 10, 16, 5, 2, 1, 6, true},               // < 6
                {PeMidComplex, 0.72, 12, 20, 5, 3, 1, 8, true},               // < 8
                {PeMaxComplex, 0.7, 16, 24, 5, MaxDelDecStates, 1, 16, true}, // otherwise
            };
            const std::int32_t        index = complexity < 1 ? 0
                : complexity < 2                             ? 1
                : complexity < 3                             ? 2
                : complexity < 4                             ? 3
                : complexity < 6                             ? 4
                : complexity < 8                             ? 5
                                                             : 6;
            const ComplexitySettings& s     = settings[index];
            e->pitchEstimationComplexity    = s.pitchEstimationComplexity;
            e->pitchEstimationThreshold_Q16 = FixConst(s.pitchEstimationThreshold, 16);
            e->pitchEstimationLPCOrder      = s.pitchEstimationLPCOrder;
            e->shapingLPCOrder              = s.shapingLPCOrder;
            e->la_shape                     = s.laShapeMs * e->fs_kHz;
            e->nStatesDelayedDecision       = s.nStatesDelayedDecision;
            e->useInterpolatedNLSFs         = s.useInterpolatedNLSFs;
            e->NLSF_MSVQ_Survivors          = s.nlsfMsvqSurvivors;
            e->warping_Q16                  = s.warping ? e->fs_kHz * FixConst(WarpingMultiplier, 16) : 0;

            // the pitch estimation LPC order is at most the prediction LPC order
            e->pitchEstimationLPCOrder = MinInt(e->pitchEstimationLPCOrder, e->predictLPCOrder);
            e->shapeWinLength          = SubFrameLengthMs * e->fs_kHz + 2 * e->la_shape;
            e->Complexity              = complexity;
            return 0;
        }

        std::int32_t SetupLbrr(SilkEncoderState* e, const SilkEncControl* c) noexcept
        {
            const std::int32_t lbrrInPreviousPacket = e->LBRR_enabled;
            e->LBRR_enabled                         = c->LBRR_coded;
            if (e->LBRR_enabled != 0) {
                // the gain increase for coding the LBRR excitation
                if (lbrrInPreviousPacket == 0)
                    e->LBRR_GainIncreases = 7; // the previous packet had no LBRR and a higher rate
                else
                    e->LBRR_GainIncreases = MaxInt(7 - Smulwb(e->PacketLoss_perc, FixConst(0.2, 16)), 3);
            }
            return SilkNoError;
        }

        std::int32_t ControlEncoder(SilkEncoderStateFlp* enc, SilkEncControl* c, std::int32_t allowBwSwitch, std::int32_t channelNb,
                                    std::int32_t forceFsKhz) noexcept
        {
            std::int32_t ret                 = 0;
            enc->sCmn.useDTX                 = c->useDTX;
            enc->sCmn.useCBR                 = c->useCBR;
            enc->sCmn.API_fs_Hz              = c->API_sampleRate;
            enc->sCmn.maxInternal_fs_Hz      = c->maxInternalSampleRate;
            enc->sCmn.minInternal_fs_Hz      = c->minInternalSampleRate;
            enc->sCmn.desiredInternal_fs_Hz  = c->desiredInternalSampleRate;
            enc->sCmn.useInBandFEC           = c->useInBandFEC;
            enc->sCmn.nChannelsAPI           = c->nChannelsAPI;
            enc->sCmn.nChannelsInternal      = c->nChannelsInternal;
            enc->sCmn.allow_bandwidth_switch = allowBwSwitch;
            enc->sCmn.channelNb              = channelNb;

            if (enc->sCmn.controlled_since_last_payload != 0 && enc->sCmn.prefillFlag == 0) {
                // a change of the API rate in the middle of a packet
                if (enc->sCmn.API_fs_Hz != enc->sCmn.prev_API_fs_Hz && enc->sCmn.fs_kHz > 0)
                    ret += SetupResamplers(enc, enc->sCmn.fs_kHz);
                return ret;
            }

            // no previously coded frames in the payload buffer from here on
            std::int32_t fsKhz = ControlAudioBandwidth(&enc->sCmn, c);
            if (forceFsKhz != 0)
                fsKhz = forceFsKhz;
            ret += SetupResamplers(enc, fsKhz);
            ret += SetupFs(enc, fsKhz, c->payloadSize_ms);
            ret += SetupComplexity(&enc->sCmn, c->complexity);
            enc->sCmn.PacketLoss_perc = c->packetLossPercentage;
            ret += SetupLbrr(&enc->sCmn, c);
            enc->sCmn.controlled_since_last_payload = 1;
            return ret;
        }

        // the SNR of the residual quantiser; the tables hold SNR / 21 at 400 bps intervals,
        // without the first 10 values (0-4 kb/s, all zero)
        std::int32_t ControlSnr(SilkEncoderState* e, std::int32_t targetRateBps) noexcept
        {
            static constexpr std::uint8_t targetRateNb21[117 - 10]
                = {0,   15,  39,  52,  61,  68,  74,  79,  84,  88,  92,  95,  99,  102, 105, 108, 111, 114, 117, 119, 122, 124, 126, 129, 131, 133, 135,
                   137, 139, 142, 143, 145, 147, 149, 151, 153, 155, 157, 158, 160, 162, 163, 165, 167, 168, 170, 171, 173, 174, 176, 177, 179, 180, 182,
                   183, 185, 186, 187, 189, 190, 192, 193, 194, 196, 197, 199, 200, 201, 203, 204, 205, 207, 208, 209, 211, 212, 213, 215, 216, 217, 219,
                   220, 221, 223, 224, 225, 227, 228, 230, 231, 232, 234, 235, 236, 238, 239, 241, 242, 243, 245, 246, 248, 249, 250, 252, 253, 255};
            static constexpr std::uint8_t targetRateMb21[165 - 10]
                = {0,   0,   28,  43,  52,  59,  65,  70,  74,  78,  81,  85,  87,  90,  93,  95,  98,  100, 102, 105, 107, 109, 111, 113, 115, 116,
                   118, 120, 122, 123, 125, 127, 128, 130, 131, 133, 134, 136, 137, 138, 140, 141, 143, 144, 145, 147, 148, 149, 151, 152, 153, 154,
                   156, 157, 158, 159, 160, 162, 163, 164, 165, 166, 167, 168, 169, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183,
                   184, 185, 186, 187, 188, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 203, 204, 205, 206, 207,
                   208, 209, 210, 211, 212, 213, 214, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 224, 225, 226, 227, 228, 229, 230, 231,
                   232, 233, 234, 235, 236, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255};
            static constexpr std::uint8_t targetRateWb21[201 - 10]
                = {0,   0,   0,   8,   29,  41,  49,  56,  62,  66,  70,  74,  77,  80,  83,  86,  88,  91,  93,  95,  97,  99,  101, 103, 105, 107, 108, 110,
                   112, 113, 115, 116, 118, 119, 121, 122, 123, 125, 126, 127, 129, 130, 131, 132, 134, 135, 136, 137, 138, 140, 141, 142, 143, 144, 145, 146,
                   147, 148, 149, 150, 151, 152, 153, 154, 156, 157, 158, 159, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 171, 172, 173,
                   174, 175, 176, 177, 177, 178, 179, 180, 181, 181, 182, 183, 184, 185, 185, 186, 187, 188, 189, 189, 190, 191, 192, 192, 193, 194, 195, 195,
                   196, 197, 198, 198, 199, 200, 200, 201, 202, 203, 203, 204, 205, 206, 206, 207, 208, 209, 209, 210, 211, 211, 212, 213, 214, 214, 215, 216,
                   216, 217, 218, 219, 219, 220, 221, 221, 222, 223, 224, 224, 225, 226, 226, 227, 228, 229, 229, 230, 231, 232, 232, 233, 234, 234, 235, 236,
                   237, 237, 238, 239, 240, 240, 241, 242, 243, 243, 244, 245, 246, 246, 247, 248, 249, 249, 250, 251, 252, 253, 255};
            e->TargetRate_bps = targetRateBps;
            if (e->nb_subfr == 2)
                targetRateBps -= 2000 + e->fs_kHz / 16;
            std::int32_t        bound    = 0;
            const std::uint8_t* snrTable = nullptr;
            if (e->fs_kHz == 8) {
                bound    = sizeof(targetRateNb21);
                snrTable = targetRateNb21;
            } else if (e->fs_kHz == 12) {
                bound    = sizeof(targetRateMb21);
                snrTable = targetRateMb21;
            } else {
                bound    = sizeof(targetRateWb21);
                snrTable = targetRateWb21;
            }
            std::int32_t id = (targetRateBps + 200) / 400;
            id              = Min(id - 10, bound - 1);
            if (id <= 0)
                e->SNR_dB_Q7 = 0;
            else
                e->SNR_dB_Q7 = snrTable[id] * 21;
            return SilkNoError;
        }

        // a high-pass filter whose cut-off adapts to the pitch lag statistics
        void HpVariableCutoff(SilkEncoderStateFlp stateFxx[]) noexcept
        {
            SilkEncoderState* e = &stateFxx[0].sCmn;
            // the low end of the pitch frequency range
            if (e->prevSignalType == TypeVoiced) {
                // the difference in the log domain
                const std::int32_t pitchFreqHzQ16 = Div32_16(Lshift(e->fs_kHz * 1000, 16), e->prevLag);
                std::int32_t       pitchFreqLogQ7 = Lin2Log(pitchFreqHzQ16) - (16 << 7);

                // adjusted by the quality
                const std::int32_t qualityQ15 = e->input_quality_bands_Q15[0];
                pitchFreqLogQ7                = Smlawb(pitchFreqLogQ7, Smulwb(Lshift(-qualityQ15, 2), qualityQ15),
                                                       pitchFreqLogQ7 - (Lin2Log(FixConst(VariableHpMinCutoffHz, 16)) - (16 << 7)));

                // delta_freq = pitch_freq_log - psEnc->variable_HP_smth1
                std::int32_t deltaFreqQ7 = pitchFreqLogQ7 - (e->variable_HP_smth1_Q15 >> 8);
                if (deltaFreqQ7 < 0)
                    deltaFreqQ7 = deltaFreqQ7 * 3; // less smoothing for decreasing pitch frequency, close to the minimum

                // limited against outliers of the pitch estimation
                deltaFreqQ7 = Limit(deltaFreqQ7, -FixConst(VariableHpMaxDeltaFreq, 7), FixConst(VariableHpMaxDeltaFreq, 7));

                // the smoother
                e->variable_HP_smth1_Q15 = Smlawb(e->variable_HP_smth1_Q15, Smulbb(e->speech_activity_Q8, deltaFreqQ7), FixConst(VariableHpSmthCoef1, 16));

                // the frequency range
                e->variable_HP_smth1_Q15
                    = Limit(e->variable_HP_smth1_Q15, Lshift(Lin2Log(VariableHpMinCutoffHz), 8), Lshift(Lin2Log(VariableHpMaxCutoffHz), 8));
            }
        }

    } // namespace

    std::int32_t GetEncoderSize(std::int32_t channels) noexcept
    {
        std::int32_t size = static_cast<std::int32_t>(sizeof(SilkEncoder));
        // mono skips the second encoder state
        if (channels == 1)
            size -= static_cast<std::int32_t>(sizeof(SilkEncoderStateFlp));
        return size;
    }

    std::int32_t InitEncoder(SilkEncoder* enc, std::int32_t channels, std::int32_t arch, SilkEncControl* encStatus) noexcept
    {
        std::int32_t ret = SilkNoError;
        // reset; mono skips the second encoder state
        std::memset(enc, 0, sizeof(SilkEncoder) - (channels == 1 ? sizeof(SilkEncoderStateFlp) : 0));
        for (std::int32_t n = 0; n < channels; ++n)
            ret += InitEncoderState(&enc->state_Fxx[n], arch);
        enc->nChannelsAPI      = 1;
        enc->nChannelsInternal = 1;
        ret += QueryEncoder(enc, encStatus);
        return ret;
    }

    std::int32_t Encode(SilkEncoder* enc, SilkEncControl* encControl, const float* samplesIn, std::int32_t nSamplesIn, RangeEncoder* rangeEnc,
                        std::int32_t* nBytesOut, std::int32_t prefillFlag, std::int32_t activity) noexcept
    {
        std::int32_t         ret              = 0;
        std::int32_t         tmpPayloadSizeMs = 0;
        std::int32_t         tmpComplexity    = 0;
        SilkEncoderStateFlp* s                = enc->state_Fxx;

        if (encControl->reducedDependency != 0) {
            for (std::int32_t n = 0; n < encControl->nChannelsAPI; ++n)
                s[n].sCmn.first_frame_after_reset = 1;
        }
        for (std::int32_t n = 0; n < encControl->nChannelsAPI; ++n)
            s[n].sCmn.nFramesEncoded = 0;
        // the values of the control structure
        if ((ret = CheckControlInput(encControl)) != 0)
            return ret;

        encControl->switchReady = 0;

        if (encControl->nChannelsInternal > enc->nChannelsInternal) {
            // mono -> stereo: the state of the second channel and the stereo state
            ret += InitEncoderState(&s[1], s[0].sCmn.arch);
            std::memset(enc->sStereo.pred_prev_Q13, 0, sizeof(enc->sStereo.pred_prev_Q13));
            std::memset(enc->sStereo.sSide, 0, sizeof(enc->sStereo.sSide));
            enc->sStereo.mid_side_amp_Q0[0] = 0;
            enc->sStereo.mid_side_amp_Q0[1] = 1;
            enc->sStereo.mid_side_amp_Q0[2] = 0;
            enc->sStereo.mid_side_amp_Q0[3] = 1;
            enc->sStereo.width_prev_Q14     = 0;
            enc->sStereo.smth_width_Q14     = static_cast<std::int16_t>(FixConst(1, 14));
            if (enc->nChannelsAPI == 2) {
                std::memcpy(&s[1].sCmn.resampler_state, &s[0].sCmn.resampler_state, sizeof(SilkResamplerState));
                std::memcpy(&s[1].sCmn.In_HP_State, &s[0].sCmn.In_HP_State, sizeof(s[1].sCmn.In_HP_State));
            }
        }

        const bool transition = encControl->payloadSize_ms != s[0].sCmn.PacketSize_ms || enc->nChannelsInternal != encControl->nChannelsInternal;

        enc->nChannelsAPI      = encControl->nChannelsAPI;
        enc->nChannelsInternal = encControl->nChannelsInternal;

        const std::int32_t nBlocksOf10ms = Div32(100 * nSamplesIn, encControl->API_sampleRate);
        const std::int32_t totBlocks     = nBlocksOf10ms > 1 ? nBlocksOf10ms >> 1 : 1;
        std::int32_t       currBlock     = 0;
        if (prefillFlag != 0) {
            SilkLpState saveLp = {};
            // only 10 ms of input
            if (nBlocksOf10ms != 1)
                return EncInputInvalidNoOfSamples;
            if (prefillFlag == 2) {
                saveLp = s[0].sCmn.sLP;
                // the rate, so the bandwidth switching keeps handling transitions
                saveLp.saved_fs_kHz = s[0].sCmn.fs_kHz;
            }
            // reset the encoder
            for (std::int32_t n = 0; n < encControl->nChannelsInternal; ++n) {
                ret = InitEncoderState(&s[n], s[n].sCmn.arch);
                // the variable LP state restored
                if (prefillFlag == 2)
                    s[n].sCmn.sLP = saveLp;
            }
            tmpPayloadSizeMs           = encControl->payloadSize_ms;
            encControl->payloadSize_ms = 10;
            tmpComplexity              = encControl->complexity;
            encControl->complexity     = 0;
            for (std::int32_t n = 0; n < encControl->nChannelsInternal; ++n) {
                s[n].sCmn.controlled_since_last_payload = 0;
                s[n].sCmn.prefillFlag                   = 1;
            }
        } else {
            // only multiples of 10 ms
            if (nBlocksOf10ms * encControl->API_sampleRate != 100 * nSamplesIn || nSamplesIn < 0)
                return EncInputInvalidNoOfSamples;
            // at most one packet
            if (1000 * nSamplesIn > encControl->payloadSize_ms * encControl->API_sampleRate)
                return EncInputInvalidNoOfSamples;
        }

        for (std::int32_t n = 0; n < encControl->nChannelsInternal; ++n) {
            // the side channel at the rate of the mid
            const std::int32_t forceFsKhz = n == 1 ? s[0].sCmn.fs_kHz : 0;
            if ((ret = ControlEncoder(&s[n], encControl, enc->allowBandwidthSwitch, n, forceFsKhz)) != 0)
                return ret;
            if (s[n].sCmn.first_frame_after_reset != 0 || transition) {
                for (std::int32_t i = 0; i < s[0].sCmn.nFramesPerPacket; ++i)
                    s[n].sCmn.LBRR_flags[i] = 0;
            }
            s[n].sCmn.inDTX = s[n].sCmn.useDTX;
        }

        // input buffering/resampling and encoding
        const std::int32_t nSamplesToBufferMax = 10 * nBlocksOf10ms * s[0].sCmn.fs_kHz;
        std::int16_t       buf[MaxApiSamples60Ms];
        while (true) {
            std::int32_t currNBitsUsedLbrr       = 0;
            std::int32_t nSamplesToBuffer        = s[0].sCmn.frame_length - s[0].sCmn.inputBufIx;
            nSamplesToBuffer                     = Min(nSamplesToBuffer, nSamplesToBufferMax);
            const std::int32_t nSamplesFromInput = Div32_16(nSamplesToBuffer * s[0].sCmn.API_fs_Hz, s[0].sCmn.fs_kHz * 1000);
            // resampled into the buffer
            if (encControl->nChannelsAPI == 2 && encControl->nChannelsInternal == 2) {
                const std::int32_t id = s[0].sCmn.nFramesEncoded;
                for (std::int32_t n = 0; n < nSamplesFromInput; ++n)
                    buf[n] = Res2Int16(samplesIn[2 * n]);
                // both resamplers start from the same state when switching from mono to stereo
                if (enc->nPrevChannelsInternal == 1 && id == 0)
                    std::memcpy(&s[1].sCmn.resampler_state, &s[0].sCmn.resampler_state, sizeof(s[1].sCmn.resampler_state));

                ret += Resample(&s[0].sCmn.resampler_state, &s[0].sCmn.inputBuf[s[0].sCmn.inputBufIx + 2], buf, nSamplesFromInput);
                s[0].sCmn.inputBufIx += nSamplesToBuffer;

                nSamplesToBuffer = s[1].sCmn.frame_length - s[1].sCmn.inputBufIx;
                nSamplesToBuffer = Min(nSamplesToBuffer, 10 * nBlocksOf10ms * s[1].sCmn.fs_kHz);
                for (std::int32_t n = 0; n < nSamplesFromInput; ++n)
                    buf[n] = Res2Int16(samplesIn[2 * n + 1]);
                ret += Resample(&s[1].sCmn.resampler_state, &s[1].sCmn.inputBuf[s[1].sCmn.inputBufIx + 2], buf, nSamplesFromInput);
                s[1].sCmn.inputBufIx += nSamplesToBuffer;
            } else if (encControl->nChannelsAPI == 2 && encControl->nChannelsInternal == 1) {
                // left and right combined before resampling
                for (std::int32_t n = 0; n < nSamplesFromInput; ++n) {
                    const std::int32_t sum = Res2Int16(samplesIn[2 * n] + samplesIn[2 * n + 1]);
                    buf[n]                 = static_cast<std::int16_t>(RshiftRound(sum, 1));
                }
                ret += Resample(&s[0].sCmn.resampler_state, &s[0].sCmn.inputBuf[s[0].sCmn.inputBufIx + 2], buf, nSamplesFromInput);
                // the first mono frame averages the results of the two resampler states
                if (enc->nPrevChannelsInternal == 2 && s[0].sCmn.nFramesEncoded == 0) {
                    ret += Resample(&s[1].sCmn.resampler_state, &s[1].sCmn.inputBuf[s[1].sCmn.inputBufIx + 2], buf, nSamplesFromInput);
                    for (std::int32_t n = 0; n < s[0].sCmn.frame_length; ++n)
                        s[0].sCmn.inputBuf[s[0].sCmn.inputBufIx + n + 2] = static_cast<std::int16_t>(
                            (s[0].sCmn.inputBuf[s[0].sCmn.inputBufIx + n + 2] + s[1].sCmn.inputBuf[s[1].sCmn.inputBufIx + n + 2]) >> 1);
                }
                s[0].sCmn.inputBufIx += nSamplesToBuffer;
            } else {
                for (std::int32_t n = 0; n < nSamplesFromInput; ++n)
                    buf[n] = Res2Int16(samplesIn[n]);
                ret += Resample(&s[0].sCmn.resampler_state, &s[0].sCmn.inputBuf[s[0].sCmn.inputBufIx + 2], buf, nSamplesFromInput);
                s[0].sCmn.inputBufIx += nSamplesToBuffer;
            }

            samplesIn += nSamplesFromInput * encControl->nChannelsAPI;
            nSamplesIn -= nSamplesFromInput;

            // default
            enc->allowBandwidthSwitch = 0;

            // the SILK encoder
            if (s[0].sCmn.inputBufIx < s[0].sCmn.frame_length)
                break;
            // enough data in the input buffer: encode

            // the LBRR data
            if (s[0].sCmn.nFramesEncoded == 0 && prefillFlag == 0) {
                // space at the start of the payload for the VAD and FEC flags
                std::uint8_t iCdf[2] = {0, 0};
                iCdf[0]              = static_cast<std::uint8_t>(256 - (256 >> ((s[0].sCmn.nFramesPerPacket + 1) * encControl->nChannelsInternal)));
                rangeEnc->EncodeIcdf(0, iCdf, 8);
                currNBitsUsedLbrr = rangeEnc->Tell();

                // the LBRR flags of the previous packet
                for (std::int32_t n = 0; n < encControl->nChannelsInternal; ++n) {
                    std::int32_t lbrrSymbol = 0;
                    for (std::int32_t i = 0; i < s[n].sCmn.nFramesPerPacket; ++i)
                        lbrrSymbol |= Lshift(s[n].sCmn.LBRR_flags[i], i);
                    s[n].sCmn.LBRR_flag = static_cast<std::int8_t>(lbrrSymbol > 0 ? 1 : 0);
                    if (lbrrSymbol != 0 && s[n].sCmn.nFramesPerPacket > 1)
                        rangeEnc->EncodeIcdf(lbrrSymbol - 1, LBRR_flags_iCDF_ptr[s[n].sCmn.nFramesPerPacket - 2], 8);
                }

                // the LBRR indices and excitation signals
                for (std::int32_t i = 0; i < s[0].sCmn.nFramesPerPacket; ++i) {
                    for (std::int32_t n = 0; n < encControl->nChannelsInternal; ++n) {
                        if (s[n].sCmn.LBRR_flags[i] != 0) {
                            if (encControl->nChannelsInternal == 2 && n == 0) {
                                StereoEncodePred(rangeEnc, enc->sStereo.predIx[i]);
                                // no mid-only flag for LBRR data when the side channel LBRR flag is set
                                if (s[1].sCmn.LBRR_flags[i] == 0)
                                    StereoEncodeMidOnly(rangeEnc, enc->sStereo.mid_only_flags[i]);
                            }
                            // conditional coding if the previous frame is available
                            const std::int32_t condCoding = i > 0 && s[n].sCmn.LBRR_flags[i - 1] != 0 ? CodeConditionally : CodeIndependently;
                            EncodeIndices(&s[n].sCmn, rangeEnc, i, true, condCoding);
                            EncodePulses(rangeEnc, s[n].sCmn.indices_LBRR[i].signalType, s[n].sCmn.indices_LBRR[i].quantOffsetType, s[n].sCmn.pulses_LBRR[i],
                                         s[n].sCmn.frame_length);
                        }
                    }
                }

                // reset the LBRR flags
                for (std::int32_t n = 0; n < encControl->nChannelsInternal; ++n)
                    std::memset(s[n].sCmn.LBRR_flags, 0, sizeof(s[n].sCmn.LBRR_flags));
                currNBitsUsedLbrr = rangeEnc->Tell() - currNBitsUsedLbrr;
            }

            HpVariableCutoff(s);

            // the total target bits of the packet
            std::int32_t nBits = Div32_16(encControl->bitRate * encControl->payloadSize_ms, 1000);
            // minus the LBRR bits
            if (prefillFlag == 0) {
                // an exponential moving average of the LBRR usage, but without averaging for the
                // first LBRR frame and back to zero at once for the first frame after LBRR
                if (currNBitsUsedLbrr < 10)
                    enc->nBitsUsedLBRR = 0;
                else if (enc->nBitsUsedLBRR < 10)
                    enc->nBitsUsedLBRR = currNBitsUsedLbrr;
                else
                    enc->nBitsUsedLBRR = (enc->nBitsUsedLBRR + currNBitsUsedLbrr) / 2;
                nBits -= enc->nBitsUsedLBRR;
            }
            // per uncoded frame of the packet
            nBits = Div32_16(nBits, s[0].sCmn.nFramesPerPacket);
            // to bits/second
            std::int32_t targetRateBps = encControl->payloadSize_ms == 10 ? Smulbb(nBits, 100) : Smulbb(nBits, 50);
            // minus a fraction of the bits in excess of the target in previous frames and packets
            targetRateBps -= Div32_16(enc->nBitsExceeded * 1000, BitreservoirDecayTimeMs);
            if (prefillFlag == 0 && s[0].sCmn.nFramesEncoded > 0) {
                // actual vs target bits so far in this packet
                const std::int32_t bitsBalance = rangeEnc->Tell() - enc->nBitsUsedLBRR - nBits * s[0].sCmn.nFramesEncoded;
                targetRateBps -= Div32_16(bitsBalance * 1000, BitreservoirDecayTimeMs);
            }
            // never above the input bitrate
            targetRateBps = Limit(targetRateBps, encControl->bitRate, 5000);

            // left/right to mid/side
            std::int32_t msTargetRatesBps[2] = {0, 0};
            if (encControl->nChannelsInternal == 2) {
                const std::int32_t fe = s[0].sCmn.nFramesEncoded;
                StereoLrToMs(&enc->sStereo, &s[0].sCmn.inputBuf[2], &s[1].sCmn.inputBuf[2], enc->sStereo.predIx[fe], &enc->sStereo.mid_only_flags[fe],
                             msTargetRatesBps, targetRateBps, s[0].sCmn.speech_activity_Q8, encControl->toMono != 0, s[0].sCmn.fs_kHz, s[0].sCmn.frame_length);
                if (enc->sStereo.mid_only_flags[fe] == 0) {
                    // the side channel memory reset for the first frame with side coding
                    if (enc->prev_decode_only_middle == 1) {
                        std::memset(&s[1].sShape, 0, sizeof(s[1].sShape));
                        std::memset(&s[1].sCmn.sNSQ, 0, sizeof(s[1].sCmn.sNSQ));
                        std::memset(s[1].sCmn.prev_NLSFq_Q15, 0, sizeof(s[1].sCmn.prev_NLSFq_Q15));
                        std::memset(&s[1].sCmn.sLP.In_LP_State, 0, sizeof(s[1].sCmn.sLP.In_LP_State));
                        s[1].sCmn.prevLag                 = 100;
                        s[1].sCmn.sNSQ.lagPrev            = 100;
                        s[1].sShape.LastGainIndex         = 10;
                        s[1].sCmn.prevSignalType          = TypeNoVoiceActivity;
                        s[1].sCmn.sNSQ.prev_gain_Q16      = 65536;
                        s[1].sCmn.first_frame_after_reset = 1;
                    }
                    EncodeDoVad(&s[1], activity);
                } else {
                    s[1].sCmn.VAD_flags[fe] = 0;
                }
                if (prefillFlag == 0) {
                    StereoEncodePred(rangeEnc, enc->sStereo.predIx[fe]);
                    if (s[1].sCmn.VAD_flags[fe] == 0)
                        StereoEncodeMidOnly(rangeEnc, enc->sStereo.mid_only_flags[fe]);
                }
            } else {
                // buffering
                std::memcpy(s[0].sCmn.inputBuf, enc->sStereo.sMid, 2 * sizeof(std::int16_t));
                std::memcpy(enc->sStereo.sMid, &s[0].sCmn.inputBuf[s[0].sCmn.frame_length], 2 * sizeof(std::int16_t));
            }
            EncodeDoVad(&s[0], activity);

            // encode
            for (std::int32_t n = 0; n < encControl->nChannelsInternal; ++n) {
                // the rate constraints
                std::int32_t maxBits = encControl->maxBits;
                if (totBlocks == 2 && currBlock == 0) {
                    maxBits = maxBits * 3 / 5;
                } else if (totBlocks == 3) {
                    if (currBlock == 0)
                        maxBits = maxBits * 2 / 5;
                    else if (currBlock == 1)
                        maxBits = maxBits * 3 / 4;
                }
                std::int32_t useCbr         = encControl->useCBR != 0 && currBlock == totBlocks - 1 ? 1 : 0;
                std::int32_t channelRateBps = 0;
                if (encControl->nChannelsInternal == 1) {
                    channelRateBps = targetRateBps;
                } else {
                    channelRateBps = msTargetRatesBps[n];
                    if (n == 0 && msTargetRatesBps[1] > 0) {
                        useCbr = 0;
                        // the mid gets up to 1/2 of the max bits of the frame
                        maxBits -= encControl->maxBits / (totBlocks * 2);
                    }
                }

                if (channelRateBps > 0) {
                    ControlSnr(&s[n].sCmn, channelRateBps);
                    // independent coding without a previous frame
                    std::int32_t condCoding = 0;
                    if (s[0].sCmn.nFramesEncoded - n <= 0)
                        condCoding = CodeIndependently;
                    else if (n > 0 && enc->prev_decode_only_middle != 0)
                        condCoding = CodeIndependentlyNoLtpScaling; // a skipped side frame leaves a well-defined LTP state
                    else
                        condCoding = CodeConditionally;
                    ret = EncodeFrame(&s[n], nBytesOut, rangeEnc, condCoding, maxBits, useCbr != 0);
                }
                s[n].sCmn.controlled_since_last_payload = 0;
                s[n].sCmn.inputBufIx                    = 0;
                ++s[n].sCmn.nFramesEncoded;
            }
            enc->prev_decode_only_middle = enc->sStereo.mid_only_flags[s[0].sCmn.nFramesEncoded - 1];

            // the VAD and FEC flags at the start of the bitstream
            if (*nBytesOut > 0 && s[0].sCmn.nFramesEncoded == s[0].sCmn.nFramesPerPacket) {
                std::int32_t flags = 0;
                for (std::int32_t n = 0; n < encControl->nChannelsInternal; ++n) {
                    for (std::int32_t i = 0; i < s[n].sCmn.nFramesPerPacket; ++i) {
                        flags = Lshift(flags, 1);
                        flags |= s[n].sCmn.VAD_flags[i];
                    }
                    flags = Lshift(flags, 1);
                    flags |= s[n].sCmn.LBRR_flag;
                }
                if (prefillFlag == 0)
                    rangeEnc->PatchInitialBits(static_cast<std::uint32_t>(flags),
                                               static_cast<std::uint32_t>((s[0].sCmn.nFramesPerPacket + 1) * encControl->nChannelsInternal));

                // zero bytes if all channels are DTXed
                if (s[0].sCmn.inDTX != 0 && (encControl->nChannelsInternal == 1 || s[1].sCmn.inDTX != 0))
                    *nBytesOut = 0;

                enc->nBitsExceeded += *nBytesOut * 8;
                enc->nBitsExceeded -= Div32_16(encControl->bitRate * encControl->payloadSize_ms, 1000);
                enc->nBitsExceeded = Limit(enc->nBitsExceeded, 0, 10000);

                // whether bandwidth switching is allowed
                const std::int32_t speechActThrForSwitchQ8
                    = Smlawb(FixConst(SpeechActivityDtxThres, 8), FixConst((1 - SpeechActivityDtxThres) / MaxBandwidthSwitchDelayMs, 16 + 8),
                             enc->timeSinceSwitchAllowed_ms);
                if (s[0].sCmn.speech_activity_Q8 < speechActThrForSwitchQ8) {
                    enc->allowBandwidthSwitch      = 1;
                    enc->timeSinceSwitchAllowed_ms = 0;
                } else {
                    enc->allowBandwidthSwitch = 0;
                    enc->timeSinceSwitchAllowed_ms += encControl->payloadSize_ms;
                }
            }

            if (nSamplesIn == 0)
                break;
            ++currBlock;
        }

        enc->nPrevChannelsInternal = encControl->nChannelsInternal;

        encControl->allowBandwidthSwitch      = enc->allowBandwidthSwitch;
        encControl->inWBmodeWithoutVariableLP = s[0].sCmn.fs_kHz == 16 && s[0].sCmn.sLP.mode == 0;
        encControl->internalSampleRate        = Smulbb(s[0].sCmn.fs_kHz, 1000);
        encControl->stereoWidth_Q14           = encControl->toMono != 0 ? 0 : enc->sStereo.smth_width_Q14;
        if (prefillFlag != 0) {
            encControl->payloadSize_ms = tmpPayloadSizeMs;
            encControl->complexity     = tmpComplexity;
            for (std::int32_t n = 0; n < encControl->nChannelsInternal; ++n) {
                s[n].sCmn.controlled_since_last_payload = 0;
                s[n].sCmn.prefillFlag                   = 0;
            }
        }

        encControl->signalType = s[0].sCmn.indices.signalType;
        encControl->offset     = Quantization_Offsets_Q10[s[0].sCmn.indices.signalType >> 1][s[0].sCmn.indices.quantOffsetType];
        return ret;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk
