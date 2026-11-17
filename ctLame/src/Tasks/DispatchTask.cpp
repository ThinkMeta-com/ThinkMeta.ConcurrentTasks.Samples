// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "DispatchTask.h"

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    DispatchTask::DispatchTask(BlockPipe& pcm, SharedSampleStream& stream, const PcmReader::Format& format, FrameRing& ring, TaskProfile* profile) noexcept
        : feed_(pcm, stream, format),
          stream_(stream),
          ring_(ring),
          profile_(profile)
    {}

    void DispatchTask::Run() noexcept
    {
        const ProfiledRun run(profile_);
        while (feed_.Next())
            DispatchReady();
        feed_.End();     // the end of the sample stream: its last samples and the padding
        DispatchReady(); // the frames these complete
        ring_.EndDispatch();
    }

    void DispatchTask::DispatchReady() noexcept
    {
        const std::int64_t ready = stream_.FramesReady();
        for (; next_ < ready; ++next_) {
            FrameData& frame = ring_.AcquireDispatch();
            // the frame points to its window in the stream, nothing is copied; Encode releases the window
            stream_.Window(next_, frame.input);
            ring_.CompleteDispatch();
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
