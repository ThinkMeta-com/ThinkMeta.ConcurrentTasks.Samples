// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "DispatchTask.h"

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    DispatchTask::DispatchTask(BlockPipe& pcm, SampleStream& stream, const PcmReader::Format& format, FrameRing& ring, TaskProfile* profile) noexcept
        : feed_(pcm, stream, format),
          stream_(stream),
          ring_(ring),
          profile_(profile)
    {}

    void DispatchTask::Run() noexcept
    {
        const std::int64_t begin = ProfileTaskBegin(profile_);

        while (feed_.Next(profile_))
            DispatchReady();
        DispatchReady(); // the rest of the input and the flush frames
        ring_.EndDispatch();

        ProfileTaskEnd(profile_, begin);
    }

    void DispatchTask::DispatchReady() noexcept
    {
        const std::int64_t ready = stream_.FramesReady();
        for (; next_ < ready; ++next_) {
            FrameData* frame;
            {
                ProfileScope idle(IdleCounter(profile_));
                frame = &ring_.AcquireDispatch();
            }
            // the workers prepare the frame from its window
            stream_.Window(next_, frame->input);
            ring_.CompleteDispatch();
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
