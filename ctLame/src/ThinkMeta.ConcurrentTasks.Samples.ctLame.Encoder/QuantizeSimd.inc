// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

// The SIMD loops of the quantizer, included by QuantizeSse41.cpp and QuantizeAvx2.cpp, which set
// QUANTIZE_SIMD_SUFFIX (Sse41 or Avx2) and, for AVX2, QUANTIZE_SIMD_AVX2. Everything here but the
// kernels has internal linkage, so the AVX2 file's code cannot be merged into other translation
// units.

#define QUANTIZE_SIMD_JOIN2(name, suffix) name##suffix
#define QUANTIZE_SIMD_JOIN(name, suffix)  QUANTIZE_SIMD_JOIN2(name, suffix)
#define QUANTIZE_SIMD_NAME(name)          QUANTIZE_SIMD_JOIN(name, QUANTIZE_SIMD_SUFFIX)

namespace {

    __m128 Abs4(__m128 v) noexcept
    {
        return _mm_and_ps(v, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)));
    }

    // |x|^(3/4) as init_xrpow_core_sse computes it
    __m128 Pow34(__m128 x) noexcept
    {
        return _mm_sqrt_ps(_mm_mul_ps(x, _mm_sqrt_ps(x)));
    }

    // table[l[0..3]]: the indices as two 64-bit halves and four scalar loads (fewer shuffles
    // than four extracts, and much faster than vgatherdps on the measuring CPU)
    __m128 Lookup4(const float* table, __m128i l) noexcept
    {
        const std::int64_t lo = _mm_cvtsi128_si64(l);
        const std::int64_t hi = _mm_extract_epi64(l, 1);
        const __m128       a  = _mm_unpacklo_ps(_mm_load_ss(table + static_cast<std::int32_t>(lo)), _mm_load_ss(table + (lo >> 32)));
        const __m128       b  = _mm_unpacklo_ps(_mm_load_ss(table + static_cast<std::int32_t>(hi)), _mm_load_ss(table + (hi >> 32)));
        return _mm_movelh_ps(a, b);
    }

    // k_34_4 on four lanes: truncate, add adj43, truncate
    __m128i Quantize4(__m128 x, const float* adj43) noexcept
    {
        const __m128i l = _mm_cvttps_epi32(x);
        return _mm_cvttps_epi32(_mm_add_ps(x, Lookup4(adj43, l)));
    }

    // the noise of one quad, (d0*d0 + d1*d1) + (d2*d2 + d3*d3), in lane 0
    __m128 QuadNoise(__m128 d) noexcept
    {
        const __m128 sq = _mm_mul_ps(d, d);
        const __m128 t  = _mm_add_ps(sq, _mm_shuffle_ps(sq, sq, _MM_SHUFFLE(2, 3, 0, 1))); // sq0+sq1 in lane 0, sq2+sq3 in lane 2
        return _mm_add_ss(t, _mm_movehl_ps(t, t));
    }

} // namespace

// The sum keeps LAME's four lanes: line i is added to lane i % 4, in the order of the lines. The
// maximum of values >= +0 does not depend on the order, so AVX2 takes it in eight lanes.
void QUANTIZE_SIMD_NAME(Xrpow)(const float* xr, float* xrpow, std::int32_t count, float& sum, float& max) noexcept
{
    __m128       sum4 = _mm_setzero_ps();
    __m128       max4 = _mm_setzero_ps();
    std::int32_t i    = 0;
#ifdef QUANTIZE_SIMD_AVX2
    const __m256 mask8 = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
    __m256       max8  = _mm256_setzero_ps();
    for (; i + 8 <= count; i += 8) {
        const __m256 x = _mm256_and_ps(_mm256_loadu_ps(xr + i), mask8);
        sum4           = _mm_add_ps(sum4, _mm256_castps256_ps128(x)); // lines i .. i+3, then i+4 .. i+7
        sum4           = _mm_add_ps(sum4, _mm256_extractf128_ps(x, 1));
        const __m256 y = _mm256_sqrt_ps(_mm256_mul_ps(x, _mm256_sqrt_ps(x)));
        max8           = _mm256_max_ps(max8, y);
        _mm256_storeu_ps(xrpow + i, y);
    }
    max4 = _mm_max_ps(_mm256_castps256_ps128(max8), _mm256_extractf128_ps(max8, 1));
#endif
    for (; i + 4 <= count; i += 4) {
        const __m128 x = Abs4(_mm_loadu_ps(xr + i));
        sum4           = _mm_add_ps(sum4, x);
        const __m128 y = Pow34(x);
        max4           = _mm_max_ps(max4, y);
        _mm_storeu_ps(xrpow + i, y);
    }

    // the last lines, padded with zeros as in init_xrpow_core_sse
    const std::int32_t rest = count - i;
    if (rest > 0) {
        alignas(16) std::array<float, 4> tail = {0, 0, 0, 0};
        for (std::int32_t k = 0; k < rest; ++k)
            tail[k] = xr[i + k];
        const __m128 x = Abs4(_mm_load_ps(tail.data()));
        sum4           = _mm_add_ps(sum4, x);
        const __m128 y = Pow34(x);
        max4           = _mm_max_ps(max4, y);
        _mm_store_ps(tail.data(), y);
        for (std::int32_t k = 0; k < rest; ++k)
            xrpow[i + k] = tail[k];
    }

    alignas(16) std::array<float, 4> laneSum;
    alignas(16) std::array<float, 4> laneMax;
    _mm_store_ps(laneSum.data(), sum4);
    _mm_store_ps(laneMax.data(), max4);
    sum            = laneSum[0] + laneSum[1] + laneSum[2] + laneSum[3];
    const float ma = laneMax[0] > laneMax[1] ? laneMax[0] : laneMax[1];
    const float mb = laneMax[2] > laneMax[3] ? laneMax[2] : laneMax[3];
    max            = ma > mb ? ma : mb;
}

