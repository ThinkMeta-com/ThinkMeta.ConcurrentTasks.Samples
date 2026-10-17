// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "SimdFeatures.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <initializer_list>

#include <immintrin.h>
#include <intrin.h>

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    CpuFeatures DetectCpuFeatures() noexcept
    {
        CpuFeatures        features;
        std::array<int, 4> info;
        __cpuid(info.data(), 0);
        const int maxLeaf = info[0];

        __cpuid(info.data(), 1);
        const bool sse41   = (info[2] & (1 << 19)) != 0;
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

        features.sse41 = sse41;
        features.avx2  = avx && avx2 && osAvx;
        features.fma   = fma && features.avx2;
        return features;
    }

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

    std::int32_t FormatSimdFeatures(const CpuFeatures& features, SimdLevel level, wchar_t* text, std::int32_t size) noexcept
    {
        const int length = swprintf_s(text, static_cast<std::size_t>(size), L"cpu%ls%ls%ls, using %ls (bit-identical)", features.sse41 ? L" SSE4.1" : L"",
                                      features.avx2 ? L" AVX2" : L"", features.fma ? L" FMA" : L"", SimdLevelName(level));
        return length > 0 ? length : 0;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
