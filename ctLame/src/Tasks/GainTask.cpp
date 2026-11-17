// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "GainTask.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    void GainTask::Run() noexcept
    {
        const ProfiledRun              run(profile_);
        SharedSampleStream::GainReader reader = stream_.OpenGainReader(channel_);
        ChannelGain&                   gain   = reader.Analysis();
        SharedSampleStream::Piece      piece;
        while (reader.NextPiece(piece))
            gain.Analyze(piece.samples, piece.count);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
