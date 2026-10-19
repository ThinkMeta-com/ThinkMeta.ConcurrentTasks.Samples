// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "FrameEncoder.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    FrameEncoder::FrameEncoder(BlockPipe& mp3, Encoder& encoder, Mp3WriterTask& writer, TaskProfile* profile) noexcept
        : mp3_(mp3),
          encoder_(encoder),
          writer_(writer),
          profile_(profile)
    {}

    void FrameEncoder::Begin() noexcept
    {
        Write(buffer_.data(), encoder_.TakeHeaderBytes(buffer_.data(), sizeof(buffer_)));
    }

    void FrameEncoder::EncodeFrame(FrameData& frame) noexcept
    {
        if (error_ != 0)
            return;
        if (!encoder_.Encode(frame)) {
            error_ = -2;
            return;
        }
        if (frame.outputSize < 0 || frame.outputSize > MaxFrameBytes) {
            error_ = -1;
            return;
        }
        Write(frame.output.data(), frame.outputSize);
    }

    void FrameEncoder::Write(const std::byte* data, std::int32_t size) noexcept
    {
        while (size > 0) {
            if (block_ == nullptr) {
                block_     = WhileIdle(profile_, [&] { return mp3_.AcquireWriteBlock(); });
                blockUsed_ = 0;
            }
            const auto space = mp3_.BlockSize() - blockUsed_;
            const auto n     = static_cast<std::uint32_t>(size) < space ? static_cast<std::uint32_t>(size) : space;
            std::memcpy(block_ + blockUsed_, data, n);
            blockUsed_ += n;
            data += n;
            size -= static_cast<std::int32_t>(n);
            if (blockUsed_ == mp3_.BlockSize()) {
                mp3_.CommitWriteBlock(blockUsed_);
                block_ = nullptr;
            }
        }
    }

    void FrameEncoder::Finish(std::int32_t encoderPadding, std::int32_t error) noexcept
    {
        if (error_ == 0)
            error_ = error;
        if (error_ == 0) {
            encoder_.SetEncoderPadding(encoderPadding);
            const std::int32_t bytes = encoder_.Flush(buffer_.data(), sizeof(buffer_));
            if (bytes < 0)
                error_ = bytes;
            else
                Write(buffer_.data(), bytes);
        }
        if (block_ != nullptr && blockUsed_ > 0) {
            mp3_.CommitWriteBlock(blockUsed_);
            block_ = nullptr;
        }

        if (error_ == 0) {
            const std::size_t size = encoder_.GetLameTagFrame(tag_.data(), sizeof(tag_));
            if (size > 0 && size <= sizeof(tag_))
                writer_.SetFinalHeader(tag_.data(), static_cast<std::uint32_t>(size));
        }
        mp3_.SetEndOfStream();
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
