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

#include "Input.h"
#include "NodeProfile.h"

// headers with imports last
#include "BlockPipe.h"
#include "FrameRing.h"
#include "PcmFeed.h"
#include "SharedSampleStream.h"

import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>
    /// Node Dispatch of the parallel encoder graph (EncodeGraph.h): takes the
    /// WAV data from the PCM pipe into the sample stream (PcmFeed) and dispatches each frame
    /// whose window is complete into the frame ring, in frame order. Holds back while all slots
    /// of the ring are in use, so the input runs at most a ring ahead of the encoding. It is the writer of the
    /// sample stream (SharedSampleStream.h): PcmFeed adds the samples for it, and it ends the stream.
    ///
    /// Converting and dispatching are one task, although each has state of its own: a frame can only be dispatched
    /// once its samples are in the stream, so the two never run at the same time, and the conversion is a small part
    /// of the work (ReplayGain, which is not, has tasks of its own: GainTask.h).
    /// </summary>
    class DispatchTask final : public Tasks::Task
    {
    public:
        DispatchTask(BlockPipe& pcm, SharedSampleStream& stream, const PcmReader::Format& format, FrameRing& ring, TaskProfile* profile) noexcept;

        void Run() noexcept override;

    private:
        void DispatchReady() noexcept;

        PcmFeed             feed_;
        SharedSampleStream& stream_;
        FrameRing&          ring_;
        TaskProfile*        profile_;
        std::int64_t        next_ = 0; // the next frame to dispatch
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
