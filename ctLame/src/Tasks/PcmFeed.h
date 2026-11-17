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
// headers with imports last
#include "BlockPipe.h"
#include "SharedSampleStream.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>
    /// The work at the start of the encoder, from the PCM pipe into the sample stream: converts the WAV blocks
    /// of the reader into the float stream of each channel (read_samples_pcm, then
    /// lame_copy_inbuffer), and at the end of the pipe completes the stream with the last,
    /// incomplete block and the flush padding (lame_encode_flush). Not an edge: one task owns it and nobody waits
    /// in it. The task at the start of the encoder (DispatchTask, SequentialEncodeTask) drives it and is thereby the
    /// writer of the stream: it calls Next until the pipe has ended, and End on every path.
    /// </summary>
    class PcmFeed
    {
    public:
        PcmFeed(BlockPipe& pcm, SharedSampleStream& stream, const PcmReader::Format& format) noexcept;

        /// <summary>
        /// Converts the next piece of the pipe into the stream, at most StepSize bytes of a block; waits for a block.
        /// False once the pipe has ended.
        /// </summary>
        bool Next() noexcept;

        /// <summary>
        /// The end of the input (the end of lame_encoder_loop): the last, incomplete block goes into the stream, and
        /// the stream is padded and ended. Call it on every path.
        /// </summary>
        void End() noexcept;

        /// <summary>Bytes converted per step: independent of the read size, so the frames go out evenly.</summary>
        static constexpr std::uint32_t StepSize = 64 * 1024;

        /// <summary>
        /// After an error of the encoder (SequentialEncodeTask): releases the rest of the pipe without converting it, so
        /// Read can finish.
        /// </summary>
        void Drain() noexcept;

    private:
        BlockPipe&           pcm_;
        SharedSampleStream&  stream_;
        PcmReader            reader_;
        BlockPipe::ReadBlock block_{};    // the block being converted
        std::uint32_t        offset_ = 0; // its bytes converted so far
        bool                 held_   = false;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
