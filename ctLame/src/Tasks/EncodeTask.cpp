// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "EncodeTask.h"

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    void EncodeTask::Run() noexcept
    {
        const ProfiledRun run(profile_);
        frameEncoder_.Begin();
        // after an error EncodeFrame does nothing; the frames are still taken and released, so the stages before and
        // the Gain tasks run to their end
        while (FrameData* const frame = ring_.AcquireEncode()) {
            frameEncoder_.EncodeFrame(*frame);
            stream_.Release(frame->frameNumber);
            ring_.CompleteEncode();
        }

        // the end of the sample stream: the padding, the ReplayGain of the Gain tasks and whether samples were lost
        const SharedSampleStream::StreamEnd end = stream_.WaitForEnd();
        frameEncoder_.Finish(end.encoderPadding, end.radioGain, end.failed ? FrameEncoder::OutOfMemory : 0);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
