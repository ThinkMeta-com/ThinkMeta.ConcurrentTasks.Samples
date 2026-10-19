// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    /// <summary>
    /// What the CPU and the operating system support (CPUID, XGETBV): the base of the CPU check (CpuCheck) and of the
    /// SIMD level a sample chooses (its SimdFeatures). AVX, AVX2 and FMA count only if the operating system saves the
    /// YMM registers. SSE2 is part of x64.
    /// <para>Compiled like CpuCheck: plain SSE2, no whole program optimization; Detect runs before the C runtime.
    /// Preliminary.</para>
    /// </summary>
    struct CpuFeatures {
        bool sse3   = false;
        bool ssse3  = false;
        bool sse41  = false;
        bool sse42  = false;
        bool popcnt = false;
        bool pclmul = false;
        bool avx    = false;
        bool avx2   = false;
        bool fma    = false;
        bool f16c   = false;
        bool bmi1   = false;
        bool bmi2   = false;
        bool lzcnt  = false;
        bool movbe  = false;

        /// <summary>The features of this CPU.</summary>
        static CpuFeatures Detect() noexcept;

        /// <summary>
        /// The line of --features and --stats: "cpu SSE2 SSE4.1 AVX2 FMA, using avx2 (bit-identical)", with the SIMD
        /// instruction sets the samples choose from and the name of the level in use. Returns the length (cut off to the
        /// buffer).
        /// </summary>
        std::int32_t FormatLine(const wchar_t* levelName, wchar_t* text, std::int32_t size) const noexcept;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