#ifndef QUANTIZE_SIMD_AVX2

// the 4-lane loops below serve the AVX2 level too: 8 lanes do not gain with the table lookups

void QUANTIZE_SIMD_NAME(QuantizeLines)(const float* xr34, std::int32_t* l3, std::int32_t count, float step34, const float* adj43) noexcept
{
    const __m128 step4 = _mm_set1_ps(step34);
    std::int32_t i     = 0;
    for (; i + 4 <= count; i += 4)
        _mm_storeu_si128(reinterpret_cast<__m128i*>(l3 + i), Quantize4(_mm_mul_ps(step4, _mm_loadu_ps(xr34 + i)), adj43));

    // the last lines as a quad padded with zeros, as quantize_x34 does
    const std::int32_t rest = count - i;
    if (rest > 0) {
        alignas(16) std::array<float, 4>        x = {0, 0, 0, 0};
        alignas(16) std::array<std::int32_t, 4> q;
        for (std::int32_t k = 0; k < rest; ++k)
            x[k] = xr34[i + k];
        _mm_store_si128(reinterpret_cast<__m128i*>(q.data()), Quantize4(_mm_mul_ps(step4, _mm_load_ps(x.data())), adj43));
        for (std::int32_t k = 0; k < rest; ++k)
            l3[i + k] = q[k];
    }
}

float QUANTIZE_SIMD_NAME(SfbNoise)(const float* xr, const float* xr34, std::int32_t bw, float step, float step34, const float* pow43,
                                   const float* adj43) noexcept
{
    const __m128 stepV   = _mm_set1_ps(step);
    const __m128 step34V = _mm_set1_ps(step34);
    __m128       noise   = _mm_setzero_ps(); // in lane 0
    std::int32_t i       = 0;
    for (; i + 4 <= bw; i += 4) {
        const __m128i l = Quantize4(_mm_mul_ps(step34V, _mm_loadu_ps(xr34 + i)), adj43);
        const __m128  d = _mm_sub_ps(Abs4(_mm_loadu_ps(xr + i)), _mm_mul_ps(stepV, Lookup4(pow43, l)));
        noise           = _mm_add_ss(noise, QuadNoise(d));
    }

    // the last lines as a quad padded with zeros: they quantize to 0 and add (0 - step * 0)^2 = 0
    const std::int32_t rest = bw - i;
    if (rest > 0) {
        alignas(16) std::array<float, 4> a = {0, 0, 0, 0};
        alignas(16) std::array<float, 4> b = {0, 0, 0, 0};
        for (std::int32_t k = 0; k < rest; ++k) {
            a[k] = xr34[i + k];
            b[k] = xr[i + k];
        }
        const __m128i l = Quantize4(_mm_mul_ps(step34V, _mm_load_ps(a.data())), adj43);
        const __m128  d = _mm_sub_ps(Abs4(_mm_load_ps(b.data())), _mm_mul_ps(stepV, Lookup4(pow43, l)));
        noise           = _mm_add_ss(noise, QuadNoise(d));
    }
    return _mm_cvtss_f32(noise);
}

#endif

#ifdef QUANTIZE_SIMD_AVX2

namespace {

    // table[index] for indices 0..15 from two registers: two permutes and a blend
    __m256 Lookup16(__m256 low, __m256 high, __m256i index) noexcept
    {
        const __m256 a = _mm256_permutevar8x32_ps(low, index);
        const __m256 b = _mm256_permutevar8x32_ps(high, index);
        return _mm256_blendv_ps(a, b, _mm256_castsi256_ps(_mm256_cmpgt_epi32(index, _mm256_set1_epi32(7))));
    }

    // table[index] for any index: four scalar loads per half
    __m256 Lookup8(const float* table, __m256i index) noexcept
    {
        return _mm256_insertf128_ps(_mm256_castps128_ps256(Lookup4(table, _mm256_castsi256_si128(index))), Lookup4(table, _mm256_extracti128_si256(index, 1)),
                                    1);
    }

    bool AnyAbove15(__m256i index) noexcept
    {
        return _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(index, _mm256_set1_epi32(15)))) != 0;
    }

} // namespace

