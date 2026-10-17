// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    struct SessionConfig;

    /// <summary>Port of lame_print_config: version and filter settings, to the message report.</summary>
    void PrintConfig(const SessionConfig& config);

    /// <summary>Port of lame_print_internals: the --verbose details, to the message report.</summary>
    void PrintInternals(const SessionConfig& config);

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
