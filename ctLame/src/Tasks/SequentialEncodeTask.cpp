// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "SequentialEncodeTask.h"

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    SequentialEncodeTask::SequentialEncodeTask(BlockPipe& pcm, SampleStream& stream, const PcmReader::Format& format, Encoder& encoder,
                                               FrameEncoder& frameEncoder, TaskProfile* profile) noexcept
        : feed_(pcm, stream, format),
          stream_(stream),
          encoder_(encoder),
          frameEncoder_(frameEncoder),
          profile_(profile)
    {
        // zeroed, like the FrameData of the encoder
        frame_ = static_cast<FrameData*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(FrameData)));
    }

    SequentialEncodeTask::~SequentialEncodeTask()
    {
        if (frame_ != nullptr)
            HeapFree(GetProcessHeap(), 0, frame_);
    }

    void SequentialEncodeTask::Run() noexcept
    {
        const ProfiledRun run(profile_);
        if (frame_ == nullptr) { // no memory for the frame: drain the input so the reader can finish, report the error
            feed_.Drain(profile_);
            frameEncoder_.Finish(0, -3);
            return;
        }
        frameEncoder_.Begin();
        bool more = true;
        while (more && !Stopped()) {
            more = feed_.Next(profile_);
            EncodeReady(); // after the end of the pipe: the rest of the input and the flush frames
        }
        if (more)
            feed_.Drain(profile_); // after an error only drain the pipe, so the reader can finish
        frameEncoder_.Finish(stream_.EncoderPadding(), stream_.Failed() ? -3 : 0);
    }

    void SequentialEncodeTask::EncodeReady() noexcept
    {
        const std::int64_t ready = stream_.FramesReady();
        for (; next_ < ready && !Stopped(); ++next_) {
            ChannelSamples input;
            stream_.Window(next_, input);

            frame_->frameNumber = next_;
            encoder_.Prepare(input, *frame_);
            encoder_.Resolve(*frame_);
            encoder_.Search(*frame_);
            frameEncoder_.EncodeFrame(*frame_);
            stream_.Release(next_);
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
