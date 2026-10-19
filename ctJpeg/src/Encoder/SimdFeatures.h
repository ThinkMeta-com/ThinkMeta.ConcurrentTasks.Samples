// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstdint>

#include "CpuFeatures.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    // Feature sets: jpegli computes differently per Highway target, so every level
    // is bit-identical to jpegli on its own target, not to the other levels. sse2 matches jpegli on
    // SSE2, SSSE3 and SSE4 (identical output), avx2 matches jpegli on AVX2 (with FMA). The level is
    // chosen once at start: the highest one the CPU supports, or lower on request (--simd).

    /// <summary>The feature sets of the bit-identical implementation, lowest first.</summary>
    enum class SimdLevel : std::int32_t { Sse2, Avx2 };

    /// <summary>What the CPU and the operating system support: Shared::CpuFeatures::Detect().</summary>
    using Shared::CpuFeatures;

    /// <summary>The highest level the features allow: avx2 needs AVX2 and FMA (as Highway's AVX2 target); SSE2 is part of
    /// x64.</summary>
    SimdLevel HighestSimdLevel(const CpuFeatures& features) noexcept;

    /// <summary>The level to use: at most the requested one, at most the highest available one.</summary>
    SimdLevel ChooseSimdLevel(SimdLevel requested, const CpuFeatures& features) noexcept;

    /// <summary>Name of a level, as --simd takes it: sse2, avx2.</summary>
    const wchar_t* SimdLevelName(SimdLevel level) noexcept;

    /// <summary>The level of a name as --simd takes it; false for an unknown name.</summary>
    bool ParseSimdLevel(const wchar_t* name, SimdLevel& level) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
