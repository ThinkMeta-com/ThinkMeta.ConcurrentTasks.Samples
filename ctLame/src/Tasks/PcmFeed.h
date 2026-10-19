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

#include "Input.h"
#include "NodeProfile.h"
#include "SampleStream.h"

// headers with imports last
#include "BlockPipe.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>
    /// The edge from the PCM pipe into the sample stream: converts the WAV blocks
    /// of the reader into the float stream of each channel (read_samples_pcm, then
    /// lame_copy_inbuffer), and at the end of the pipe completes the stream with the last,
    /// incomplete block and the flush padding (lame_encode_flush). The task at the start of the
    /// encoder (DispatchTask, SequentialEncodeTask) drives it.
    /// </summary>
    class PcmFeed
    {
    public:
        PcmFeed(BlockPipe& pcm, SampleStream& stream, const PcmReader::Format& format) noexcept;

        /// <summary>
        /// Converts the next piece of the pipe into the stream, at most StepSize bytes of a block;
        /// waits for a block (idle time into profile). At the end of the pipe it completes the
        /// stream and returns false.
        /// </summary>
        bool Next(TaskProfile* profile) noexcept;

        /// <summary>Bytes converted per step: independent of the read size, so the frames go out evenly.</summary>
        static constexpr std::uint32_t StepSize = 64 * 1024;

        /// <summary>After an error: releases the rest of the pipe without converting it, so the reader can finish.</summary>
        void Drain(TaskProfile* profile) noexcept;

    private:
        BlockPipe&           pcm_;
        SampleStream&        stream_;
        PcmReader            reader_;
        BlockPipe::ReadBlock block_{};    // the block being converted
        std::uint32_t        offset_ = 0; // its bytes converted so far
        bool                 held_   = false;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
