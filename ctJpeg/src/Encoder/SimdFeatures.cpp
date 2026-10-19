// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "SimdFeatures.h"

#include <array>
#include <cstdint>
#include <cwchar>
#include <initializer_list>

#include <immintrin.h>
#include <intrin.h>

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    CpuFeatures DetectCpuFeatures() noexcept
    {
        CpuFeatures        features;
        std::array<int, 4> info;
        __cpuid(info.data(), 0);
        const int maxLeaf = info[0];

        __cpuid(info.data(), 1);
        const bool fma     = (info[2] & (1 << 12)) != 0;
        const bool osxsave = (info[2] & (1 << 27)) != 0;
        const bool avx     = (info[2] & (1 << 28)) != 0;

        // the OS saves the XMM and YMM registers on a context switch
        const bool osAvx = osxsave && (_xgetbv(0) & 0x6) == 0x6;

        bool avx2 = false;
        if (maxLeaf >= 7) {
            __cpuidex(info.data(), 7, 0);
            avx2 = (info[1] & (1 << 5)) != 0;
        }

        // Highway's AVX2 target requires FMA as well, and so does the avx2 feature set
        features.avx2 = avx && avx2 && fma && osAvx;
        return features;
    }

    SimdLevel HighestSimdLevel(const CpuFeatures& features) noexcept
    {
        return features.avx2 ? SimdLevel::Avx2 : SimdLevel::Sse2;
    }

    SimdLevel ChooseSimdLevel(SimdLevel requested, const CpuFeatures& features) noexcept
    {
        const SimdLevel highest = HighestSimdLevel(features);
        return static_cast<std::int32_t>(requested) < static_cast<std::int32_t>(highest) ? requested : highest;
    }

    const wchar_t* SimdLevelName(SimdLevel level) noexcept
    {
        return level == SimdLevel::Avx2 ? L"avx2" : L"sse2";
    }

    bool ParseSimdLevel(const wchar_t* name, SimdLevel& level) noexcept
    {
        for (const SimdLevel candidate : {SimdLevel::Sse2, SimdLevel::Avx2}) {
            if (std::wcscmp(name, SimdLevelName(candidate)) == 0) {
                level = candidate;
                return true;
            }
        }
        return false;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
