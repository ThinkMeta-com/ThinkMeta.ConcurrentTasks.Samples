// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Mp3File.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    void Mp3File::Append(const std::byte* data, std::int32_t size) noexcept
    {
        while (size > 0) {
            if (block_ == nullptr) {
                block_     = stream_.AcquireWriteBlock();
                blockUsed_ = 0;
            }
            const auto space = stream_.BlockSize() - blockUsed_;
            const auto n     = static_cast<std::uint32_t>(size) < space ? static_cast<std::uint32_t>(size) : space;
            std::memcpy(block_ + blockUsed_, data, n);
            blockUsed_ += n;
            data += n;
            size -= static_cast<std::int32_t>(n);
            if (blockUsed_ == stream_.BlockSize()) {
                stream_.CommitWriteBlock(blockUsed_);
                block_ = nullptr;
            }
        }
    }

    void Mp3File::End(std::uint32_t lameTagSize) noexcept
    {
        EndStream(true, lameTagSize <= MaxLameTagBytes ? lameTagSize : 0);
    }

    void Mp3File::EndAfterError() noexcept
    {
        EndStream(false, 0);
    }

    void Mp3File::EndStream(bool complete, std::uint32_t lameTagSize) noexcept
    {
        if (block_ != nullptr && blockUsed_ > 0) {
            stream_.CommitWriteBlock(blockUsed_);
            block_ = nullptr;
        }

        // first what the consumer reads after the end, then the end mark
        complete_    = complete;
        lameTagSize_ = lameTagSize;
        stream_.End();
    }

    bool Mp3File::AcquireBlock(Bytes& block) noexcept
    {
        BlockPipe::ReadBlock next{};
        if (!stream_.AcquireReadBlock(next)) {
            ended_ = true;
            return false;
        }
        block = {next.data, next.size};
        return true;
    }

    Mp3File::Bytes Mp3File::Id3v1() const noexcept
    {
        assert(ended_);
        return complete_ ? Bytes{id3_.v1, id3_.v1Size} : Bytes{};
    }

    Mp3File::Bytes Mp3File::LameTag() const noexcept
    {
        assert(ended_);
        return complete_ ? Bytes{lameTag_.data(), lameTagSize_} : Bytes{};
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
