// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <cstdint>

#include "Encoder.h"
#include "Profiling.h"

// headers with imports last
#include "FrameRing.h"

import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    /// <summary>
    /// Nodes Prepare and Search of the parallel encoder graph (EncodeGraph.h): one worker per
    /// compute thread claims work from the frame ring, the preparation of a
    /// dispatched frame (psychoacoustic model without state, polyphase filterbank) or the search
    /// of a resolved frame (MDCT, stereo decision, quantization search, main data). The workers
    /// run at once, each on its own frame.
    /// </summary>
    class WorkerTask : public Tasks::Task
    {
    public:
        WorkerTask(FrameRing& ring, const Encoder& encoder, TaskProfile* profile) noexcept : ring_(ring), encoder_(encoder), profile_(profile) {}

        void Run() noexcept override
        {
            const std::int64_t begin = ProfileTaskBegin(profile_);
            for (;;) {
                FrameData*      frame;
                FrameRing::Work work;
                {
                    ProfileScope idle(IdleCounter(profile_));
                    frame = ring_.AcquireWork(work);
                }
                if (frame == nullptr)
                    break;
                if (work == FrameRing::Work::Prepare)
                    encoder_.Prepare(frame->input, *frame);
                else
                    encoder_.Search(*frame);
                ring_.CompleteWork(*frame, work);
            }
            ProfileTaskEnd(profile_, begin);
        }

    private:
        FrameRing&     ring_;
        const Encoder& encoder_;
        TaskProfile*   profile_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
