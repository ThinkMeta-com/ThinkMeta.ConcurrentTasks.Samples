// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <cstddef>

#include "SimdFeatures.h"
#include "Types.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    // The input part (FIR) of the Yule-Walker filter of the ReplayGain analysis (filterYule in gain_analysis.c) in one
    // scalar and two SIMD versions. It needs only the input, so each output sample can be computed on its own; each
    // SIMD lane computes LAME's expression for one sample in LAME's order, so all three give the same bits. The
    // feedback and the Butterworth filter stay serial (ChannelGain::Analyze).

    /// <summary>s1 of filterYule for count samples: input[-10 .. count - 1] must be valid, kernel is ABYule's row as Float_t.</summary>
    using GainFirFunction = void (*)(const sample_t* input, sample_t* s1, std::size_t count, const float* kernel) noexcept;

    void GainYuleFirScalar(const sample_t* input, sample_t* s1, std::size_t count, const float* kernel) noexcept;
    void GainYuleFirSse41(const sample_t* input, sample_t* s1, std::size_t count, const float* kernel) noexcept;
    void GainYuleFirAvx2(const sample_t* input, sample_t* s1, std::size_t count, const float* kernel) noexcept;

    /// <summary>The function of a SIMD level.</summary>
    GainFirFunction GainYuleFirFor(SimdLevel level) noexcept;

    // LAME's expressions for one sample, shared by the scalar code and the tails of the SIMD functions

    /// <summary>s1 of filterYule for the sample at p (p[-10 .. 0]).</summary>
    inline sample_t YuleFir(const sample_t* p, const float* kernel) noexcept
    {
        const sample_t y0  = p[-10] * kernel[0];
        const sample_t y2  = p[-9] * kernel[1];
        const sample_t y4  = p[-8] * kernel[2];
        const sample_t y6  = p[-7] * kernel[3];
        const sample_t s00 = y0 + y2 + y4 + y6;
        const sample_t y8  = p[-6] * kernel[4];
        const sample_t yA  = p[-5] * kernel[5];
        const sample_t yC  = p[-4] * kernel[6];
        const sample_t yE  = p[-3] * kernel[7];
        const sample_t s01 = y8 + yA + yC + yE;
        const sample_t yG  = p[-2] * kernel[8] + p[-1] * kernel[9];
        const sample_t yK  = p[0] * kernel[10];
        return s00 + s01 + yG + yK;
    }

    /// <summary>s1 of filterButter for the sample at p (p[-2 .. 0]).</summary>
    inline sample_t ButterFir(const sample_t* p, const float* kernel) noexcept
    {
        return p[-2] * kernel[0] + p[-1] * kernel[2] + p[0] * kernel[4];
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
