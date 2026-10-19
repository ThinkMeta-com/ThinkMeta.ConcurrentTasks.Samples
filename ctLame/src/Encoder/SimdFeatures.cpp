// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "SimdFeatures.h"

#include <cstdint>
#include <cwchar>
#include <initializer_list>

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    SimdLevel HighestSimdLevel(const CpuFeatures& features) noexcept
    {
        if (features.avx2 && features.sse41)
            return SimdLevel::Avx2;
        if (features.sse41)
            return SimdLevel::Sse41;
        return SimdLevel::Scalar;
    }

    SimdLevel ChooseSimdLevel(SimdLevel requested, const CpuFeatures& features) noexcept
    {
        const SimdLevel highest = HighestSimdLevel(features);
        return static_cast<std::int32_t>(requested) < static_cast<std::int32_t>(highest) ? requested : highest;
    }

    const wchar_t* SimdLevelName(SimdLevel level) noexcept
    {
        switch (level) {
            case SimdLevel::Sse41: return L"sse4.1";
            case SimdLevel::Avx2: return L"avx2";
            default: return L"scalar";
        }
    }

    bool ParseSimdLevel(const wchar_t* name, SimdLevel& level) noexcept
    {
        for (const SimdLevel candidate : {SimdLevel::Scalar, SimdLevel::Sse41, SimdLevel::Avx2}) {
            if (std::wcscmp(name, SimdLevelName(candidate)) == 0) {
                level = candidate;
                return true;
            }
        }
        return false;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
