// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "PcmFeed.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    PcmFeed::PcmFeed(BlockPipe& pcm, SharedSampleStream& stream, const PcmReader::Format& format) noexcept : pcm_(pcm), stream_(stream)
    {
        // a sample width read_samples_pcm cannot read yields no samples; LAME then still
        // encodes the flush frames
        (void)reader_.Initialize(format);
    }

    bool PcmFeed::Next() noexcept
    {
        if (!held_) {
            if (!pcm_.AcquireReadBlock(block_))
                return false;
            held_   = true;
            offset_ = 0;
        }
        const std::uint32_t rest  = block_.size - offset_;
        const std::uint32_t bytes = rest < StepSize ? rest : StepSize;
        reader_.Push(block_.data + offset_, bytes, stream_);
        offset_ += bytes;
        if (offset_ == block_.size) {
            pcm_.ReleaseReadBlock();
            held_ = false;
        }
        return true;
    }

    void PcmFeed::End() noexcept
    {
        reader_.Finish(stream_);
        stream_.End();
    }

    void PcmFeed::Drain() noexcept
    {
        if (held_) {
            pcm_.ReleaseReadBlock();
            held_ = false;
        }
        BlockPipe::ReadBlock block{};
        while (pcm_.AcquireReadBlock(block))
            pcm_.ReleaseReadBlock();
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
