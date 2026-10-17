// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#pragma once

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    // Feature sets: jpegli computes differently per Highway target, so every level
    // is bit-identical to jpegli on its own target, not to the other levels. sse2 matches jpegli on
    // SSE2, SSSE3 and SSE4 (identical output), avx2 matches jpegli on AVX2 (with FMA). The level is
    // chosen once at start: the highest one the CPU supports, or lower on request (--simd).

    /// <summary>The feature sets of the bit-identical implementation, lowest first.</summary>
    enum class SimdLevel : std::int32_t { Sse2, Avx2 };

    /// <summary>What the CPU and the operating system support.</summary>
    struct CpuFeatures {
        bool avx2 = false; // includes FMA and the OS saving the AVX registers
    };

    /// <summary>The features of this CPU (CPUID, XGETBV).</summary>
    CpuFeatures DetectCpuFeatures() noexcept;

    /// <summary>The highest level the features allow; SSE2 is part of x64.</summary>
    SimdLevel HighestSimdLevel(const CpuFeatures& features) noexcept;

    /// <summary>The level to use: at most the requested one, at most the highest available one.</summary>
    SimdLevel ChooseSimdLevel(SimdLevel requested, const CpuFeatures& features) noexcept;

    /// <summary>Name of a level, as --simd takes it: sse2, avx2.</summary>
    const wchar_t* SimdLevelName(SimdLevel level) noexcept;

    /// <summary>The level of a name as --simd takes it; false for an unknown name.</summary>
    bool ParseSimdLevel(const wchar_t* name, SimdLevel& level) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
