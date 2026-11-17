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

#include <cstdint>

#include "Encoder.h"
#include "NodeProfile.h"

// headers with imports last
#include "FrameRing.h"

import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>
    /// Node Search of the parallel encoder graph (EncodeGraph.h): takes the resolved frames from the ring and
    /// searches them, MDCT, stereo decision, quantization search and main data. The stage keeps nothing from frame
    /// to frame, so the graph runs several of these tasks at once, each on its own frame. It runs with High
    /// priority: its frames are older than the ones Prepare works on, and they free slots sooner.
    /// </summary>
    class SearchTask final : public Tasks::Task
    {
    public:
        SearchTask(FrameRing& ring, const Encoder& encoder, TaskProfile* profile) noexcept : ring_(ring), encoder_(encoder), profile_(profile) {}

        void Run() noexcept override
        {
            const ProfiledRun run(profile_);
            while (FrameData* const frame = ring_.AcquireSearch()) {
                encoder_.Search(*frame);
                ring_.CompleteSearch(*frame);
            }
        }

    private:
        FrameRing&     ring_;
        const Encoder& encoder_;
        TaskProfile*   profile_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
