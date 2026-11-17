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

#include "NodeProfile.h"

// headers with imports last
#include "SharedSampleStream.h"

import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>
    /// Node Gain of the parallel encoder graph (EncodeGraph.h), one per channel: the ReplayGain analysis of its channel
    /// (ChannelGain), in LAME's pieces as the sample stream grows. The task reads the stream through a port of its own
    /// (SharedSampleStream::GainReader). The channels need nothing from each other until the end, where the stream
    /// joins their window sums for Encode; so the analysis, a serial chain of two filters, runs beside the frame stages
    /// instead of in Dispatch.
    /// </summary>
    class GainTask final : public Tasks::Task
    {
    public:
        GainTask(SharedSampleStream& stream, std::int32_t channel, TaskProfile* profile) noexcept : stream_(stream), channel_(channel), profile_(profile) {}

        void Run() noexcept override;

    private:
        SharedSampleStream& stream_;
        std::int32_t        channel_;
        TaskProfile*        profile_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
