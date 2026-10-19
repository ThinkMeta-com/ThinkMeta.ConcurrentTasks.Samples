// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Profiling.h"

#include <Windows.h>

#include <array>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    const wchar_t* ProfileStageName(ProfileStage stage) noexcept
    {
        switch (stage) {
            case ProfileStage::Input: return L"input";
            case ProfileStage::Psy: return L"psy-prep";
            case ProfileStage::PsyResolve: return L"psy-res";
            case ProfileStage::Mdct: return L"mdct";
            case ProfileStage::Stereo: return L"stereo";
            case ProfileStage::Search: return L"q-search";
            case ProfileStage::Quantize: return L"q-commit";
            case ProfileStage::Bitstream: return L"bitstream";
            default: return L"?";
        }
    }

    std::int64_t ProfileNow() noexcept
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }

    std::int64_t ProfileTicksPerSecond() noexcept
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return frequency.QuadPart;
    }

    bool ProfileRequested() noexcept
    {
        std::array<wchar_t, 8> value;
        const std::uint32_t    length = GetEnvironmentVariableW(L"CTLAME_PROFILE", value.data(), static_cast<unsigned long>(value.size()));
        return length > 0 && length < 8 && !(length == 1 && value[0] == L'0');
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
