// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "Profiling.h"

// headers with imports last
#include "BlockPipe.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>
    /// The ID3 tags of the frontend (lame_main.c): the version 2 tag goes in front of the MP3 stream, the version 1 tag
    /// behind it; each may be empty. The memory stays valid until the encoding has ended.
    /// </summary>
    struct Id3Bytes {
        const std::byte* v2     = nullptr;
        std::uint32_t    v2Size = 0;
        const std::byte* v1     = nullptr;
        std::uint32_t    v1Size = 0;
    };

    /// <summary>
    /// The edge between Encode and Write (EncodeGraph.h): the MP3 file as its sections, in file order.
    /// <code>
    ///  ID3v2 tag | MP3 stream: placeholder of the LAME tag, frames, flushed rest | ID3v1 tag
    ///            ^ the LAME tag frame replaces the placeholder once the stream has ended
    /// </code>
    /// Encode appends the stream and ends it. With the end it hands over what is only known then: whether the stream is
    /// complete (then the ID3v1 tag follows) and the LAME tag frame. Write takes the sections in file order.
    ///
    /// Who waits: Write for the next block of the stream, Encode for a free block; the stream is a bounded BlockPipe, so
    /// Encode never runs far ahead of the file. Who wakes: the other side, inside the pipe. The end: End or
    /// EndAfterError, on every path of Encode; what they hand over is set before the pipe's end mark, so Write reads it
    /// only after AcquireBlock has returned false. One producer, one consumer.
    /// </summary>
    class Mp3File
    {
    public:
        /// <summary>A run of bytes of the file; none if size is 0.</summary>
        struct Bytes {
            const std::byte* data = nullptr;
            std::uint32_t    size = 0;
        };

        /// <summary>The largest LAME tag frame: MAXFRAMESIZE of VbrTag.c, the largest free-format frame (640 kbit/s at 32 kHz).</summary>
        static constexpr std::size_t MaxLameTagBytes = 2880;

        /// <summary>id3: the tags around the stream.</summary>
        explicit Mp3File(const Id3Bytes& id3) noexcept : id3_(id3) {}

        Mp3File(const Mp3File&)            = delete;
        Mp3File& operator=(const Mp3File&) = delete;

        /// <summary>Allocates the blocks of the stream; false if the memory is not available.</summary>
        bool Initialize(std::uint32_t blockCount, std::uint32_t blockSize) noexcept { return stream_.Initialize(blockCount, blockSize); }

        /// <summary>
        /// CTLAME_PROFILE: the time Encode and Write wait here counts as idle time of their nodes; null: not measured.
        /// Call it before the tasks start.
        /// </summary>
        void MeasureWaits(TaskProfile* producer, TaskProfile* consumer) noexcept { stream_.MeasureWaits(producer, consumer); }

        // --- Encode: the producer ---

        /// <summary>Appends bytes to the MP3 stream; waits while all blocks of the stream are in use.</summary>
        void Append(const std::byte* data, std::int32_t size) noexcept;

        /// <summary>MaxLameTagBytes of memory for the LAME tag frame; fill it before End.</summary>
        std::byte* LameTagBuffer() noexcept { return lameTag_.data(); }

        /// <summary>
        /// The stream is complete: the ID3v1 tag follows it, and the first lameTagSize bytes of LameTagBuffer replace the
        /// placeholder at the start of the stream (0: no LAME tag).
        /// </summary>
        void End(std::uint32_t lameTagSize) noexcept;

        /// <summary>The stream ends here because of an error: no ID3v1 tag, no LAME tag.</summary>
        void EndAfterError() noexcept;

        // --- Write: the consumer, in file order ---

        /// <summary>The ID3v2 tag in front of the stream; known from the start.</summary>
        Bytes Id3v2() const noexcept { return {id3_.v2, id3_.v2Size}; }

        /// <summary>The next block of the stream; waits for it. False once the stream has ended.</summary>
        bool AcquireBlock(Bytes& block) noexcept;

        /// <summary>Returns the block of the last AcquireBlock to the producer.</summary>
        void ReleaseBlock() noexcept { stream_.ReleaseReadBlock(); }

        /// <summary>The ID3v1 tag behind the stream; only after AcquireBlock has returned false. None after an error.</summary>
        Bytes Id3v1() const noexcept;

        /// <summary>
        /// The LAME tag frame, which belongs where the stream began; only after AcquireBlock has returned false. None after
        /// an error or without a LAME tag.
        /// </summary>
        Bytes LameTag() const noexcept;

    private:
        void EndStream(bool complete, std::uint32_t lameTagSize) noexcept;

        Id3Bytes  id3_;
        BlockPipe stream_;

        // producer side: the block being filled
        std::byte*    block_     = nullptr;
        std::uint32_t blockUsed_ = 0;

        // handed over with the end of the stream: written by the producer before the end mark, read by the consumer after it
        bool                                   complete_    = false;
        std::uint32_t                          lameTagSize_ = 0;
        std::array<std::byte, MaxLameTagBytes> lameTag_;

        // consumer side
        bool ended_ = false; // AcquireBlock has returned false
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
