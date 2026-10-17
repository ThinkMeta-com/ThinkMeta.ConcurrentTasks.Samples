// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    // SIMD in ctLame: the reference implementation has SIMD paths that give
    // the same bits as its scalar code. The level is chosen once at start: the highest one the
    // CPU supports, or lower on request (--simd).

    /// <summary>The SIMD levels of the bit-identical implementation, lowest first.</summary>
    enum class SimdLevel : std::int32_t { Scalar, Sse41, Avx2 };

    /// <summary>What the CPU and the operating system support.</summary>
    struct CpuFeatures {
        bool sse41 = false;
        bool avx2  = false; // includes the OS saving the AVX registers
        bool fma   = false;
    };

    /// <summary>The features of this CPU (CPUID, XGETBV).</summary>
    CpuFeatures DetectCpuFeatures() noexcept;

    /// <summary>The highest level the features allow.</summary>
    SimdLevel HighestSimdLevel(const CpuFeatures& features) noexcept;

    /// <summary>The level to use: at most the requested one, at most the highest available one.</summary>
    SimdLevel ChooseSimdLevel(SimdLevel requested, const CpuFeatures& features) noexcept;

    /// <summary>Name of a level, as --simd takes it: scalar, sse4.1, avx2.</summary>
    const wchar_t* SimdLevelName(SimdLevel level) noexcept;

    /// <summary>The level of a name as --simd takes it; false for an unknown name.</summary>
    bool ParseSimdLevel(const wchar_t* name, SimdLevel& level) noexcept;

    /// <summary>
    /// The feature line (--features, CTLAME_PROFILE): "cpu SSE4.1 AVX2 FMA, using avx2
    /// (bit-identical)". Returns the length.
    /// </summary>
    std::int32_t FormatSimdFeatures(const CpuFeatures& features, SimdLevel level, wchar_t* text, std::int32_t size) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
