// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

#include "CeltBands.h"
#include "CeltMode.h"
#include "RangeEncoder.h"
#include "ToneAnalysis.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    // Internal request numbers of libopus (celt.h, opus_private.h)
    constexpr std::int32_t CeltSetPredictionRequest = 10002;
    constexpr std::int32_t CeltSetChannelsRequest   = 10008;
    constexpr std::int32_t CeltSetStartBandRequest  = 10010;
    constexpr std::int32_t CeltSetEndBandRequest    = 10012;
    constexpr std::int32_t CeltSetSignallingRequest = 10016;

    /// <summary>
    /// libopus: celt_preemphasis() (celt/celt_encoder.c) for the static mode: n frames of channel
    /// stride cc into inp, with the filter memory mem.
    /// </summary>
    void CeltPreemphasis(const float* pcmp, float* inp, std::int32_t n, std::int32_t cc, std::int32_t upsample, const float* coef, float* mem,
                         bool clip) noexcept;

    struct CeltSignalFrame;
    struct CeltFrontFrame;
    struct QabInputs;
    struct QabJob;
    struct CeltSpeculation;
    struct SeedPromise;
    class ThetaTrialRunner;

    /// <summary>The inputs of the next frame as the stream encoder expects them (for the speculation).</summary>
    struct NextCeltFrame {
        const float*           pcm;
        const CeltSignalFrame* signal;
        const CeltFrontFrame*  front;
        AnalysisInfo           analysis;
        const NextCeltFrame*   after; // the frame after it, or nullptr
    };

    /// <summary>
    /// Port of the CELT encoder of libopus 1.6.1 (celt/celt_encoder.c) for the static 48 kHz mode,
    /// float, without QEXT, custom modes and resynthesis: the encoder state, its controls and
    /// celt_encode_with_ec with the transient, tone, pitch pre-filter, TF, dynamic allocation,
    /// trim, stereo and VBR decisions. The stages below it are the ports in CeltMdct, CeltBands,
    /// CeltEnergy, CeltRate, CeltVq and CeltPitch.
    /// </summary>
    /// <remarks>
    /// celt_encode_with_ec() is split into stages so that the parts that depend only on the input
    /// can run ahead on other threads (the results are taken only if they were computed from
    /// exactly the encoder's inputs and state, else they are computed in place):
    /// <list type="bullet">
    /// <item>Encode: the frame size, the bit budget and the VBR bounds (up to the constrained VBR
    /// limit), then the stages below, then everything from the surround masking analysis on.</item>
    /// <item>SignalStage (AnalyseSignal on a chain): the sample peak, celt_preemphasis(),
    /// tone_detect(), transient_analysis() without weak transients and the pitch search of
    /// run_prefilter() (pitch_downsample(), pitch_search()).</item>
    /// <item>Front (on a chain, or in Encode): the silence decision, the weak-transient analysis
    /// of hybrid frames, run_prefilter() with remove_doubling() and the comb filters, the
    /// pre-filter symbols (EncodeFrontSymbols), compute_mdcts() (the second MDCT included),
    /// compute_band_energies(), amp2Log2(), the temporal VBR; for frames of 5 ms and more without
    /// surround masking also normalise_bands(), dynalloc_analysis(), tf_analysis() and
    /// spreading_decision().</item>
    /// <item>Encode again: the surround masking, patch_transient_decision() and the transient
    /// flag; the extended front part unless a late transient changed the blocks; the energy error
    /// bias, quant_coarse_energy(), tf_encode(), the spreading symbol, the dynalloc symbols,
    /// stereo_analysis() and the intensity, alloc_trim_analysis(), compute_vbr() and the VBR
    /// state, clt_compute_allocation(), quant_fine_energy(), quant_all_bands() (or the replay of a
    /// speculation, see CeltSpeculation.h), the anti-collapse bit, quant_energy_finalise() and the
    /// energy histories (FinishEnergies).</item>
    /// </list>
    /// </remarks>
    class CeltEncoder
    {
    public:
        static constexpr std::int32_t NbEBands    = 21;
        static constexpr std::int32_t Overlap     = 120;
        static constexpr std::int32_t MaxPeriod   = 1024; // COMBFILTER_MAXPERIOD
        static constexpr std::int32_t MinPeriod   = 15;   // COMBFILTER_MINPERIOD
        static constexpr std::int32_t MaxChannels = 2;

        /// <summary>libopus: celt_encoder_init() (celt/celt_encoder.c); returns an OPUS error code.</summary>
        std::int32_t Init(std::int32_t fs, std::int32_t channels, std::int32_t arch) noexcept;

        /// <summary>libopus: opus_custom_encoder_ctl() (celt/celt_encoder.c) for the requests with an int value, and OPUS_RESET_STATE.</summary>
        std::int32_t Ctl(std::int32_t request, std::int32_t value) noexcept;

        /// <summary>OPUS_RESET_STATE.</summary>
        void Reset() noexcept;

        std::int32_t  PhaseInversionDisabled() const noexcept { return disableInv_; }
        std::uint32_t FinalRange() const noexcept { return rng_; }
        void          SetAnalysis(const AnalysisInfo& info) noexcept { analysis_ = info; }
        void          SetSilkInfo(std::int32_t signalType, std::int32_t offset) noexcept
        {
            silkSignalType_ = signalType;
            silkOffset_     = offset;
        }
        void SetEnergyMask(const float* mask) noexcept { energyMask_ = mask; }
        void SetLfe(std::int32_t value) noexcept { lfe_ = value; }

        std::int32_t ModeOverlap() const noexcept { return mode_.overlap; }
        const float* ModeWindow() const noexcept { return mode_.window; }

        /// <summary>
        /// libopus: celt_encode_with_ec() (celt/celt_encoder.c): encodes frameSize interleaved
        /// frames into compressed, or into ec (then compressed is not used). Returns the bytes or an
        /// OPUS error code. signal, if given, is the signal stage of this frame computed elsewhere
        /// (AnalyseSignal); it is used only if its inputs (the input, the configuration and the
        /// state it started from) are exactly those of this call, else it is computed here. front
        /// likewise for the front part (Front), which also needs the signal stage used.
        /// </summary>
        std::int32_t Encode(const float* pcm, std::int32_t frameSize, std::uint8_t* compressed, std::int32_t nbCompressedBytes, RangeEncoder* ec,
                            const CeltSignalFrame* signal = nullptr, const CeltFrontFrame* front = nullptr) noexcept;

        // --- the front part: the pre-filter decision, the MDCTs and the band energies ---------

        /// <summary>The state of the front part (the pre-filter, the temporal VBR and the spreading decision).</summary>
        struct FrontState {
            float        prefilterMem[MaxChannels * MaxPeriod];
            float        inMem[MaxChannels * Overlap];
            std::int32_t prefilterPeriod;
            float        prefilterGain;
            std::int32_t prefilterTapset;
            float        specAvg;
            std::int32_t tonalAverage;
            std::int32_t hfAverage;
            std::int32_t spreadDecision;
            std::int32_t tapsetDecision;
        };

        /// <summary>What the front part depends on besides the signal stage and its state.</summary>
        struct FrontParams {
            std::int32_t frameSize;
            std::int32_t lm;
            std::int32_t tell; // at the start of the frame (1: an empty range coder)
            std::int32_t totalBits;
            std::int32_t nbCompressedBytes;
            std::int32_t nbAvailableBytes;
            std::int32_t nbFilledBytes;
            std::int32_t effectiveBytes;
            std::int32_t vbrRate;
            std::int32_t start;
            std::int32_t end;
            std::int32_t effEnd;
            std::int32_t streamChannels;
            std::int32_t lsbDepth;
            std::int32_t complexity;
            std::int32_t lfe;
            std::int32_t disablePf;
            std::int32_t lossRate;
            std::int32_t vbr;
            std::int32_t constrainedVbr;
            bool         masking; // surround masking (from the coded bands of the frame before)
            std::int32_t silkSignalType;
            std::int32_t analysisValid;
            float        analysisTonality;
            float        analysisMaxPitchRatio;
            std::uint8_t analysisLeakBoost[LeakBands];
            bool         operator==(const FrontParams&) const noexcept = default;
        };

        FrontState GetFrontState() const noexcept;
        /// <summary>GetFrontState() equals state bit for bit (without prefilterMem if that is known to be equal).</summary>
        bool         FrontStateIs(const FrontState& state, bool prefilterMemKnown) const noexcept;
        std::int32_t TapsetDecision() const noexcept { return tapsetDecision_; }

        /// <summary>True if the last Encode took the front part from its caller.</summary>
        bool LastFrontUsed() const noexcept { return lastFrontUsed_; }

        /// <summary>
        /// Speculates on the next frame while this one is quantised (nullptr: no speculation): see
        /// CeltSpeculation. The result is the same.
        /// </summary>
        void SetSpeculation(CeltSpeculation* spec) noexcept { spec_ = spec; }
        /// <summary>The expected inputs of the next frame, for the next Encode only (nullptr: none, no speculation).</summary>
        void SetNextFrame(const NextCeltFrame* next) noexcept { next_ = next; }

        /// <summary>Runs the theta RDO trials of stereo bands in parallel (nullptr: not).</summary>
        void SetThetaTrialRunner(ThetaTrialRunner* runner) noexcept { thetaRunner_ = runner; }
        /// <summary>The front parameters of the last Encode.</summary>
        const FrontParams& LastFrontParams() const noexcept { return lastFrontParams_; }

        /// <summary>
        /// The front part of a frame from state (the pre-filter decision, the comb filter, the
        /// MDCTs, the band energies, the temporal VBR; for frames of 5 ms and more without surround
        /// masking also the normalisation, the dynamic allocation analysis, the TF analysis and the
        /// spreading decision, assuming no late transient decision and room for the spreading
        /// symbol); encodes its symbols into enc (an empty coder elsewhere: EncodeFrontSymbols
        /// repeats them) and advances state.
        /// </summary>
        void Front(const FrontParams& params, const CeltSignalFrame& signal, FrontState& state, RangeEncoder* enc, CeltFrontFrame& frame) const noexcept;

        // --- the signal stage: what depends only on the input of the frame -------------------

        /// <summary>The configuration the signal stage depends on.</summary>
        struct SignalConfig {
            std::int32_t channels;       // the coded channels (CC)
            std::int32_t streamChannels; // C (the sample peak reads C channels' worth of samples)
            std::int32_t complexity;
            std::int32_t clip;
            std::int32_t lfe;
            std::int32_t upsample;
            std::int32_t arch;
            bool         operator==(const SignalConfig&) const noexcept = default;
        };

        /// <summary>The state the signal stage carries from frame to frame (signal only).</summary>
        struct SignalState {
            float preemphMem[MaxChannels];
            float overlapMax;
            float prefilterMem[MaxChannels * MaxPeriod];
        };

        SignalConfig GetSignalConfig() const noexcept;
        SignalState  GetSignalState() const noexcept;

        /// <summary>
        /// The signal stage of one frame from state: sample peak, pre-emphasis, tone and transient
        /// detection, pitch search; advances state to the next frame (as Encode does).
        /// </summary>
        void AnalyseSignal(const SignalConfig& config, const float* pcm, std::int32_t frameSize, SignalState& state, CeltSignalFrame& frame) const noexcept;

    private:
        // the signal stage on the given state (the prefilter memory is not advanced): the first
        // part of celt_encode_with_ec, from the sample peak to the pitch search of run_prefilter
        void SignalStage(const SignalConfig& config, const float* pcm, std::int32_t n, float* preemphMem, float& overlapMax, const float* prefilterMem,
                         CeltSignalFrame& frame) const noexcept;

        // libopus: run_prefilter() (celt/celt_encoder.c), with the pitch search from the signal stage
        std::int32_t RunPrefilter(FrontState& fs, float* in, std::int32_t cc, std::int32_t n, std::int32_t prefilterTapset, std::int32_t& pitch, float& gain,
                                  std::int32_t& qgain, bool enabled, std::int32_t complexity, float tfEstimate, std::int32_t nbAvailableBytes, float toneFreq,
                                  float toneishness, const CeltSignalFrame& signal, std::int32_t lossRate, std::int32_t analysisValid,
                                  float analysisMaxPitchRatio) const noexcept;
        // the symbols of the front part (the silence flag and the pre-filter parameters)
        static void EncodeFrontSymbols(const FrontParams& params, const CeltFrontFrame& frame, RangeEncoder* enc) noexcept;
        void        SetFrontState(const FrontState& state) noexcept;

        // the energies at the end of a frame: the finalisation with bitsLeft bits, the energy
        // error and the energy histories (what the next frame takes from this one's leftover bits)
        struct EnergyTail {
            std::int32_t start;
            std::int32_t end;
            std::int32_t c;
            std::int32_t cc;
            std::int32_t fineQuant[NbEBands];
            std::int32_t finePriority[NbEBands];
            bool         silence;
            bool         isTransient;
        };
        void FinishEnergies(const EnergyTail& t, float* error, std::int32_t bitsLeft, RangeEncoder* enc) noexcept;

        // --- the speculation (CeltSpeculation.cpp; the protocol is described in CeltSpeculation.h)

        // the speculation of the next frame: its state from this one before the residual
        // quantisation, with a guessed number of bits left for the finalisation
        void BuildSpeculation(const EnergyTail& t, const float* error, bool transientGotDisabled, std::int32_t frameSize, std::int32_t nbCompressedBytes,
                              const RangeEncoder& entryEnc, std::int64_t frame) noexcept;
        // a speculative run: hands the residual quantisation over as the job and requests the
        // speculation of the frame after (the cascade) if there is one to request
        void PostQabJob(const QabInputs& in, const RangeEncoder& enc, const EnergyTail& t, const float* error, bool transientGotDisabled,
                        std::int32_t frameSize, std::int32_t nbCompressedBytes, const RangeEncoder& entryEnc) noexcept;
        // the speculations for this frame (this encoder's and the cascaded ones): replays a
        // matching result into enc; true if it did (then the residual quantisation is done)
        bool TakeSpeculation(std::int64_t frame, const QabInputs& in, RangeEncoder& enc) noexcept;
        // the speculation of the next frame, if it can be requested: the pending ones are
        // cancelled first; true if it was requested (then the seed is to be published)
        bool SpeculateNext(const EnergyTail& t, const float* error, bool transientGotDisabled, std::int32_t frameSize, std::int32_t nbCompressedBytes,
                           const RangeEncoder& entryEnc, std::int64_t frame) noexcept;
        // publishes the seed of the speculation of the next frame: this frame's final range
        void        PublishSeed(std::uint32_t seed) noexcept;
        friend void RunSpeculation(CeltSpeculation& spec) noexcept;

        CeltSpeculation*     spec_      = nullptr; // the speculation of this encoder (nullptr: none)
        QabJob*              specOut_   = nullptr; // a speculative run: its residual quantisation as a job
        const NextCeltFrame* next_      = nullptr;
        std::int64_t         frames_    = 0;  // the main frames so far
        std::int64_t         specFrame_ = -1; // a speculative run: its frame

        CeltMode::View    mode_            = {};
        std::int32_t      channels_        = 0;
        std::int32_t      streamChannels_  = 0;
        std::int32_t      forceIntra_      = 0;
        std::int32_t      clip_            = 0;
        std::int32_t      disablePf_       = 0;
        std::int32_t      complexity_      = 0;
        std::int32_t      upsample_        = 0;
        std::int32_t      start_           = 0;
        std::int32_t      end_             = 0;
        std::int32_t      bitrate_         = 0;
        std::int32_t      vbr_             = 0;
        std::int32_t      signalling_      = 0;
        std::int32_t      constrainedVbr_  = 0;
        std::int32_t      lossRate_        = 0;
        std::int32_t      lsbDepth_        = 0;
        std::int32_t      lfe_             = 0;
        std::int32_t      disableInv_      = 0;
        std::int32_t      arch_            = 0;
        bool              lastFrontUsed_   = false;
        ThetaTrialRunner* thetaRunner_     = nullptr;
        FrontParams       lastFrontParams_ = {};

        // ENCODER_RESET_START: everything below is cleared by OPUS_RESET_STATE
        std::uint32_t rng_;
        std::int32_t  spreadDecision_;
        float         delayedIntra_;
        std::int32_t  tonalAverage_;
        std::int32_t  lastCodedBands_;
        std::int32_t  hfAverage_;
        std::int32_t  tapsetDecision_;
        std::int32_t  prefilterPeriod_;
        float         prefilterGain_;
        std::int32_t  prefilterTapset_;
        std::int32_t  consecTransient_;
        AnalysisInfo  analysis_;
        std::int32_t  silkSignalType_;
        std::int32_t  silkOffset_;
        float         preemphMemE_[2];
        float         preemphMemD_[2];
        std::int32_t  vbrReservoir_;
        std::int32_t  vbrDrift_;
        std::int32_t  vbrOffset_;
        std::int32_t  vbrCount_;
        float         overlapMax_;
        float         stereoSaving_;
        std::int32_t  intensity_;
        const float*  energyMask_;
        float         specAvg_;
        float         inMem_[MaxChannels * Overlap];
        float         prefilterMem_[MaxChannels * MaxPeriod];
        float         oldBandE_[MaxChannels * NbEBands];
        float         oldLogE_[MaxChannels * NbEBands];
        float         oldLogE2_[MaxChannels * NbEBands];
        float         energyError_[MaxChannels * NbEBands];
    };

    /// <summary>
    /// The signal stage of one CELT frame (CeltEncoder::AnalyseSignal): everything up to the
    /// pre-filter decision that depends only on the input and the signal state, together with
    /// what it was computed from.
    /// </summary>
    struct CeltSignalFrame {
        // the inputs
        std::int32_t              frameSize; // 0: no frame
        CeltEncoder::SignalConfig config;
        float                     preemphMemBefore[CeltEncoder::MaxChannels];
        float                     overlapMaxBefore;
        float                     prefilterMemBefore[CeltEncoder::MaxChannels * CeltEncoder::MaxPeriod];
        float                     pcm[CeltEncoder::MaxChannels * 960];
        // the results
        float        sampleMax;
        float        preemphMemAfter[CeltEncoder::MaxChannels];
        float        overlapMaxAfter;
        float        in[CeltEncoder::MaxChannels * (960 + CeltEncoder::Overlap)]; // pre-emphasised, with the overlap of the pre-filter memory
        float        toneFreq;
        float        toneishness; // before the limit by the transient estimate
        bool         transient;   // transient analysis done (complexity >= 1, not LFE; no weak transients)
        bool         isTransient;
        bool         weakTransient;
        float        tfEstimate;
        std::int32_t tfChan;
        bool         pitch; // pitch search done (complexity >= 5)
        std::int32_t pitchIndex;
        float        pitchBuf[(CeltEncoder::MaxPeriod + 960) >> 1];
    };

    /// <summary>
    /// The front part of one CELT frame (CeltEncoder::Front): its results and, for the check in
    /// Encode, the parameters and the state it was computed from.
    /// </summary>
    struct CeltFrontFrame {
        bool valid; // computed (a chain may have no parameters yet)
        // before.prefilterMem equals the prefilterMemBefore of the signal stage it was computed
        // from (checked by the producer; the encoder checks the signal stage against its state)
        bool                     prefilterMemAsSignal;
        std::int32_t             epoch; // a chain's resynchronisation count when it was computed
        CeltEncoder::FrontParams params;
        CeltEncoder::FrontState  before;
        // the results
        bool         silence;
        std::int32_t tell;
        std::int32_t totalBits;
        std::int32_t nbCompressedBytes;
        std::int32_t nbAvailableBytes;
        std::int32_t effectiveBytes;
        float        tfEstimate;
        float        toneishness;
        float        toneFreq;
        bool         isTransient;
        bool         weakTransient;
        bool         transientGotDisabled;
        bool         secondMdct;
        bool         pitchChange;
        std::int32_t tfChan;
        std::int32_t shortBlocks;
        std::int32_t pitchIndex;
        std::int32_t qg;
        std::int32_t prefilterTapset;
        std::int32_t pfOn;
        float        gain1;
        float        temporalVbr;
        // the extended part (frames of 5 ms and more, no surround masking)
        bool                    extended;
        std::int32_t            offsets[CeltEncoder::NbEBands];
        std::int32_t            tfRes[CeltEncoder::NbEBands];
        std::int32_t            tfSelect;
        std::int32_t            totBoost;
        float                   maxDepth;
        std::int32_t            spreadDecision;
        std::int32_t            spreadBefore[4];                     // tonalAverage, hfAverage, spreadDecision, tapsetDecision before the decision
        std::int32_t            stereoItheta[CeltEncoder::NbEBands]; // BandStereoItheta of every band (stereo)
        float                   x[CeltEncoder::MaxChannels * 960];
        float                   in[CeltEncoder::MaxChannels * (960 + CeltEncoder::Overlap)];
        float                   freq[CeltEncoder::MaxChannels * 960];
        float                   bandE[CeltEncoder::NbEBands * CeltEncoder::MaxChannels];
        float                   bandLogE[CeltEncoder::NbEBands * CeltEncoder::MaxChannels];
        float                   bandLogE2[CeltEncoder::NbEBands * CeltEncoder::MaxChannels];
        CeltEncoder::FrontState after;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus

// The speculation (QabInputs, QabJob, CeltSpeculation, SpeculationRunner, RunSpeculation) is
// declared in CeltSpeculation.h; included here for the callers that include only this header.
#include "CeltSpeculation.h"