// SfbNoise with two quads per register. Most quantized values are small: for indices up to 15
// adj43 and pow43 come from registers, otherwise from the tables. The two quad sums are added to
// the noise one after the other, as in the scalar code.
float QUANTIZE_SIMD_NAME(SfbNoise)(const float* xr, const float* xr34, std::int32_t bw, float step, float step34, const float* pow43,
                                   const float* adj43) noexcept
{
    const __m256 adjLow   = _mm256_loadu_ps(adj43);
    const __m256 adjHigh  = _mm256_loadu_ps(adj43 + 8);
    const __m256 powLow   = _mm256_loadu_ps(pow43);
    const __m256 powHigh  = _mm256_loadu_ps(pow43 + 8);
    const __m256 step8    = _mm256_set1_ps(step);
    const __m256 step34x8 = _mm256_set1_ps(step34);
    const __m256 absMask  = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
    __m128       noise    = _mm_setzero_ps(); // in lane 0
    std::int32_t i        = 0;
    for (; i + 8 <= bw; i += 8) {
        const __m256  x   = _mm256_mul_ps(step34x8, _mm256_loadu_ps(xr34 + i));
        const __m256i l1  = _mm256_cvttps_epi32(x);
        const __m256  adj = AnyAbove15(l1) ? Lookup8(adj43, l1) : Lookup16(adjLow, adjHigh, l1);
        const __m256i l   = _mm256_cvttps_epi32(_mm256_add_ps(x, adj));
        const __m256  p   = AnyAbove15(l) ? Lookup8(pow43, l) : Lookup16(powLow, powHigh, l);
        const __m256  d   = _mm256_sub_ps(_mm256_and_ps(_mm256_loadu_ps(xr + i), absMask), _mm256_mul_ps(step8, p));
        const __m256  sq  = _mm256_mul_ps(d, d);
        const __m256  t   = _mm256_add_ps(sq, _mm256_shuffle_ps(sq, sq, _MM_SHUFFLE(2, 3, 0, 1)));
        const __m256  u   = _mm256_add_ps(t, _mm256_shuffle_ps(t, t, _MM_SHUFFLE(1, 0, 3, 2))); // quad sums in lanes 0 and 4
        noise             = _mm_add_ss(noise, _mm256_castps256_ps128(u));
        noise             = _mm_add_ss(noise, _mm256_extractf128_ps(u, 1));
    }

    // a last full quad, then the last lines as a quad padded with zeros
    const __m128 stepV   = _mm_set1_ps(step);
    const __m128 step34V = _mm_set1_ps(step34);
    for (; i + 4 <= bw; i += 4) {
        const __m128i l = Quantize4(_mm_mul_ps(step34V, _mm_loadu_ps(xr34 + i)), adj43);
        const __m128  d = _mm_sub_ps(Abs4(_mm_loadu_ps(xr + i)), _mm_mul_ps(stepV, Lookup4(pow43, l)));
        noise           = _mm_add_ss(noise, QuadNoise(d));
    }
    const std::int32_t rest = bw - i;
    if (rest > 0) {
        alignas(16) std::array<float, 4> a = {0, 0, 0, 0};
        alignas(16) std::array<float, 4> b = {0, 0, 0, 0};
        for (std::int32_t k = 0; k < rest; ++k) {
            a[k] = xr34[i + k];
            b[k] = xr[i + k];
        }
        const __m128i l = Quantize4(_mm_mul_ps(step34V, _mm_load_ps(a.data())), adj43);
        const __m128  d = _mm_sub_ps(Abs4(_mm_load_ps(b.data())), _mm_mul_ps(stepV, Lookup4(pow43, l)));
        noise           = _mm_add_ss(noise, QuadNoise(d));
    }
    return _mm_cvtss_f32(noise);
}

#endif

// all values are >= +0, so the maximum does not depend on the order
float QUANTIZE_SIMD_NAME(VecMax)(const float* xr34, std::int32_t bw) noexcept
{
    std::int32_t i    = 0;
    __m128       max4 = _mm_setzero_ps();
#ifdef QUANTIZE_SIMD_AVX2
    __m256 max8 = _mm256_setzero_ps();
    for (; i + 8 <= bw; i += 8)
        max8 = _mm256_max_ps(max8, _mm256_loadu_ps(xr34 + i));
    max4 = _mm_max_ps(_mm256_castps256_ps128(max8), _mm256_extractf128_ps(max8, 1));
#endif
    for (; i + 4 <= bw; i += 4)
        max4 = _mm_max_ps(max4, _mm_loadu_ps(xr34 + i));
    max4      = _mm_max_ps(max4, _mm_movehl_ps(max4, max4));
    max4      = _mm_max_ss(max4, _mm_shuffle_ps(max4, max4, 1));
    float max = _mm_cvtss_f32(max4);
    for (; i < bw; ++i) {
        if (max < xr34[i])
            max = xr34[i];
    }
    return max;
}

#undef QUANTIZE_SIMD_NAME
#undef QUANTIZE_SIMD_JOIN
#undef QUANTIZE_SIMD_JOIN2
