// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include "SimdFeatures.h"
#include "Types.h"

// The transform kernels of the MDCT: the polyphase analysis filterbank (32 subband samples
// from 512 PCM samples) and the long and short MDCTs. They compute exactly what LAME's
// window_subband, mdct_long and mdct_short compute: every operation keeps LAME's order and
// precision, including the places where a double constant turns part of an expression into
// double arithmetic before the result is rounded to float. MdctGoldenTests checks the bits.

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame::MdctKernels {

    /// <summary>
    /// Windowing and matrixing of the analysis filterbank: 32 subband samples from the PCM
    /// samples around x1. enwindow is LAME's analysis window table.
    /// </summary>
    void WindowSubband(const float* enwindow, const sample_t* x1, float* a) noexcept;

    /// <summary>
    /// The part of WindowSubband after its 15 pairs of windowed sums (a[0..13], a[16..29]): the
    /// middle subbands and the 32 point DCT. The SIMD versions compute the pairs, then call it.
    /// </summary>
    void WindowSubbandTail(const float* enwindow, const sample_t* x1, float* a) noexcept;

    /// <summary>WindowSubband with the 15 pairs in SIMD lanes (MdctSimd.inc), bit-identical.</summary>
    using WindowSubbandFunction = void (*)(const float* enwindow, const sample_t* x1, float* a) noexcept;

    void WindowSubbandSse41(const float* enwindow, const sample_t* x1, float* a) noexcept;
    void WindowSubbandAvx2(const float* enwindow, const sample_t* x1, float* a) noexcept;

    /// <summary>The window kernel of a SIMD level.</summary>
    WindowSubbandFunction WindowSubbandFor(SimdLevel level) noexcept;

    /// <summary>Three short MDCTs (6 lines each) in place; inout holds 18 interleaved values.</summary>
    void MdctShort(const float* shortWindow, float* inout) noexcept;

    /// <summary>The long MDCT: 18 lines from 18 folded input values. cx are LAME's cosine factors.</summary>
    void MdctLong(const float* cx, float* out, const float* in) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame::MdctKernels
