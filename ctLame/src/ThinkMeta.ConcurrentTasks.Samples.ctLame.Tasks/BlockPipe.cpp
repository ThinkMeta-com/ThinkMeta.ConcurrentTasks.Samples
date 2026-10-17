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
        return static_cast<std::uint32_t>(ReadAcquire64(&committed_) - ReadAcquire64(&released_));
    }

    std::byte* BlockPipe::AcquireWriteBlock() noexcept
    {
        while (writeAcquired_ - ReadAcquire64(&released_) >= blockCount_)
            spaceAvailable_.Wait();
        return memory_ + static_cast<std::size_t>(writeAcquired_++ % blockCount_) * blockSize_;
    }

    std::byte* BlockPipe::TryAcquireWriteBlock() noexcept
    {
        if (writeAcquired_ - ReadAcquire64(&released_) >= blockCount_)
            return nullptr;
        return memory_ + static_cast<std::size_t>(writeAcquired_++ % blockCount_) * blockSize_;
    }

    void BlockPipe::CommitWriteBlock(std::uint32_t size) noexcept
    {
        const std::int64_t index    = committed_;
        sizes_[index % blockCount_] = size;
        WriteRelease64(&committed_, index + 1);
        dataAvailable_.Set();
    }

    void BlockPipe::SetEndOfStream() noexcept
    {
        WriteRelease(&endOfStream_, 1);
        dataAvailable_.Set();
    }

    bool BlockPipe::AcquireReadBlock(ReadBlock& block) noexcept
    {
        for (;;) {
            // read the end mark first: once it is set, every block has been committed before it
            const bool ended = ReadAcquire(&endOfStream_) != 0;
            if (ReadAcquire64(&committed_) > readAcquired_)
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
        ended = ReadAcquire(&endOfStream_) != 0;
        if (ReadAcquire64(&committed_) <= readAcquired_)
            return false;
        ended                    = false;
        const std::uint32_t slot = static_cast<std::uint32_t>(readAcquired_++ % blockCount_);
        block.data               = memory_ + static_cast<std::size_t>(slot) * blockSize_;
        block.size               = sizes_[slot];
        return true;
    }

    void BlockPipe::ReleaseReadBlock() noexcept
    {
        WriteRelease64(&released_, released_ + 1);
        spaceAvailable_.Set();
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
