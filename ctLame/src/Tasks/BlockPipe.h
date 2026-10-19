// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    /// <summary>
    /// A bounded pipe of fixed-size blocks between one producer task and one consumer task, which
    /// may run on different schedulers. Unlike Memory::SpscVirtualMemoryPipe it holds back the
    /// producer when all blocks are full, and its fill level drives the I/O priorities.
    ///
    /// Both sides may hold several blocks at once (for overlapped I/O into or out of the pipe),
    /// but they commit and release them in the order they acquired them.
    ///
    /// How it is built: two counters (committed by the producer, released by the consumer, each moved by
    /// its side only, std::atomic on its own cache line) and two events with one waiter each. The library's
    /// StreamedFileReader and OrderedFileWriter are not used: the reader starts behind the WAV header the
    /// frontend has parsed and stops at the end of the data chunk, the writer writes the LAME tag back to
    /// offset 0 after the last block, and the fill level of the pipe sets the I/O priorities (IoPriority.h).
    /// </summary>
    class BlockPipe
    {
    public:
        struct ReadBlock {
            const std::byte* data;
            std::uint32_t    size;
        };

        BlockPipe() noexcept = default;
        ~BlockPipe();

        BlockPipe(const BlockPipe&)            = delete;
        BlockPipe& operator=(const BlockPipe&) = delete;

        /// <summary>Allocates the blocks; false if the memory is not available.</summary>
        bool Initialize(std::uint32_t blockCount, std::uint32_t blockSize) noexcept;

        std::uint32_t BlockCount() const noexcept { return blockCount_; }
        std::uint32_t BlockSize() const noexcept { return blockSize_; }

        /// <summary>Committed blocks the consumer has not released yet.</summary>
        std::uint32_t FilledBlocks() const noexcept;

        // --- producer ---

        /// <summary>The next free block; waits while all blocks are in use.</summary>
        std::byte* AcquireWriteBlock() noexcept;

        /// <summary>The next free block, or nullptr if all blocks are in use.</summary>
        std::byte* TryAcquireWriteBlock() noexcept;

        /// <summary>Hands the oldest acquired block with size bytes to the consumer.</summary>
        void CommitWriteBlock(std::uint32_t size) noexcept;

        /// <summary>No more blocks will be committed.</summary>
        void SetEndOfStream() noexcept;

        // --- consumer ---

        /// <summary>The next committed block; waits for it. False once the stream has ended.</summary>
        bool AcquireReadBlock(ReadBlock& block) noexcept;

        /// <summary>
        /// The next committed block without waiting. False if there is none yet, or, with ended
        /// set, if the stream has ended.
        /// </summary>
        bool TryAcquireReadBlock(ReadBlock& block, bool& ended) noexcept;

        /// <summary>Returns the oldest acquired block to the producer.</summary>
        void ReleaseReadBlock() noexcept;

    private:
        std::byte*     memory_     = nullptr;
        std::uint32_t* sizes_      = nullptr;
        std::uint32_t  blockCount_ = 0;
        std::uint32_t  blockSize_  = 0;

        // producer side
        std::int64_t writeAcquired_ = 0;
        alignas(64) std::atomic<std::int64_t> committed_{0};
        // consumer side
        std::int64_t readAcquired_ = 0;
        alignas(64) std::atomic<std::int64_t> released_{0};
        alignas(64) std::atomic<bool> endOfStream_{false};

        Synchronization::AutoResetEvent_s dataAvailable_;  // one waiter: the consumer
        Synchronization::AutoResetEvent_s spaceAvailable_; // one waiter: the producer
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
