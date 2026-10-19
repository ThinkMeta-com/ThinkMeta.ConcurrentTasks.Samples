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

#include "NodeProfile.h"
#include "SampleStream.h"

// headers with imports last
#include "FrameEncoder.h"
#include "FrameRing.h"

import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>
    /// Node Encode of the parallel encoder graph (EncodeGraph.h): takes the
    /// searched frames from the ring in frame order, encodes them with the FrameEncoder
    /// (quantization with the reservoir, bit stream, into the MP3 pipe) and frees each frame's
    /// slot and input window. A serial node, so it runs with High priority: the pipeline drains
    /// before the input runs further ahead.
    /// </summary>
    class EncodeTask : public Tasks::Task
    {
    public:
        EncodeTask(FrameRing& ring, SampleStream& stream, FrameEncoder& frameEncoder, TaskProfile* profile) noexcept
            : ring_(ring),
              stream_(stream),
              frameEncoder_(frameEncoder),
              profile_(profile)
        {}

        void Run() noexcept override;

    private:
        FrameRing&    ring_;
        SampleStream& stream_;
        FrameEncoder& frameEncoder_;
        TaskProfile*  profile_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
