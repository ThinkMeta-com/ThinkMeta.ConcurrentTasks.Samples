// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#if defined(_WIN32)
#    include <Windows.h>
#endif

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <array>
#include <atomic>
#include <cstdint>

#include "FrameData.h"
#include "GainAnalysis.h"
#include "Input.h"
#include "Profiling.h"
#include "SampleStream.h"
#include "Types.h"

// headers with imports last
import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    struct SessionConfig;

    /// <summary>
    /// The edge between the start of the encoder, the Gain tasks and Encode (EncodeGraph.h): the sample stream
    /// (SampleStream.h) as the object in which its tasks meet. One task writes; with ReplayGain a reader per channel
    /// follows the stream in LAME's pieces; Encode releases the frames and takes the end of the stream.
    /// <code>
    ///  writer (Dispatch) ──AddSamples, End──► stream ──NextPiece──► Gain L, Gain R (a GainReader each)
    ///                                            └──────WaitForEnd──► Encode
    /// </code>
    /// Who waits: a gain reader for the next piece of its channel, Encode for the end. Who wakes: the writer wakes the
    /// readers as the stream grows; the writer's End and the last reader that ends wake Encode. The end: End, on every
    /// path of the writer. It reaches every reader behind the samples: NextPiece returns false after the last piece.
    /// What is only known then travels with it: the padding of the encoder, the ReplayGain and whether the stream lost
    /// samples (StreamEnd).
    ///
    /// The stream counts its readers when it is initialized: the graph starts one Gain task per reader
    /// (GainReaderCount), or Encode would wait for a reader that never ends. In the sequential graph one task is
    /// writer and Encode, and the stream analyzes the ReplayGain itself.
    /// </summary>
    class SharedSampleStream : public IPcmSink
    {
    public:
        /// <summary>What the stream knows once it has ended.</summary>
        struct StreamEnd {
            std::int32_t encoderPadding = 0;     // samples of padding at the end (encoder_padding)
            std::int32_t radioGain      = 0;     // the ReplayGain of the input in 0.1 dB; 0 without the analysis
            bool         failed         = false; // the stream ran out of memory and lost samples
        };

        /// <summary>A piece of one channel for the ReplayGain analysis: LAME's next AnalyzeSamples call.</summary>
        struct Piece {
            const sample_t* samples = nullptr;
            std::size_t     count   = 0;
        };

        /// <summary>
        /// The port of a Gain task: remembers where its reader stands in the stream. It belongs to one task, which keeps
        /// it for the time it reads.
        /// </summary>
        class GainReader
        {
        public:
            /// <summary>The analysis of this reader's channel.</summary>
            ChannelGain& Analysis() noexcept { return owner_->stream_.Gain()->Channel(channel_); }

            /// <summary>
            /// The next piece of the channel; waits until the stream has it. The piece before is then analyzed, so the
            /// stream may free it. False after the last piece: the reader has ended.
            /// </summary>
            bool NextPiece(Piece& piece) noexcept;

        private:
            friend class SharedSampleStream;
            GainReader(SharedSampleStream& owner, std::int32_t channel) noexcept;

            SharedSampleStream* owner_;
            std::int32_t        channel_;
            std::int32_t        unreported_ = 0; // pieces analyzed since the stream was told
            std::uint64_t       position_;       // the piece this reader takes next
            std::size_t         taken_ = 0;      // samples of the piece taken last
        };

        SharedSampleStream() noexcept = default;

        SharedSampleStream(const SharedSampleStream&)            = delete;
        SharedSampleStream& operator=(const SharedSampleStream&) = delete;

        /// <summary>Who analyzes the ReplayGain of the input, if the session asks for it.</summary>
        enum class ReplayGainBy {
            Stream, // the stream itself, as it is written
            Tasks,  // a Gain task per channel, each through a GainReader
        };

        /// <summary>Reserves the stream (SampleStream::Initialize); false if the address space is not available.</summary>
        bool Initialize(const SessionConfig& config, std::uint64_t maxInputSamples, ReplayGainBy replayGainBy, SimdLevel simd) noexcept;

        /// <summary>
        /// CTLAME_PROFILE: the time the Gain tasks and Encode wait here counts as idle time of their nodes; null: not
        /// measured. Call it before the tasks start.
        /// </summary>
        void MeasureWaits(TaskProfile* gain, TaskProfile* encode) noexcept
        {
            gainIdle_   = IdleCounter(gain);
            encodeIdle_ = IdleCounter(encode);
        }

        // --- the writer: Dispatch, or SequentialEncode ---

        /// <summary>IPcmSink: the samples of PcmReader go into the stream; wakes the gain readers once enough is new.</summary>
        void AddSamples(const std::int32_t* left, const std::int32_t* right, std::int32_t count) override;

        /// <summary>The end of the input: pads the stream (lame_encode_flush) and ends it for all readers. Call it on every path.</summary>
        void End() noexcept;

        /// <summary>Frames whose window is complete; after End, all frames of the stream.</summary>
        std::int64_t FramesReady() const noexcept { return stream_.FramesReady(); }

        /// <summary>The window of a ready frame (SampleStream::Window).</summary>
        void Window(std::int64_t frame, ChannelSamples& input) const noexcept { stream_.Window(frame, input); }

        // --- the gain readers: one Gain task per channel ---

        /// <summary>The readers the stream waits for: one per channel with ReplayGainBy::Tasks, else none.</summary>
        std::int32_t GainReaderCount() const noexcept { return readers_; }

        /// <summary>The port of the reader of a channel; each reader opens its own, once.</summary>
        GainReader OpenGainReader(std::int32_t channel) noexcept { return GainReader(*this, channel); }

        // --- Encode ---

        /// <summary>A frame is encoded: its samples that no later frame and no gain reader needs are freed. In frame order.</summary>
        void Release(std::int64_t frame) noexcept { stream_.Release(frame); }

        /// <summary>
        /// Waits until the writer has ended the stream and every gain reader has read it to the end; returns what is
        /// known then. One task waits.
        /// </summary>
        StreamEnd WaitForEnd() noexcept;

    private:
        void WakeGainReaders(bool ended) noexcept;
        void ParticipantEnded() noexcept;

        /// <summary>The pieces the writer collects before it wakes the gain readers.</summary>
        static constexpr std::uint64_t WakeBatch = 32;

        SampleStream stream_;

        std::atomic<std::int64_t>* gainIdle_   = nullptr; // CTLAME_PROFILE
        std::atomic<std::int64_t>* encodeIdle_ = nullptr;

        std::int32_t                                     readers_     = 0;
        bool                                             writerEnded_ = false; // writer: End was called
        std::uint64_t                                    signaled_    = 0;     // writer: the pieces the readers were woken for
        std::array<Synchronization::AutoResetEvent_s, 2> wake_;                // a reader waits for more of the stream

        std::atomic<std::int32_t>         pending_{1}; // the writer and the gain readers that have not ended yet
        Synchronization::AutoResetEvent_s ended_;      // Encode waits for the end
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
