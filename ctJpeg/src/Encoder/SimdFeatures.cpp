// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "SimdFeatures.h"

#include "Text.h"
#include <cstdint>
#include <initializer_list>

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    using Shared::Text;

    SimdLevel HighestSimdLevel(const CpuFeatures& features) noexcept
    {
        // Highway's AVX2 target requires FMA as well, and so does the avx2 feature set
        return features.avx2 && features.fma ? SimdLevel::Avx2 : SimdLevel::Sse2;
    }

    SimdLevel ChooseSimdLevel(SimdLevel requested, const CpuFeatures& features) noexcept
    {
        const SimdLevel highest = HighestSimdLevel(features);
        return static_cast<std::int32_t>(requested) < static_cast<std::int32_t>(highest) ? requested : highest;
    }

    const Shared::Char* SimdLevelName(SimdLevel level) noexcept
    {
        return level == SimdLevel::Avx2 ? CT_TEXT("avx2") : CT_TEXT("sse2");
    }

    bool ParseSimdLevel(const Shared::Char* name, SimdLevel& level) noexcept
    {
        for (const SimdLevel candidate : {SimdLevel::Sse2, SimdLevel::Avx2}) {
            if (Text::Equal(name, SimdLevelName(candidate))) {
                level = candidate;
                return true;
            }
        }
        return false;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
