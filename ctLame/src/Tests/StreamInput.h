// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <cstdint>

#include "SampleStream.h"
#include "Types.h"

namespace LameTests {

    using ThinkMeta::ConcurrentTasks::Samples::Lame::sample_t;
    using ThinkMeta::ConcurrentTasks::Samples::Lame::ChannelSamples;

    /// <summary>Receives the input of one frame: the window of each channel in the sample stream (input[1] is null for mono).</summary>
    struct IFrameSink {
        virtual void EncodeFrame(const ChannelSamples& input) = 0;
    };

    /// <summary>
    /// The sample stream driven like LAME's lame_encode_buffer_int and lame_encode_flush: every
    /// frame goes to the sink as soon as its window is complete, then it is released.
    /// </summary>
    class StreamInput
    {
    public:
        bool Initialize(const ThinkMeta::ConcurrentTasks::Samples::Lame::SessionConfig& config) noexcept { return stream_.Initialize(config, 0); }

        void AddInt(const std::int32_t* left, const std::int32_t* right, std::int32_t count, IFrameSink& sink)
        {
            stream_.AddInt(left, right, count);
            Deliver(sink);
        }

        void Flush(IFrameSink& sink)
        {
            stream_.Finish();
            Deliver(sink);
        }

        std::int32_t Frames() const noexcept { return static_cast<std::int32_t>(frames_); }

        std::int32_t EncoderPadding() const noexcept { return stream_.EncoderPadding(); }

    private:
        void Deliver(IFrameSink& sink)
        {
            for (; frames_ < stream_.FramesReady(); ++frames_) {
                ChannelSamples input;
                stream_.Window(frames_, input);
                sink.EncodeFrame(input);
                stream_.Release(frames_);
            }
        }

        ThinkMeta::ConcurrentTasks::Samples::Lame::SampleStream stream_;
        std::int64_t                                             frames_ = 0;
    };

} // namespace LameTests
