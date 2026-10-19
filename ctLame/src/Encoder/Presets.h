// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    struct EncoderOptions;

    /// <summary>
    /// Port of apply_preset (presets.c): maps the legacy presets to V0..V9 and applies the VBR
    /// preset table, interpolated by the fractional part of -V. With enforce = 0 only values the
    /// user left at their default are set. Returns the preset that was applied.
    /// </summary>
    std::int32_t ApplyPreset(EncoderOptions& options, std::int32_t preset, std::int32_t enforce) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
