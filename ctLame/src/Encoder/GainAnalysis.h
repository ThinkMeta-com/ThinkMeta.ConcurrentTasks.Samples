// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "GainKernels.h"
#include "SimdFeatures.h"
#include "Types.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>
    /// One channel of the ReplayGain analysis of gain_analysis.c: a Yule-Walker and a Butterworth filter, then the sum
    /// of the squares of every 50 ms window. LAME runs both channels in one AnalyzeSamples call, but the channels only
    /// meet at the end of each window ((lsum + rsum) / 2), so each channel can run on its own and keep its window sums.
    /// <para>
    /// Each filter runs over the channel as if in one go (LAME keeps the history across segments, windows and calls),
    /// so the Yule filter is split into its input part (FIR, with SIMD kernels) and the serial rest. Only the float sums depend
    /// on how the samples are cut into Analyze calls, so a caller that wants LAME's bits has to call it with LAME's
    /// pieces (SampleStream::GainPiece), of at most MaxPiece samples.
    /// </para>
    /// </summary>
    class ChannelGain
    {
    public:
        ChannelGain() noexcept = default;
        ~ChannelGain();

        ChannelGain(const ChannelGain&)            = delete;
        ChannelGain& operator=(const ChannelGain&) = delete;

        /// <summary>The most samples of one Analyze call: FRAMESIZE, the frontend's read.</summary>
        static constexpr std::size_t MaxPiece = FRAMESIZE;

        /// <summary>ResetSampleFrequency and InitGainAnalysis for this channel: false for a rate without filters.</summary>
        bool Initialize(std::int32_t samplerate, SimdLevel simd) noexcept;

        /// <summary>AnalyzeSamples for this channel: the next count samples (at most MaxPiece).</summary>
        void Analyze(const sample_t* samples, std::size_t count) noexcept;

        /// <summary>Whether the window sums ran out of memory (or a piece was too long): the gain is then unknown.</summary>
        bool Failed() const noexcept { return failed_; }

        /// <summary>The windows completed so far, each with the sum of its squared filtered samples.</summary>
        std::size_t   Windows() const noexcept { return windows_; }
        const double* WindowSums() const noexcept { return sums_; }

        /// <summary>The samples of a window (sampleWindow).</summary>
        std::int64_t SampleWindow() const noexcept { return sampleWindow_; }

    private:
        static constexpr std::int32_t YuleOrder   = 10;
        static constexpr std::int32_t ButterOrder = 2;
        static constexpr std::int32_t MaxOrder    = YuleOrder; // MAX_ORDER

        // the filters: input, Yule output and Butterworth output, each behind its history
        std::array<sample_t, MaxOrder + MaxPiece>    in_{};
        std::array<sample_t, MaxPiece>               s1_{}; // the input part of the filter in hand
        std::array<sample_t, MaxOrder + MaxPiece>    yuleOut_{};
        std::array<sample_t, ButterOrder + MaxPiece> butterOut_{};
        GainFirFunction                              yuleFir_      = nullptr;
        std::int64_t                                 sampleWindow_ = 0;
        std::int64_t                                 totsamp_      = 0;
        double                                       sum_          = 0;   // lsum or rsum
        std::array<float, 2 * YuleOrder + 1>         yule_{};             // ABYule[freqindex] as Float_t
        std::array<float, 2 * ButterOrder + 1>       butter_{};           // ABButter[freqindex] as Float_t
        double*                                      sums_     = nullptr; // per window, on the heap
        std::size_t                                  windows_  = 0;
        std::size_t                                  capacity_ = 0;
        bool                                         failed_   = false;
    };

    /// <summary>
    /// ReplayGain analysis of one track (gain_analysis.c) as LAME 4.0 uses it for the "radio" gain of the LAME tag: a
    /// ChannelGain per channel, at the end the histogram of the window levels, whose 95th percentile gives the gain.
    /// </summary>
    class GainAnalysis
    {
    public:
        /// <summary>GAIN_NOT_ENOUGH_SAMPLES: the title gain of a track without a full window.</summary>
        static constexpr float NotEnoughSamples = -24601.0f;

        /// <summary>InitGainAnalysis: false for a sample rate without filter coefficients. simd selects the bit-identical kernels.</summary>
        bool Initialize(std::int32_t samplerate, std::int32_t channels, SimdLevel simd) noexcept;

        /// <summary>The analysis of one channel (0 or 1, only 0 with one channel), for a task of its own.</summary>
        ChannelGain& Channel(std::int32_t channel) noexcept { return channels_[channel]; }

        std::int32_t Channels() const noexcept { return channelCount_; }

        /// <summary>AnalyzeSamples: the next count samples of each channel at once (right is ignored for one channel).</summary>
        void Analyze(const sample_t* left, const sample_t* right, std::size_t count) noexcept;

        /// <summary>GetTitleGain once all samples are analyzed: the gain in dB, or NotEnoughSamples (also if a channel Failed).</summary>
        float TitleGain() noexcept;

    private:
        static constexpr std::int32_t StepsPerDb = 100;
        static constexpr std::int32_t MaxDb      = 120;

        std::array<ChannelGain, 2>                    channels_;
        std::int32_t                                  channelCount_ = 2;
        std::array<std::uint32_t, StepsPerDb * MaxDb> histogram_{}; // A
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
