// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "PcmFeed.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    PcmFeed::PcmFeed(BlockPipe& pcm, SampleStream& stream, const PcmReader::Format& format) noexcept : pcm_(pcm), stream_(stream)
    {
        // a sample width read_samples_pcm cannot read yields no samples; LAME then still
        // encodes the flush frames
        (void)reader_.Initialize(format);
    }

    bool PcmFeed::Next(TaskProfile* profile) noexcept
    {
        if (!held_) {
            const bool available = WhileIdle(profile, [&] { return pcm_.AcquireReadBlock(block_); });
            if (!available) {
                // the end of lame_encoder_loop: the rest of the input and the flush padding
                reader_.Finish(stream_);
                stream_.Finish();
                return false;
            }
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

    void PcmFeed::Drain(TaskProfile* profile) noexcept
    {
        if (held_) {
            pcm_.ReleaseReadBlock();
            held_ = false;
        }
        BlockPipe::ReadBlock block{};
        for (;;) {
            if (!WhileIdle(profile, [&] { return pcm_.AcquireReadBlock(block); }))
                break;
            pcm_.ReleaseReadBlock();
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
