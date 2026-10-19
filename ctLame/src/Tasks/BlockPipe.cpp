// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "BlockPipe.h"

#include <cstddef>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    BlockPipe::~BlockPipe()
    {
        if (memory_ != nullptr)
            VirtualFree(memory_, 0, MEM_RELEASE);
        if (sizes_ != nullptr)
            HeapFree(GetProcessHeap(), 0, sizes_);
    }

    bool BlockPipe::Initialize(std::uint32_t blockCount, std::uint32_t blockSize) noexcept
    {
        const std::size_t total = static_cast<std::size_t>(blockCount) * blockSize;
        memory_                 = static_cast<std::byte*>(VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        sizes_                  = static_cast<std::uint32_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, blockCount * sizeof(std::uint32_t)));
        if (memory_ == nullptr || sizes_ == nullptr)
            return false;
        blockCount_ = blockCount;
        blockSize_  = blockSize;
        return true;
    }

    std::uint32_t BlockPipe::FilledBlocks() const noexcept
    {
        return static_cast<std::uint32_t>(committed_.load(std::memory_order_acquire) - released_.load(std::memory_order_acquire));
    }

    std::byte* BlockPipe::AcquireWriteBlock() noexcept
    {
        while (writeAcquired_ - released_.load(std::memory_order_acquire) >= blockCount_)
            spaceAvailable_.Wait();
        return memory_ + static_cast<std::size_t>(writeAcquired_++ % blockCount_) * blockSize_;
    }

    std::byte* BlockPipe::TryAcquireWriteBlock() noexcept
    {
        if (writeAcquired_ - released_.load(std::memory_order_acquire) >= blockCount_)
            return nullptr;
        return memory_ + static_cast<std::size_t>(writeAcquired_++ % blockCount_) * blockSize_;
    }

    void BlockPipe::CommitWriteBlock(std::uint32_t size) noexcept
    {
        const std::int64_t index    = committed_.load(std::memory_order_relaxed); // only the producer moves it
        sizes_[index % blockCount_] = size;
        committed_.store(index + 1, std::memory_order_release);
        dataAvailable_.Set();
    }

    void BlockPipe::SetEndOfStream() noexcept
    {
        endOfStream_.store(true, std::memory_order_release);
        dataAvailable_.Set();
    }

    bool BlockPipe::AcquireReadBlock(ReadBlock& block) noexcept
    {
        for (;;) {
            // read the end mark first: once it is set, every block has been committed before it
            const bool ended = endOfStream_.load(std::memory_order_acquire);
            if (committed_.load(std::memory_order_acquire) > readAcquired_)
                break;
            if (ended)
                return false;
            dataAvailable_.Wait();
        }
        const std::uint32_t slot = static_cast<std::uint32_t>(readAcquired_++ % blockCount_);
        block.data               = memory_ + static_cast<std::size_t>(slot) * blockSize_;
        block.size               = sizes_[slot];
        return true;
    }

    bool BlockPipe::TryAcquireReadBlock(ReadBlock& block, bool& ended) noexcept
    {
        ended = endOfStream_.load(std::memory_order_acquire);
        if (committed_.load(std::memory_order_acquire) <= readAcquired_)
            return false;
        ended                    = false;
        const std::uint32_t slot = static_cast<std::uint32_t>(readAcquired_++ % blockCount_);
        block.data               = memory_ + static_cast<std::size_t>(slot) * blockSize_;
        block.size               = sizes_[slot];
        return true;
    }

    void BlockPipe::ReleaseReadBlock() noexcept
    {
        released_.store(released_.load(std::memory_order_relaxed) + 1, std::memory_order_release); // only the consumer moves it
        spaceAvailable_.Set();
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
