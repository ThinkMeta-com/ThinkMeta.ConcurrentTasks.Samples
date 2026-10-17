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
#include "FrameData.h"
#include "Input.h"
#include "Profiling.h"
#include "SampleStream.h"

// headers with imports last
#include "BlockPipe.h"
#include "FrameEncoder.h"
#include "PcmFeed.h"

import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    /// <summary>
    /// The one compute node of the sequential encoder graph (--sequential, EncodeGraph.h): takes
    /// the WAV data from the PCM pipe into the sample stream (PcmFeed) and runs all four calls of
    /// the encoder on each frame in turn, Prepare, Resolve, Search and Encode (FrameEncoder), into
    /// the MP3 pipe. The same encoder and the same edges as the parallel graph, on one thread: the
    /// reference for bisecting the parallel path.
    /// </summary>
    class SequentialEncodeTask : public Tasks::Task
    {
    public:
        SequentialEncodeTask(BlockPipe& pcm, SampleStream& stream, const PcmReader::Format& format, Encoder& encoder, FrameEncoder& frameEncoder,
                             TaskProfile* profile) noexcept;
        ~SequentialEncodeTask();

        void Run() noexcept override;

    private:
        void EncodeReady() noexcept;
        bool Stopped() const noexcept { return frameEncoder_.Error() != 0; }

        PcmFeed       feed_;
        SampleStream& stream_;
        Encoder&      encoder_;
        FrameEncoder& frameEncoder_;
        TaskProfile*  profile_;
        FrameData*    frame_ = nullptr; // the one frame in flight
        std::int64_t  next_  = 0;       // the next frame to encode
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
