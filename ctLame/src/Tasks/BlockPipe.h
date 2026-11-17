// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#    include <Windows.h>
#endif

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "Profiling.h"

import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>
    /// A bounded pipe of fixed-size blocks between one producer task and one consumer task, which may run on
    /// different schedulers: the edge from Read to the encoder (the PCM pipe), and the stream inside Mp3File. Both
    /// sides work in the pipe's own memory. Unlike Memory::SpscVirtualMemoryPipe it holds back the producer when
    /// all blocks are full.
    ///
    /// Who waits: the consumer for the next committed block, the producer for a free block. Who wakes: the other
    /// side, the producer when it commits a block and the consumer when it releases one. The end: End, which the
    /// producer calls on every path, also after an error; the consumer then gets the blocks that are left and after
    /// them no more. The consumer releases every block it acquired, also after an error of its own, so the producer
    /// never waits for a pipe that nobody empties.
    ///
    /// How it is built: two counters (committed by the producer, released by the consumer, each moved by its side
    /// only), the end mark and two events with one waiter each. What a side writes lies on a cache line of its own,
    /// apart from what both sides only read. A side may acquire several blocks before it commits or releases them, in
    /// the order it acquired them.
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

        std::uint32_t BlockSize() const noexcept { return blockSize_; }

        /// <summary>
        /// CTLAME_PROFILE: the time each side waits here counts as idle time of its node; null: not measured. Call it
        /// before the tasks start.
        /// </summary>
        void MeasureWaits(TaskProfile* producer, TaskProfile* consumer) noexcept
        {
            producerIdle_ = IdleCounter(producer);
            consumerIdle_ = IdleCounter(consumer);
        }

        // --- producer ---

        /// <summary>The next free block; waits while all blocks are in use.</summary>
        std::byte* AcquireWriteBlock() noexcept;

        /// <summary>Hands the oldest acquired block with size bytes to the consumer.</summary>
        void CommitWriteBlock(std::uint32_t size) noexcept;

        /// <summary>No more blocks will be committed. Call it on every path.</summary>
        void End() noexcept;

        // --- consumer ---

        /// <summary>The next committed block; waits for it. False once the pipe has ended and all blocks are taken.</summary>
        bool AcquireReadBlock(ReadBlock& block) noexcept;

        /// <summary>Returns the oldest acquired block to the producer.</summary>
        void ReleaseReadBlock() noexcept;

    private:
        // read by both sides, written before the tasks start
        std::byte*                 memory_       = nullptr;
        std::size_t                memoryBytes_  = 0; // its size, for the release
        std::uint32_t*             sizes_        = nullptr;
        std::uint32_t              blockCount_   = 0;
        std::uint32_t              blockSize_    = 0;
        std::atomic<std::int64_t>* producerIdle_ = nullptr;
        std::atomic<std::int64_t>* consumerIdle_ = nullptr;

        // written by the producer
        alignas(64) std::atomic<std::int64_t> committed_{0};
        std::int64_t writeAcquired_ = 0;
        // written by the consumer
        alignas(64) std::atomic<std::int64_t> released_{0};
        std::int64_t readAcquired_ = 0;
        alignas(64) std::atomic<bool> ended_{false};

        Synchronization::AutoResetEvent_s dataAvailable_;  // one waiter: the consumer
        Synchronization::AutoResetEvent_s spaceAvailable_; // one waiter: the producer
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
