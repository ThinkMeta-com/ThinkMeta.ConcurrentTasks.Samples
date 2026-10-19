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
#include "NodeProfile.h"

// headers with imports last
#include "FrameRing.h"

import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>
    /// Node Resolve of the parallel encoder graph (EncodeGraph.h): takes the
    /// prepared frames from the ring in frame order and resolves them, the psychoacoustic model
    /// with state (attacks, block types, pre-echo control, M/S thresholds, perceptual entropy,
    /// ATH adjustment) and the filterbank carry. A serial link between the two parallel stages,
    /// so it runs with High priority.
    /// </summary>
    class ResolveTask : public Tasks::Task
    {
    public:
        ResolveTask(FrameRing& ring, Encoder& encoder, TaskProfile* profile) noexcept : ring_(ring), encoder_(encoder), profile_(profile) {}

        void Run() noexcept override
        {
            const ProfiledRun run(profile_);
            for (;;) {
                FrameData* const frame = WhileIdle(profile_, [&] { return ring_.AcquireResolve(); });
                if (frame == nullptr)
                    break;
                encoder_.Resolve(*frame);
                ring_.CompleteResolve();
            }
        }

    private:
        FrameRing&   ring_;
        Encoder&     encoder_;
        TaskProfile* profile_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
