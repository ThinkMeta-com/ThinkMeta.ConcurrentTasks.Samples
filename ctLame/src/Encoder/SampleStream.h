// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "FrameData.h"
#include "GainAnalysis.h"
#include "Input.h"
#include "Types.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    struct SessionConfig;

    /// <summary>
    /// The encoder input as one float stream per channel in reserved virtual memory, port of
    /// LAME's input buffering (lame_copy_inbuffer, lame_encode_buffer_sample_t,
    /// fill_buffer and lame_encode_flush). The stream is what LAME's mfbuf holds over time:
    /// 528 zeros (ENCDELAY - MDCTDELAY), then every sample through the PCM transform (scaling,
    /// downmix), and at the end the flush padding as transformed zeros (-0.0 for a negative
    /// scale). Frame k reads the window of FrameInputSize samples from k * FRAMESIZE, so frames
    /// point into the stream instead of copying from it.
    ///
    /// One task writes (AddInt, Finish) and reads the windows; another may release frames,
    /// always in order. Memory is committed as the stream grows and decommitted behind the
    /// released frames, so only the frames in flight use memory although the reservation
    /// covers the whole input.
    /// </summary>
    class SampleStream : public IPcmSink
    {
    public:
        SampleStream() noexcept = default;
        ~SampleStream();

        SampleStream(const SampleStream&)            = delete;
        SampleStream& operator=(const SampleStream&) = delete;

        /// <summary>
        /// Reserves the stream for at most maxInputSamples samples per channel (0: unknown, a
        /// large reservation). False if the address space is not available. With findReplayGain the
        /// stream analyzes the ReplayGain itself in AddInt and Finish, or with gainTasks leaves it to
        /// tasks of their own, one per channel (GainPiece, Samples, GainProgress, FinishGain).
        /// </summary>
        bool Initialize(const SessionConfig& config, std::uint64_t maxInputSamples, bool gainTasks = false, SimdLevel simd = SimdLevel::Scalar) noexcept;

        // --- writer ---

        /// <summary>lame_encode_buffer_int: ints with the sample in the upper bits.</summary>
        void AddInt(const std::int32_t* left, const std::int32_t* right, std::int32_t count) noexcept;

        /// <summary>IPcmSink: the samples of PcmReader.</summary>
        void AddSamples(const std::int32_t* left, const std::int32_t* right, std::int32_t count) override { AddInt(left, right, count); }

        /// <summary>lame_encode_flush: pads with silence until the last sample is encoded.</summary>
        void Finish() noexcept;

        /// <summary>Frames whose window is complete; after Finish, all frames of the stream.</summary>
        std::int64_t FramesReady() const noexcept { return framesReady_; }

        /// <summary>The window of a ready frame: FrameInputSize samples per channel (input[1] is null for mono).</summary>
        void Window(std::int64_t frame, ChannelSamples& input) const noexcept;

        /// <summary>Samples of padding at the end (encoder_padding), known after Finish.</summary>
        std::int32_t EncoderPadding() const noexcept { return encoderPadding_; }

        /// <summary>The ReplayGain of the input in 0.1 dB (RadioGain), known after Finish; 0 without the analysis.</summary>
        std::int32_t RadioGain() const noexcept { return radioGain_; }

        /// <summary>Whether the stream ran out of memory and lost samples; the output is then incomplete.</summary>
        bool Failed() const noexcept { return failed_; }

        // --- ReplayGain in tasks of their own (Initialize with gainTasks) ---

        /// <summary>The analysis, null without findReplayGain; a gain task works on its Channel.</summary>
        GainAnalysis* Gain() noexcept { return gain_; }

        /// <summary>
        /// LAME's next AnalyzeSamples call from position on (the first is at GainStart): its samples per channel; 0
        /// while they are not in the stream yet, -1 after the last call.
        /// </summary>
        std::int32_t GainPiece(std::uint64_t position) const noexcept;

        /// <summary>The stream length the gain tasks may read (for GainSignals).</summary>
        std::uint64_t GainAvailable() const noexcept { return available_.load(std::memory_order_acquire) & ~Ended; }

        /// <summary>The position of the first AnalyzeSamples call: behind the start zeros.</summary>
        static std::uint64_t GainStart() noexcept;

        /// <summary>The samples of a channel from position on.</summary>
        const sample_t* SamplesOf(std::int32_t channel, std::uint64_t position) const noexcept;

        /// <summary>
        /// A gain task has analyzed its channel up to position; Release may decommit the samples before it. Release
        /// decommits in chunks of several pieces, so a task reports only every few pieces (it shares the cache line
        /// with the releaser).
        /// </summary>
        void GainProgress(std::int32_t channel, std::uint64_t position) noexcept;

        /// <summary>save_gain_values once all channels are analyzed: the radio gain (RadioGain).</summary>
        void FinishGain() noexcept;

        // --- releaser ---

        /// <summary>Frame is done: its samples that no later frame reads may be decommitted. Frames are released in order.</summary>
        void Release(std::int64_t frame) noexcept;

    private:
        bool Reserve(std::uint64_t samples) noexcept;
        bool Grow(std::uint64_t length) noexcept;
        void Append(const ChannelSamples& samples, std::int32_t count) noexcept;
        void UpdateFramesReady() noexcept;
        void PlanGainTail() noexcept;
        void AnalyzeGainInline() noexcept;

        /// <summary>In available_: Finish is done, so inputLength_ and tail_ are final.</summary>
        static constexpr std::uint64_t Ended = 1ull << 63;

        // ReplayGain (findReplayGain): LAME's AnalyzeSamples runs in lame_encode_buffer_sample_t on each piece that
        // fill_buffer copies into mfbuf, so the stream is analyzed in those pieces: the input in the frontend's reads
        // of FRAMESIZE samples, the last read shorter, then the flush padding in the bunches of lame_encode_flush.
        // Written by Initialize and Finish, read by the gain tasks after available_ says Ended.
        GainAnalysis*                gain_      = nullptr; // on the heap (about 120 KB)
        bool                         gainTasks_ = false;
        std::array<std::int32_t, 16> tail_{}; // the bunches of lame_encode_flush
        std::int32_t                 tailCount_ = 0;
        std::uint64_t                tailStart_ = 0;
        std::int32_t                 radioGain_ = 0;

        std::array<sample_t*, 2>            base_           = {};
        std::size_t                         reserved_       = 0; // bytes per channel
        std::int32_t                        channels_       = 2;
        std::int32_t                        channelsIn_     = 2;
        std::array<std::array<float, 2>, 2> transform_      = {};
        std::array<sample_t, 2>             zero_           = {}; // a transformed zero sample, the flush padding
        bool                                finished_       = false;
        bool                                failed_         = false;
        std::int32_t                        encoderPadding_ = 0;

        // writer side
        std::uint64_t length_      = 0; // samples per channel in the stream
        std::uint64_t inputLength_ = 0; // samples that came in through AddInt
        std::size_t   committed_   = 0; // bytes per channel from the start of the reservation
        std::int64_t  framesReady_ = 0;
        std::uint64_t gainInline_  = 0; // without gain tasks: the position analyzed

        std::atomic<std::uint64_t> available_{0}; // the stream length the gain tasks may read, with Ended in one value

        std::array<std::array<sample_t, FRAMESIZE>, 2> buffer_; // one block through the transform

        // releaser side, with what it reads from the gain tasks (they report only every few pieces, GainProgress)
        alignas(64) std::size_t decommitted_ = 0;                  // bytes per channel from the start of the reservation
        std::array<std::atomic<std::uint64_t>, 2> gainPosition_{}; // what each gain task has analyzed
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
