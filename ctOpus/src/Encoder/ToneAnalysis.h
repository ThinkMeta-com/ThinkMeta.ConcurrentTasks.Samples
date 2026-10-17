// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    constexpr std::int32_t LeakBands = 19;

    /// <summary>AnalysisInfo of celt.h: the analysis result of one frame, handed to CELT.</summary>
    struct AnalysisInfo {
        std::int32_t valid;
        float        tonality;
        float        tonalitySlope;
        float        noisiness;
        float        activity;
        float        musicProb;
        float        musicProbMin;
        float        musicProbMax;
        std::int32_t bandwidth;
        float        activityProbability;
        float        maxPitchRatio;
        std::uint8_t leakBoost[LeakBands];
    };

    /// <summary>
    /// Port of the tonality and music/speech analysis of libopus 1.6.1 (src/analysis.c, src/mlp.c):
    /// a 480-point FFT every 20 ms of the downmixed input at 24 kHz, band features, and a small
    /// GRU network. The encoder runs it ahead of the frames it codes (run_analysis) and reads the
    /// result per frame (tonality_get_info).
    /// </summary>
    class ToneAnalysis
    {
    public:
        /// <summary>The finished analysis entries a chain keeps for readers that lag behind (a power of 2).</summary>
        static constexpr std::int32_t HistorySize = 512;

        /// <summary>
        /// What tonality_get_info reads besides the entries: the write position and the frame
        /// count right after the analysis of one encoder call, and the number of entries written
        /// so far (the absolute index of the entry at the write position).
        /// </summary>
        struct View {
            std::int32_t writePos;
            std::int32_t count;
            std::int64_t entries;
        };

        /// <summary>tonality_analysis_init.</summary>
        void Init(std::int32_t fs) noexcept;
        /// <summary>tonality_analysis_reset: clears everything but the sample rate.</summary>
        void Reset() noexcept;

        bool         Initialized() const noexcept { return initialized_ != 0; }
        std::int32_t ReadPos() const noexcept { return readPos_; }
        std::int32_t ReadSubframe() const noexcept { return readSubframe_; }
        void         SetRead(std::int32_t readPos, std::int32_t readSubframe) noexcept
        {
            readPos_      = readPos;
            readSubframe_ = readSubframe;
        }

        /// <summary>
        /// run_analysis with downmix_float: analyses the interleaved float pcm (C channels, c1/c2
        /// select the downmix) up to analysisFrameSize frames, then reads the info of the next frame.
        /// </summary>
        void Run(const float* pcm, std::int32_t analysisFrameSize, std::int32_t frameSize, std::int32_t c1, std::int32_t c2, std::int32_t channels,
                 std::int32_t lsbDepth, AnalysisInfo& info) noexcept;

        /// <summary>tonality_get_info: the info for the next len frames.</summary>
        void GetInfo(AnalysisInfo& info, std::int32_t len) noexcept;

        // --- the analysis on its own chain, ahead of the encoder ------------------------------

        /// <summary>
        /// The analysis part of Run (without the read of the info): the analysis state then
        /// depends only on the input, not on the reads.
        /// </summary>
        void Advance(const float* pcm, std::int32_t analysisFrameSize, std::int32_t frameSize, std::int32_t c1, std::int32_t c2, std::int32_t channels,
                     std::int32_t lsbDepth) noexcept;

        /// <summary>The view after the last Advance.</summary>
        View CurrentView() const noexcept { return {writePos_, count_, entries_}; }

        /// <summary>
        /// Keeps a copy of every finished entry in history (HistorySize entries, indexed by the
        /// absolute entry number), for GetInfoFrom; survives Reset.
        /// </summary>
        void SetHistory(AnalysisInfo* history) noexcept { history_ = history; }

        /// <summary>
        /// tonality_get_info with the read position of this object and the analysis of a chain
        /// as it was at view (the chain may be up to HistorySize - 100 entries ahead).
        /// </summary>
        void GetInfoFrom(const ToneAnalysis& chain, const View& view, AnalysisInfo& info, std::int32_t len) noexcept;

        /// <summary>What count calls of GetInfoFrom (with views[i]) would give, without moving the read position.</summary>
        void PeekInfoFrom(const ToneAnalysis& chain, const View* views, AnalysisInfo* infos, std::int32_t count, std::int32_t len) const noexcept;

    private:
        static constexpr std::int32_t NbFrames         = 8;
        static constexpr std::int32_t NbTBands         = 18;
        static constexpr std::int32_t AnalysisBufSize  = 720; // 30 ms at 24 kHz
        static constexpr std::int32_t AnalysisCountMax = 10000;
        static constexpr std::int32_t DetectSize       = 100;
        static constexpr std::int32_t MaxNeurons       = 32;

        void Analyze(const float* x, std::int32_t len, std::int32_t offset, std::int32_t c1, std::int32_t c2, std::int32_t channels,
                     std::int32_t lsbDepth) noexcept;
        void AnalyzeFrame(const float* x, std::int32_t len, std::int32_t offset, std::int32_t c1, std::int32_t c2, std::int32_t channels,
                          std::int32_t lsbDepth) noexcept;
        // the entry at ring position pos (0 .. DetectSize - 1) as the reader sees it at view
        const AnalysisInfo& EntryAt(const View& view, std::int32_t pos) const noexcept;
        float               DownmixAndResample(const float* x, float* y, std::int32_t subframe, std::int32_t offset, std::int32_t c1, std::int32_t c2,
                                               std::int32_t channels) noexcept;

        std::int32_t  fs_      = 0;
        AnalysisInfo* history_ = nullptr; // a chain's copies of the finished entries, or none
        // everything below is cleared by Reset (TONALITY_ANALYSIS_RESET_START)
        float        angle_[240];
        float        dAngle_[240];
        float        d2Angle_[240];
        float        inmem_[AnalysisBufSize];
        std::int32_t memFill_; // usable samples in inmem_
        float        prevBandTonality_[NbTBands];
        float        prevTonality_;
        std::int32_t prevBandwidth_;
        float        e_[NbFrames][NbTBands];
        float        logE_[NbFrames][NbTBands];
        float        lowE_[NbTBands];
        float        highE_[NbTBands];
        float        meanE_[NbTBands + 1];
        float        mem_[32];
        float        cmean_[8];
        float        std_[9];
        float        eTracker_;
        float        lowECount_;
        std::int32_t eCount_;
        std::int32_t count_;
        std::int32_t analysisOffset_;
        std::int32_t writePos_;
        std::int32_t readPos_;
        std::int32_t readSubframe_;
        float        hpEnerAccum_;
        std::int32_t initialized_;
        float        rnnState_[MaxNeurons];
        float        downmixState_[3];
        AnalysisInfo info_[DetectSize];
        std::int64_t entries_; // the entries written since the reset
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
