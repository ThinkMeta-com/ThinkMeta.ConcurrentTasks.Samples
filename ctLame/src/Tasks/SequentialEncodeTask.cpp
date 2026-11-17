// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "SequentialEncodeTask.h"

#include "Memory.h"

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    SequentialEncodeTask::SequentialEncodeTask(BlockPipe& pcm, SharedSampleStream& stream, const PcmReader::Format& format, Encoder& encoder,
                                               FrameEncoder& frameEncoder, TaskProfile* profile) noexcept
        : feed_(pcm, stream, format),
          stream_(stream),
          encoder_(encoder),
          frameEncoder_(frameEncoder),
          profile_(profile)
    {
        // zeroed, like the FrameData of the encoder
        frame_ = static_cast<FrameData*>(Shared::Heap::Allocate(sizeof(FrameData), true));
    }

    SequentialEncodeTask::~SequentialEncodeTask()
    {
        Shared::Heap::Free(frame_);
    }

    void SequentialEncodeTask::Run() noexcept
    {
        const ProfiledRun run(profile_);
        if (frame_ == nullptr) { // no memory for the frame: empty the pipe so that Read can finish, report the error
            feed_.Drain();
            feed_.End();
            frameEncoder_.Finish(0, 0, FrameEncoder::OutOfMemory);
            return;
        }
        frameEncoder_.Begin();
        while (!Stopped() && feed_.Next())
            EncodeReady();
        if (Stopped())
            feed_.Drain(); // after an error of the encoder only empty the pipe, so that Read can finish
        feed_.End();       // the end of the sample stream: its last samples and the padding
        EncodeReady();     // the frames these complete (none after an error)

        // this task has ended the stream itself, so the end is there
        const SharedSampleStream::StreamEnd end = stream_.WaitForEnd();
        frameEncoder_.Finish(end.encoderPadding, end.radioGain, end.failed ? FrameEncoder::OutOfMemory : 0);
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

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
