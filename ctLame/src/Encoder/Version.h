// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    // Port of libmp3lame/version.{h,c}. ctLame reports the LAME version it is bit-identical to,
    // because the version also goes into every stream: the LAME tag carries the encoder string.

    inline constexpr std::int32_t LameMajorVersion = 4;
    inline constexpr std::int32_t LameMinorVersion = 0;
    inline constexpr std::int32_t PsyMajorVersion  = 1;
    inline constexpr std::int32_t PsyMinorVersion  = 0;

    /// <summary>Version for screen reports (get_lame_version).</summary>
    const char* GetLameVersion() noexcept;

    /// <summary>Short version (get_lame_short_version).</summary>
    const char* GetLameShortVersion() noexcept;

    /// <summary>Very short version (get_lame_very_short_version).</summary>
    const char* GetLameVeryShortVersion() noexcept;

    /// <summary>
    /// Encoder string of the LAME tag (get_lame_tag_encoder_short_version). It fills a fixed
    /// 9-byte field of the tag, so it must never grow beyond 9 characters.
    /// </summary>
    const char* GetLameTagEncoderShortVersion() noexcept;

    /// <summary>Web site of LAME (get_lame_url).</summary>
    const char* GetLameUrl() noexcept;

    /// <summary>"32bits" or "64bits" (get_lame_os_bitness).</summary>
    const char* GetLameOsBitness() noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
