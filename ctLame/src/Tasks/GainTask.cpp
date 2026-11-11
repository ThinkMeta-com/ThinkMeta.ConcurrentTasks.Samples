// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "GainTask.h"

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    void GainSignals::Publish(const SampleStream& stream, bool ended) noexcept
    {
        // WakeBatch new pieces of FRAMESIZE samples, or the end with the last pieces: a piece is only about 25 us of
        // work, a wake-up per piece would cost more than the analysis
        const std::uint64_t pieces = ended ? ~0ull : stream.GainAvailable() / FRAMESIZE;
        if (pieces < signaled_ + WakeBatch)
            return;
        signaled_ = pieces;
        for (std::int32_t i = 0; i < tasks_; ++i)
            wake_[i].Set();
    }

    void GainTask::Run() noexcept
    {
        // the progress for Release every few pieces: it decommits 64 KB at once anyway, and the progress shares its cache
        // line with the releaser
        constexpr std::int32_t ProgressBatch = 16;

        const ProfiledRun run(profile_);
        ChannelGain&      gain     = stream_.Gain()->Channel(channel_);
        std::uint64_t     position = SampleStream::GainStart();
        std::int32_t      pieces   = 0;
        for (;;) {
            const std::int32_t n = stream_.GainPiece(position);
            if (n < 0)
                break;
            if (n == 0) {
                stream_.GainProgress(channel_, position);
                // a Set before this wait leaves the event set, so no wake-up is lost
                WhileIdle(profile_, [&] { signals_.WaitForStream(channel_); });
                continue;
            }
            gain.Analyze(stream_.SamplesOf(channel_, position), static_cast<std::size_t>(n));
            position += static_cast<std::uint64_t>(n);
            if (++pieces == ProgressBatch) {
                pieces = 0;
                stream_.GainProgress(channel_, position);
            }
        }
        stream_.GainProgress(channel_, position);
        signals_.TaskDone();
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
