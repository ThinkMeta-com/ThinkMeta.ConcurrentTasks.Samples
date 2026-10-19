// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Version.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    namespace {
        // LAME 4.0 is a release without patch level: no alpha/beta suffix, type character ' '.
        constexpr char Version[]           = "4.0";
        constexpr char TagEncoderVersion[] = "LAME4.0 ";

        static_assert(sizeof(TagEncoderVersion) - 1 <= 9, "the LAME tag has a fixed 9-byte encoder field");
    } // namespace

    const char* GetLameVersion() noexcept
    {
        return Version;
    }

    const char* GetLameShortVersion() noexcept
    {
        return Version;
    }

    const char* GetLameVeryShortVersion() noexcept
    {
        return TagEncoderVersion;
    }

    const char* GetLameTagEncoderShortVersion() noexcept
    {
        return TagEncoderVersion;
    }

    const char* GetLameUrl() noexcept
    {
        return "https://lame.sourceforge.io";
    }

    const char* GetLameOsBitness() noexcept
    {
        return sizeof(void*) == 8 ? "64bits" : "32bits";
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
