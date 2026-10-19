// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstddef>
#include <cstdint>

#include "CeltEncoder.h"
#include "HeapArray.h"
#include "SilkState.h"
#include "ToneAnalysis.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    // Internal request numbers of libopus (opus_private.h)
    constexpr std::int32_t OpusSetForceModeRequest  = 11002;
    constexpr std::int32_t OpusSetVoiceRatioRequest = 11018;
    constexpr std::int32_t OpusGetVoiceRatioRequest = 11019;

    constexpr std::int32_t ModeSilkOnly = 1000;
    constexpr std::int32_t ModeHybrid   = 1001;
    constexpr std::int32_t ModeCeltOnly = 1002;

    /// <summary>The state of compute_stereo_width.</summary>
    struct StereoWidthState {
        float xx            = 0;
        float xy            = 0;
        float yy            = 0;
        float smoothedWidth = 0;
        float maxFollower   = 0;
    };

    /// <summary>compute_stereo_width: the stereo width of a frame of 2 interleaved channels at fs, advancing mem.</summary>
    float StereoWidth(const float* pcm, std::int32_t frameSize, std::int32_t fs, StereoWidthState& mem) noexcept;

    /// <summary>
    /// The input stage of one frame of a stream (opus_encode_native up to the mode decisions, and
    /// the DC rejection of the frame): it depends on the input alone, so a chain computes it; the
    /// encoder takes it for the same input (and the same states before).
    /// </summary>
    struct StreamInputFrame {
        std::int32_t     frameSize;
        std::int32_t     lsbDepth;
        bool             silence; // is_digital_silence
        float            energy;  // compute_frame_energy
        bool             width;   // the stereo width computed (2 channels)
        float            stereoWidth;
        StereoWidthState widthBefore;
        StereoWidthState widthAfter;
        float            hpMemBefore[4];
        float            hpMemAfter[4]; // after the DC rejection and the check for NaNs
        float            pcm[2 * 960];  // the input
        float            dcRejected[2 * 960];
    };

    /// <summary>What a chain needs to compute the CELT front parts again from frame `call` on.</summary>
    struct FrontSync {
        CeltEncoder::FrontState  state;   // the CELT front state after the frame before
        CeltEncoder::FrontParams params;  // its parameters
        std::int32_t             readPos; // the read position of the tonality analysis after the frame before
        std::int32_t             readSubframe;
    };

    /// <summary>
    /// The tonality analysis of the streams, computed on a chain of its own ahead of the encoder:
    /// the analysis state depends only on the input, the reads of the encoder only on their own
    /// read position (ToneAnalysis::GetInfoFrom).
    /// </summary>
    class ToneAnalysisSource
    {
    public:
        /// <summary>The chain's analysis of stream s right after encoder call `call` (0, 1, ...); waits until it is there.</summary>
        virtual const ToneAnalysis& Wait(std::int32_t stream, std::int64_t call, ToneAnalysis::View& view) noexcept = 0;

        /// <summary>
        /// The CELT signal stage of stream s for encoder call `call`, or nullptr; waits until it is
        /// there. CeltEncoder::Encode uses it only if it fits exactly.
        /// </summary>
        virtual const CeltSignalFrame* WaitSignal(std::int32_t stream, std::int64_t call) noexcept = 0;

        /// <summary>
        /// Without waiting: the view, the CELT signal stage and the front part of stream s for
        /// encoder call `call`, if they are there and final (false otherwise).
        /// </summary>
        virtual bool PeekCall(std::int32_t stream, std::int64_t call, ToneAnalysis::View& view, const CeltSignalFrame*& signal,
                              const CeltFrontFrame*& front) noexcept = 0;

        /// <summary>The input stage of stream s for encoder call `call`, or nullptr; waits until it is there.</summary>
        virtual const StreamInputFrame* WaitInput(std::int32_t stream, std::int64_t call) noexcept = 0;

        /// <summary>The CELT front part of stream s for encoder call `call` (computed after the last Resync), or nullptr; waits.</summary>
        virtual const CeltFrontFrame* WaitFront(std::int32_t stream, std::int64_t call) noexcept = 0;

        /// <summary>The front parts from call on are to be computed again from sync (the encoder's real state).</summary>
        virtual void Resync(std::int32_t stream, std::int64_t call, const FrontSync& sync) noexcept = 0;

    protected:
        ~ToneAnalysisSource() = default;
    };

    class OpusStreamEncoder;

    /// <summary>
    /// The CELT input of the frames as EncodeFrameNative builds it before the mode-dependent fades
    /// (the delay buffer and the DC rejection), from the stream's input alone: for the CELT
    /// signal stage on a chain.
    /// </summary>
    class CeltInputChain
    {
    public:
        /// <summary>From the encoder before its first frame; false if the input is filtered otherwise (VoIP) or not coded by CELT.</summary>
        bool Init(const OpusStreamEncoder& encoder) noexcept;

        /// <summary>
        /// The CELT input of the next frame from frameSize interleaved frames of the stream's
        /// channels; valid until the next call. With stage, also the input stage of the frame.
        /// </summary>
        const float* Next(const float* pcm, std::int32_t frameSize, StreamInputFrame* stage = nullptr) noexcept;

    private:
        std::int32_t     lsbDepth_ = 0;
        StereoWidthState widthMem_;
        std::int32_t     channels_                = 0;
        std::int32_t     fs_                      = 0;
        std::int32_t     encoderBuffer_           = 0;
        std::int32_t     totalBuffer_             = 0;
        float            hpMem_[4]                = {};
        float            delayBuffer_[480 * 2]    = {};
        float            pcmBuf_[(480 + 960) * 2] = {};
    };

    /// <summary>
    /// Port of libopus' OpusEncoder (src/opus_encoder.c) for the float API without DRED and QEXT:
    /// mode, channel and bandwidth decisions, the SILK/CELT split, redundancy frames, multi-frame
    /// packets and the encoder controls.
    /// </summary>
    class OpusStreamEncoder
    {
        friend class CeltInputChain;

    public:
        static constexpr std::int32_t MaxEncoderBuffer = 480;
        // resynchronisations of a front chain in a row without a hit before it is given up
        static constexpr std::int32_t MaxFrontMisses = 16;

        OpusStreamEncoder() noexcept                           = default;
        OpusStreamEncoder(const OpusStreamEncoder&)            = delete;
        OpusStreamEncoder& operator=(const OpusStreamEncoder&) = delete;

        /// <summary>opus_encoder_init; returns an OPUS error code.</summary>
        std::int32_t Init(std::int32_t fs, std::int32_t channels, std::int32_t application) noexcept;

        /// <summary>opus_encoder_ctl for requests with an int value (SET and OPUS_RESET_STATE).</summary>
        std::int32_t Ctl(std::int32_t request, std::int32_t value) noexcept;

        /// <summary>opus_encoder_ctl for requests with an int result (GET).</summary>
        std::int32_t Get(std::int32_t request, std::int32_t& value) noexcept;

        /// <summary>OPUS_GET_FINAL_RANGE.</summary>
        std::uint32_t FinalRange() const noexcept { return rangeFinal_; }

        /// <summary>OPUS_SET_ENERGY_MASK (surround masking; not used for mono/stereo streams).</summary>
        std::int32_t SetEnergyMask(const float* mask) noexcept;

        /// <summary>
        /// opus_encode_native for float input: pcm holds frameSize interleaved frames of this
        /// stream; analysisPcm the multistream input for the tonality analysis.
        /// </summary>
        std::int32_t EncodeNative(const float* pcm, std::int32_t frameSize, std::uint8_t* data, std::int32_t outDataBytes, std::int32_t lsbDepth,
                                  const float* analysisPcm, std::int32_t analysisSize, std::int32_t c1, std::int32_t c2,
                                  std::int32_t analysisChannels) noexcept;

        // --- the tonality analysis on a chain of its own ---------------------------------------

        /// <summary>True if EncodeNative runs the tonality analysis (complexity >= 7, 16 to 48 kHz, not restricted SILK).</summary>
        bool AnalysisEnabled() const noexcept;

        /// <summary>The LSB depth the analysis gets for an input of lsbDepth bits.</summary>
        std::int32_t AnalysisLsbDepth(std::int32_t lsbDepth) const noexcept { return lsbDepth < lsbDepth_ ? lsbDepth : lsbDepth_; }

        /// <summary>The analysis state as initialised (the starting point of a chain).</summary>
        const ToneAnalysis& Analysis() const noexcept { return analysis_; }

        /// <summary>The CELT encoder (for its signal stage on a chain).</summary>
        const CeltEncoder& Celt() const noexcept { return celt_; }
        /// <summary>CeltEncoder::SetSpeculation.</summary>
        void SetSpeculation(CeltSpeculation* spec) noexcept { celt_.SetSpeculation(spec); }
        /// <summary>CeltEncoder::SetThetaTrialRunner.</summary>
        void SetThetaTrialRunner(ThetaTrialRunner* runner) noexcept { celt_.SetThetaTrialRunner(runner); }

        /// <summary>
        /// From now on, EncodeNative takes the analysis of every call from source (stream index
        /// stream) instead of running it; the calls are counted from 0. nullptr runs it again.
        /// </summary>
        void SetToneSource(ToneAnalysisSource* source, std::int32_t stream) noexcept
        {
            toneSource_ = source;
            toneStream_ = stream;
            callIndex_  = 0;
        }

    private:
        std::int32_t EncodeFrameNative(const float* pcm, std::int32_t frameSize, std::uint8_t* data, std::int32_t origMaxDataBytes,
                                       const AnalysisInfo* analysisInfo, bool isSilence, bool redundancy, bool celtToSilk, std::int32_t prefill,
                                       std::int32_t equivRate, bool toCelt) noexcept;
        std::int32_t UserBitrateToBitrate(std::int32_t frameSize, std::int32_t maxDataBytes) const noexcept;
        float        ComputeStereoWidth(const float* pcm, std::int32_t frameSize) noexcept;
        float        ComputeFrameEnergy(const float* pcm, std::int32_t frameSize) const noexcept;
        // ComputeFrameEnergy, from the chain's input stage for the frame of the current call
        float FrameEnergy(const float* pcm, std::int32_t frameSize) const noexcept;
        void  ResetState() noexcept;

        SilkEncoder* Silk() noexcept { return reinterpret_cast<SilkEncoder*>(silk_.Data()); }

        CeltEncoder             celt_;
        HeapArray<std::byte>    silk_;
        HeapArray<float>        frameInput_; // the delayed and DC-rejected input of a frame (EncodeFrameNative)
        ToneAnalysis            analysis_;
        ToneAnalysisSource*     toneSource_    = nullptr;
        std::int32_t            toneStream_    = 0;
        std::int64_t            callIndex_     = 0;
        std::int64_t            currentCall_   = 0;       // the call EncodeNative is in
        const ToneAnalysis*     toneChain_     = nullptr; // the source's analysis of the current call
        ToneAnalysis::View      toneView_      = {};
        const CeltSignalFrame*  currentSignal_ = nullptr; // the source's CELT signal stage of the current frame
        const CeltFrontFrame*   currentFront_  = nullptr; // the source's CELT front part of the current frame
        const StreamInputFrame* currentInput_  = nullptr; // the source's input stage of the current call (for currentPcm_)
        const float*            currentPcm_    = nullptr;
        bool                    celtCoded_     = false; // the main CELT frame of the current call was coded
        std::int32_t            frontMisses_   = 0;     // resynchronisations in a row without a hit
        SilkEncControl          silkMode_      = {};

        std::int32_t application_       = 0;
        std::int32_t channels_          = 0;
        std::int32_t delayCompensation_ = 0;
        std::int32_t forceChannels_     = 0;
        std::int32_t signalType_        = 0;
        std::int32_t userBandwidth_     = 0;
        std::int32_t maxBandwidth_      = 0;
        std::int32_t userForcedMode_    = 0;
        std::int32_t voiceRatio_        = 0;
        std::int32_t fs_                = 0;
        std::int32_t useVbr_            = 0;
        std::int32_t vbrConstraint_     = 0;
        std::int32_t variableDuration_  = 0;
        std::int32_t bitrateBps_        = 0;
        std::int32_t userBitrateBps_    = 0;
        std::int32_t lsbDepth_          = 0;
        std::int32_t encoderBuffer_     = 0;
        std::int32_t lfe_               = 0;
        std::int32_t arch_              = 0;
        std::int32_t useDtx_            = 0;
        std::int32_t fecConfig_         = 0;

        // OPUS_ENCODER_RESET_START: cleared by OPUS_RESET_STATE
        std::int32_t     streamChannels_       = 0;
        std::int16_t     hybridStereoWidthQ14_ = 0;
        std::int32_t     variableHpSmth2Q15_   = 0;
        float            prevHbGain_           = 0;
        float            hpMem_[4]             = {};
        std::int32_t     mode_                 = 0;
        std::int32_t     prevMode_             = 0;
        std::int32_t     prevChannels_         = 0;
        std::int32_t     prevFramesize_        = 0;
        std::int32_t     bandwidth_            = 0;
        std::int32_t     autoBandwidth_        = 0;
        std::int32_t     silkBwSwitch_         = 0;
        std::int32_t     first_                = 0;
        const float*     energyMasking_        = nullptr;
        StereoWidthState widthMem_;
        std::int32_t     detectedBandwidth_                 = 0;
        std::int32_t     nbNoActivityMsQ1_                  = 0;
        float            peakSignalEnergy_                  = 0;
        std::int32_t     nonfinalFrame_                     = 0;
        std::uint32_t    rangeFinal_                        = 0;
        float            delayBuffer_[MaxEncoderBuffer * 2] = {};
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
