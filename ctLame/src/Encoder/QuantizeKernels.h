// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <cstdint>

#include "SimdFeatures.h"
#include "Types.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    // The loops of the quantizer in a scalar and SIMD versions with the same bits
    // (QuantizeSimd.inc). The scalar versions are in Quantize.cpp and VbrQuantize.cpp. The AVX2
    // level uses 8 lanes where it is faster (xrpow, VecMax, SfbNoise with small quantized values
    // looked up in registers); QuantizeLines uses its SSE4.1 version.

    /// <summary>
    /// init_xrpow_core_sse: xrpow[i] = |xr[i]|^(3/4) as sqrt(x * sqrt(x)) for count lines, the sum
    /// of |xr| in LAME's four lanes (line i in lane i % 4, combined at the end) and the maximum.
    /// </summary>
    using XrpowFunction = void (*)(const float* xr, float* xrpow, std::int32_t count, float& sum, float& max) noexcept;

    void XrpowScalar(const float* xr, float* xrpow, std::int32_t count, float& sum, float& max) noexcept;
    void XrpowSse41(const float* xr, float* xrpow, std::int32_t count, float& sum, float& max) noexcept;
    void XrpowAvx2(const float* xr, float* xrpow, std::int32_t count, float& sum, float& max) noexcept;

    /// <summary>
    /// The band loop of quantize_x34: l3[i] for count lines at step34 (k_34_4: truncate, add
    /// adj43, truncate).
    /// </summary>
    using QuantizeLinesFunction = void (*)(const float* xr34, std::int32_t* l3, std::int32_t count, float step34, const float* adj43) noexcept;

    void QuantizeLinesScalar(const float* xr34, std::int32_t* l3, std::int32_t count, float step34, const float* adj43) noexcept;
    void QuantizeLinesSse41(const float* xr34, std::int32_t* l3, std::int32_t count, float step34, const float* adj43) noexcept;

    /// <summary>
    /// calc_sfb_noise_x34: the quantization noise of a band of bw lines at step = 2^(sf/4),
    /// step34 = step^(-3/4); the noise of each quad (d0^2 + d1^2) + (d2^2 + d3^2) added in order.
    /// </summary>
    using SfbNoiseFunction
        = float (*)(const float* xr, const float* xr34, std::int32_t bw, float step, float step34, const float* pow43, const float* adj43) noexcept;

    float SfbNoiseScalar(const float* xr, const float* xr34, std::int32_t bw, float step, float step34, const float* pow43, const float* adj43) noexcept;
    float SfbNoiseSse41(const float* xr, const float* xr34, std::int32_t bw, float step, float step34, const float* pow43, const float* adj43) noexcept;
    float SfbNoiseAvx2(const float* xr, const float* xr34, std::int32_t bw, float step, float step34, const float* pow43, const float* adj43) noexcept;

    /// <summary>The largest value of a band of xr34 (all >= 0).</summary>
    using VecMaxFunction = float (*)(const float* xr34, std::int32_t bw) noexcept;

    float VecMaxScalar(const float* xr34, std::int32_t bw) noexcept;
    float VecMaxSse41(const float* xr34, std::int32_t bw) noexcept;
    float VecMaxAvx2(const float* xr34, std::int32_t bw) noexcept;

    /// <summary>The quantizer's kernels of one SIMD level.</summary>
    struct QuantizeKernels {
        XrpowFunction         xrpow;
        QuantizeLinesFunction quantizeLines;
        SfbNoiseFunction      sfbNoise;
        VecMaxFunction        vecMax;
    };

    /// <summary>The kernels of a SIMD level.</summary>
    QuantizeKernels QuantizeKernelsFor(SimdLevel level) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
