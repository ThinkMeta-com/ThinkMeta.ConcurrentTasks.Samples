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
        for (;;) {
            FrameData* const frame = WhileIdle(profile_, [&] { return ring_.AcquireEncode(); });
            if (frame == nullptr)
                break;
            // after an error the frames are only released, so the analysis can finish
            frameEncoder_.EncodeFrame(*frame);
            stream_.Release(frame->frameNumber);
            ring_.CompleteEncode();
        }

        // the ring has ended, so the analysis has finished the sample stream
        frameEncoder_.Finish(stream_.EncoderPadding(), stream_.Failed() ? -3 : 0);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
