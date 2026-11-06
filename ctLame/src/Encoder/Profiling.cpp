// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Profiling.h"

#if defined(_WIN32)
#    include <Windows.h>
#endif

#include <array>
#include <cstdint>
#include <ctime>

#include "TestEnvironment.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    const Shared::Char* ProfileStageName(ProfileStage stage) noexcept
    {
        switch (stage) {
            case ProfileStage::Input: return CT_TEXT("input");
            case ProfileStage::Psy: return CT_TEXT("psy-prep");
            case ProfileStage::PsyResolve: return CT_TEXT("psy-res");
            case ProfileStage::Mdct: return CT_TEXT("mdct");
            case ProfileStage::Stereo: return CT_TEXT("stereo");
            case ProfileStage::Search: return CT_TEXT("q-search");
            case ProfileStage::Quantize: return CT_TEXT("q-commit");
            case ProfileStage::Bitstream: return CT_TEXT("bitstream");
            default: return CT_TEXT("?");
        }
    }

    std::int64_t ProfileNow() noexcept
    {
#if defined(_WIN32)
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return now.QuadPart;
#else
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        return static_cast<std::int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
#endif
    }

    std::int64_t ProfileTicksPerSecond() noexcept
    {
#if defined(_WIN32)
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return frequency.QuadPart;
#else
        return 1000000000; // nanoseconds
#endif
    }

    bool ProfileRequested() noexcept
    {
        std::array<Shared::Char, 8> value;
        if (!Shared::TestEnvironment::Get(CT_TEXT("CTLAME_PROFILE"), value.data(), static_cast<std::uint32_t>(value.size())))
            return false;
        return !(value[0] == '0' && value[1] == 0);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
