// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

// The SIMD loops of the psychoacoustic model, included by PsySse41.cpp and PsyAvx2.cpp, which set
// PSY_SIMD_SUFFIX (Sse41 or Avx2) and, for AVX2, PSY_SIMD_AVX2. Everything here but the kernels
// has internal linkage, so the AVX2 file's code cannot be merged into other translation units.

#define PSY_SIMD_JOIN2(name, suffix) name##suffix
#define PSY_SIMD_JOIN(name, suffix)  PSY_SIMD_JOIN2(name, suffix)
#define PSY_SIMD_NAME(name)          PSY_SIMD_JOIN(name, PSY_SIMD_SUFFIX)

namespace {

    // table[l[0..3]] without a round trip through memory: the indices as two 64-bit halves,
    // four scalar loads, two unpacks
    __m128 Lookup4(const float* table, __m128i l) noexcept
    {
        const std::int64_t lo = _mm_cvtsi128_si64(l);
        const std::int64_t hi = _mm_extract_epi64(l, 1);
        const __m128       a  = _mm_unpacklo_ps(_mm_load_ss(table + static_cast<std::int32_t>(lo)), _mm_load_ss(table + (lo >> 32)));
        const __m128       b  = _mm_unpacklo_ps(_mm_load_ss(table + static_cast<std::int32_t>(hi)), _mm_load_ss(table + (hi >> 32)));
        return _mm_movelh_ps(a, b);
    }

#ifdef PSY_SIMD_AVX2
    struct Ops {
        using V                             = __m256;
        static constexpr std::int32_t Width = 8;
        static V     Load(const float* p) noexcept { return _mm256_loadu_ps(p); }
        static void  Store(float* p, V v) noexcept { _mm256_storeu_ps(p, v); }
        static V     Set(float x) noexcept { return _mm256_set1_ps(x); }
        static V     Zero() noexcept { return _mm256_setzero_ps(); }
        static V     Add(V a, V b) noexcept { return _mm256_add_ps(a, b); }
        static V     Sub(V a, V b) noexcept { return _mm256_sub_ps(a, b); }
        static V     Mul(V a, V b) noexcept { return _mm256_mul_ps(a, b); }
        static V     Max(V a, V b) noexcept { return _mm256_max_ps(a, b); }
        static V     Abs(V v) noexcept { return _mm256_and_ps(v, _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF))); }
        static V     Div(V a, V b) noexcept { return _mm256_div_ps(a, b); }
        static V     Lt(V a, V b) noexcept { return _mm256_cmp_ps(a, b, _CMP_LT_OQ); }
        static V     Le(V a, V b) noexcept { return _mm256_cmp_ps(a, b, _CMP_LE_OQ); }
        static V     Gt(V a, V b) noexcept { return _mm256_cmp_ps(a, b, _CMP_GT_OQ); }
        static V     Ge(V a, V b) noexcept { return _mm256_cmp_ps(a, b, _CMP_GE_OQ); }
        static V     And(V a, V b) noexcept { return _mm256_and_ps(a, b); }
        static V     AndNot(V a, V b) noexcept { return _mm256_andnot_ps(a, b); } // ~a & b
        static V     Or(V a, V b) noexcept { return _mm256_or_ps(a, b); }
        static V     Select(V mask, V yes, V no) noexcept { return _mm256_blendv_ps(no, yes, mask); }

        using I = __m256i;
        static I     Bits(V v) noexcept { return _mm256_castps_si256(v); }
        static I     SetI(std::int32_t x) noexcept { return _mm256_set1_epi32(x); }
        static I     AndI(I a, I b) noexcept { return _mm256_and_si256(a, b); }
        static I     SubI(I a, I b) noexcept { return _mm256_sub_epi32(a, b); }
        static I     MinI(I a, I b) noexcept { return _mm256_min_epi32(a, b); }
        static I     MaxI(I a, I b) noexcept { return _mm256_max_epi32(a, b); }
        template <int N>
        static I     SrlI(I a) noexcept { return _mm256_srli_epi32(a, N); }
        template <int N>
        static I     SraI(I a) noexcept { return _mm256_srai_epi32(a, N); }
        static V     ToFloat(I a) noexcept { return _mm256_cvtepi32_ps(a); }
        static V     Mask(I a) noexcept { return _mm256_castsi256_ps(a); }
        static I     AddI(I a, I b) noexcept { return _mm256_add_epi32(a, b); }
        static I     AbsI(I a) noexcept { return _mm256_abs_epi32(a); }
        static I     GtI(I a, I b) noexcept { return _mm256_cmpgt_epi32(a, b); }
        static I     LoadI(const std::int32_t* p) noexcept { return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p)); }
        // MaskAddTable2[index] for index 0..9: two registers and permutes
        static V LookupTable2(I index) noexcept
        {
            const __m256 low  = _mm256_loadu_ps(MaskAddTable2.data());
            const __m256 high = _mm256_castps128_ps256(_mm_castsi128_ps(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(MaskAddTable2.data() + 8))));
            const __m256 a    = _mm256_permutevar8x32_ps(low, index);
            const __m256 b    = _mm256_permutevar8x32_ps(high, _mm256_sub_epi32(index, _mm256_set1_epi32(8)));
            return _mm256_blendv_ps(a, b, _mm256_castsi256_ps(_mm256_cmpgt_epi32(index, _mm256_set1_epi32(7))));
        }
        static V     Lookup(const float* table, I index) noexcept
        {
            return _mm256_insertf128_ps(_mm256_castps128_ps256(Lookup4(table, _mm256_castsi256_si128(index))), Lookup4(table, _mm256_extracti128_si256(index, 1)), 1);
        }
        static void  StoreI(std::int32_t* p, I v) noexcept { _mm256_storeu_si256(reinterpret_cast<__m256i*>(p), v); }

        // (float) (ecb < (double) eb * f ? ecb : (double) eb * f), lane by lane in double
        static V MinDouble(V ecb, V eb, double f) noexcept
        {
            const __m256d fd = _mm256_set1_pd(f);
            auto          half = [&](__m128 e, __m128 b) {
                const __m256d de = _mm256_cvtps_pd(e);
                const __m256d dl = _mm256_mul_pd(_mm256_cvtps_pd(b), fd);
                return _mm256_cvtpd_ps(_mm256_blendv_pd(dl, de, _mm256_cmp_pd(de, dl, _CMP_LT_OQ)));
            };
            return _mm256_insertf128_ps(_mm256_castps128_ps256(half(_mm256_castps256_ps128(ecb), _mm256_castps256_ps128(eb))),
                                        half(_mm256_extractf128_ps(ecb, 1), _mm256_extractf128_ps(eb, 1)), 1);
        }
        static float HMax(V v) noexcept
        {
            __m128 m = _mm_max_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
            m        = _mm_max_ps(m, _mm_movehl_ps(m, m));
            return _mm_cvtss_f32(_mm_max_ss(m, _mm_shuffle_ps(m, m, 1)));
        }
    };
#else
    struct Ops {
        using V                             = __m128;
        static constexpr std::int32_t Width = 4;
        static V     Load(const float* p) noexcept { return _mm_loadu_ps(p); }
        static void  Store(float* p, V v) noexcept { _mm_storeu_ps(p, v); }
        static V     Set(float x) noexcept { return _mm_set1_ps(x); }
        static V     Zero() noexcept { return _mm_setzero_ps(); }
        static V     Add(V a, V b) noexcept { return _mm_add_ps(a, b); }
        static V     Sub(V a, V b) noexcept { return _mm_sub_ps(a, b); }
        static V     Mul(V a, V b) noexcept { return _mm_mul_ps(a, b); }
        static V     Max(V a, V b) noexcept { return _mm_max_ps(a, b); }
        static V     Abs(V v) noexcept { return _mm_and_ps(v, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF))); }
        static V     Div(V a, V b) noexcept { return _mm_div_ps(a, b); }
        static V     Lt(V a, V b) noexcept { return _mm_cmplt_ps(a, b); }
        static V     Le(V a, V b) noexcept { return _mm_cmple_ps(a, b); }
        static V     Gt(V a, V b) noexcept { return _mm_cmpgt_ps(a, b); }
        static V     Ge(V a, V b) noexcept { return _mm_cmpge_ps(a, b); }
        static V     And(V a, V b) noexcept { return _mm_and_ps(a, b); }
        static V     AndNot(V a, V b) noexcept { return _mm_andnot_ps(a, b); } // ~a & b
        static V     Or(V a, V b) noexcept { return _mm_or_ps(a, b); }
        static V     Select(V mask, V yes, V no) noexcept { return _mm_blendv_ps(no, yes, mask); }

        using I = __m128i;
        static I     Bits(V v) noexcept { return _mm_castps_si128(v); }
        static I     SetI(std::int32_t x) noexcept { return _mm_set1_epi32(x); }
        static I     AndI(I a, I b) noexcept { return _mm_and_si128(a, b); }
        static I     SubI(I a, I b) noexcept { return _mm_sub_epi32(a, b); }
        static I     MinI(I a, I b) noexcept { return _mm_min_epi32(a, b); }
        static I     MaxI(I a, I b) noexcept { return _mm_max_epi32(a, b); }
        template <int N>
        static I     SrlI(I a) noexcept { return _mm_srli_epi32(a, N); }
        template <int N>
        static I     SraI(I a) noexcept { return _mm_srai_epi32(a, N); }
        static V     ToFloat(I a) noexcept { return _mm_cvtepi32_ps(a); }
        static V     Mask(I a) noexcept { return _mm_castsi128_ps(a); }
        static I     AddI(I a, I b) noexcept { return _mm_add_epi32(a, b); }
        static I     AbsI(I a) noexcept { return _mm_abs_epi32(a); }
        static I     GtI(I a, I b) noexcept { return _mm_cmpgt_epi32(a, b); }
        static I     LoadI(const std::int32_t* p) noexcept { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)); }
        static V     Lookup(const float* table, I index) noexcept { return Lookup4(table, index); }
        static V     LookupTable2(I index) noexcept { return Lookup4(MaskAddTable2.data(), index); }
        static void  StoreI(std::int32_t* p, I v) noexcept { _mm_storeu_si128(reinterpret_cast<__m128i*>(p), v); }

        // (float) (ecb < (double) eb * f ? ecb : (double) eb * f), lane by lane in double
        static V MinDouble(V ecb, V eb, double f) noexcept
        {
            const __m128d fd   = _mm_set1_pd(f);
            auto          half = [&](__m128 e, __m128 b) {
                const __m128d de = _mm_cvtps_pd(e);
                const __m128d dl = _mm_mul_pd(_mm_cvtps_pd(b), fd);
                return _mm_cvtpd_ps(_mm_blendv_pd(dl, de, _mm_cmplt_pd(de, dl)));
            };
            return _mm_movelh_ps(half(ecb, eb), half(_mm_movehl_ps(ecb, ecb), _mm_movehl_ps(eb, eb)));
        }
        static float HMax(V m) noexcept
        {
            m = _mm_max_ps(m, _mm_movehl_ps(m, m));
            return _mm_cvtss_f32(_mm_max_ss(m, _mm_shuffle_ps(m, m, 1)));
        }
    };
#endif

    // the high pass of fs/4 for 576 samples; each lane computes one sample in the scalar order
    void HighPass(const sample_t* firbuf, float* out) noexcept
    {
        using V = Ops::V;
        std::array<V, 10> coef;
        for (std::int32_t j = 0; j < 10; ++j)
            coef[j] = Ops::Set(AttackFirCoef[j]);
        for (std::int32_t i = 0; i < 576; i += Ops::Width) {
            const sample_t* const f    = firbuf + i;
            V                     sum1 = Ops::Load(f + 10);
            V                     sum2 = Ops::Zero();
            for (std::int32_t j = 0; j < ((NSFIRLEN - 1) / 2) - 1; j += 2) {
                sum1 = Ops::Add(sum1, Ops::Mul(coef[j], Ops::Add(Ops::Load(f + j), Ops::Load(f + NSFIRLEN - j))));
                sum2 = Ops::Add(sum2, Ops::Mul(coef[j + 1], Ops::Add(Ops::Load(f + j + 1), Ops::Load(f + NSFIRLEN - j - 1))));
            }
            Ops::Store(out + i, Ops::Add(sum1, sum2));
        }
    }

    // the peak of |x| (at least 1) of each of the 9 sub-short blocks; the maximum of values >= +0
    // does not depend on the order
    void Peaks(const float* x, float* peaks) noexcept
    {
        for (std::int32_t b = 0; b < 9; ++b, x += 576 / 9) {
            Ops::V p = Ops::Set(1.0f);
            for (std::int32_t i = 0; i < 576 / 9; i += Ops::Width)
                p = Ops::Max(p, Ops::Abs(Ops::Load(x + i)));
            peaks[b] = Ops::HMax(p);
        }
    }

    // vbrpsy_mask_add in every lane; near is all ones where |b| <= delta. All branches are
    // computed and selected; the values of lanes that a branch does not take do not matter,
    // but they must not index outside the tables.
    Ops::V MaskAddLanes(Ops::V m1, Ops::V m2, Ops::V near, const float* limits) noexcept
    {
        const Ops::V zero = Ops::Zero();
        m1                = Ops::Select(Ops::Lt(m1, zero), zero, m1);
        m2                = Ops::Select(Ops::Lt(m2, zero), zero, m2);
        const Ops::V m2Larger = Ops::Gt(m2, m1);
        const Ops::V ratio    = Ops::Div(Ops::Select(m2Larger, m2, m1), Ops::Select(m2Larger, m1, m2));
        const Ops::V sum      = Ops::Add(m1, m2);

        // near: (m1 + m2) * table2[(int) FastLog10X(ratio, 16)] below the limit; the index is the
        // number of index limits the ratio reaches (MaskAddIndexLimits), 0..9 in every lane
        Ops::I i = Ops::SetI(0);
        for (std::int32_t k = 0; k < 9; ++k)
            i = Ops::SubI(i, Ops::Bits(Ops::Ge(ratio, Ops::Set(limits[k]))));
        const Ops::V nearValue = Ops::Select(Ops::Ge(ratio, Ops::Set(MaskAddMaxI1)), sum, Ops::Mul(sum, Ops::LookupTable2(i)));

        // far: the sum below the limit, else the larger one
        const Ops::V farValue = Ops::Select(Ops::Lt(ratio, Ops::Set(MaskAddMaxI2)), sum, Ops::Select(Ops::Lt(m1, m2), m2, m1));

        Ops::V result = Ops::Select(near, nearValue, farValue);
        result        = Ops::Select(Ops::Le(m2, zero), m1, result);
        result        = Ops::Select(Ops::Le(m1, zero), m2, result);
        return result;
    }

    // the chains of Ops::Width partitions, one per lane
    struct ChainLanes {
        Ops::I       first;  // s3ind[b][0]
        Ops::I       length; // s3ind[b][1] - s3ind[b][0] + 1, 0 for lanes past npart
        Ops::I       row;    // start of the partition's row in s3
        Ops::I       deltaPlus1;
        Ops::I       b;
        Ops::V       ecb;
        std::int32_t maxLength;
    };

    ChainLanes InitChains(const std::array<std::int32_t, 2>* s3ind, std::int32_t npart, const std::uint8_t* maskIdx, std::int32_t b0,
                          std::int32_t& rowStart) noexcept
    {
        alignas(32) std::int32_t first[Ops::Width], length[Ops::Width], row[Ops::Width], delta[Ops::Width], b[Ops::Width];
        ChainLanes               c{};
        c.maxLength = 0;
        for (std::int32_t l = 0; l < Ops::Width; ++l) {
            b[l] = b0 + l;
            if (b[l] < npart) {
                first[l]  = s3ind[b[l]][0];
                length[l] = s3ind[b[l]][1] - s3ind[b[l]][0] + 1;
                row[l]    = rowStart;
                delta[l]  = MaskAddDeltaTable[maskIdx[b[l]]] + 1;
                rowStart += length[l];
            } else {
                first[l] = length[l] = row[l] = delta[l] = 0;
            }
            if (c.maxLength < length[l])
                c.maxLength = length[l];
        }
        c.first      = Ops::LoadI(first);
        c.length     = Ops::LoadI(length);
        c.row        = Ops::LoadI(row);
        c.deltaPlus1 = Ops::LoadI(delta);
        c.b          = Ops::LoadI(b);
        return c;
    }

    // the term of step j in each lane; lanes past their range read a valid entry
    Ops::V ChainTerm(const ChainLanes& c, Ops::I j, const float* terms) noexcept
    {
        const Ops::I jj = Ops::MaxI(Ops::MinI(j, Ops::SubI(c.length, Ops::SetI(1))), Ops::SetI(0));
        return Ops::Lookup(terms, Ops::AddI(c.row, jj));
    }

    void ChainStep(ChainLanes& c, std::int32_t j, const float* terms, const float* limits) noexcept
    {
        const Ops::I jv     = Ops::SetI(j);
        const Ops::V x      = ChainTerm(c, jv, terms);
        const Ops::V active = Ops::Mask(Ops::GtI(c.length, jv));
        // |kk - b| <= delta with kk = first + j
        const Ops::V near = Ops::Mask(Ops::GtI(c.deltaPlus1, Ops::AbsI(Ops::SubI(Ops::AddI(c.first, jv), c.b))));
        c.ecb             = Ops::Select(active, MaskAddLanes(c.ecb, x, near, limits), c.ecb);
    }

} // namespace

void PSY_SIMD_NAME(AttackPrep)(const ChannelSamples& buffer, std::int32_t channels, std::int32_t psyChannels, SubShortPeaks& subShort) noexcept
{
    alignas(32) std::array<std::array<float, 576>, 2> hp;
    std::memset(&hp, 0, sizeof(hp));
    for (std::int32_t chn = 0; chn < channels; chn++)
        HighPass(&buffer[chn][576 - 350 - NSFIRLEN + 192], hp[chn].data());

    for (std::int32_t chn = 0; chn < psyChannels; chn++) {
        if (chn == 2) {
            // mid and side
            for (std::int32_t i = 0; i < 576; i += Ops::Width) {
                const Ops::V l = Ops::Load(hp[0].data() + i);
                const Ops::V r = Ops::Load(hp[1].data() + i);
                Ops::Store(hp[0].data() + i, Ops::Add(l, r));
                Ops::Store(hp[1].data() + i, Ops::Sub(l, r));
            }
        }
        Peaks(hp[chn & 1].data(), subShort[chn].data());
    }
}

// the spreading convolution with one partition per lane: each lane runs its partition's chain
// in the scalar order; lanes whose range has ended keep their value. Several groups of lanes run
// side by side, so that the latency of one chain (division, log, lookups) overlaps the others.
void PSY_SIMD_NAME(MaskChains)(const float* s3, const std::array<std::int32_t, 2>* s3ind, std::int32_t npart, const float* eb, const std::uint8_t* maskIdx,
                               float* ecbOut) noexcept
{
    constexpr std::int32_t W      = Ops::Width;
    const float* const     limits = MaskAddIndexLimits();

    // the terms s3 * eb * tab[maskIdx] of all rows, in the scalar order of the products; a row's
    // entries and its partitions' energies are contiguous
    std::array<float, CBANDS> maskTab;
    for (std::int32_t k = 0; k < npart; ++k)
        maskTab[k] = MaskTab[maskIdx[k]];
    alignas(32) std::array<float, CBANDS * CBANDS> terms;
    std::int32_t      k = 0;
    for (std::int32_t b = 0; b < npart; ++b) {
        const std::int32_t first  = s3ind[b][0];
        const std::int32_t length = s3ind[b][1] - first + 1;
        std::int32_t       j      = 0;
        for (; j + Ops::Width <= length; j += Ops::Width)
            Ops::Store(terms.data() + k + j, Ops::Mul(Ops::Mul(Ops::Load(s3 + k + j), Ops::Load(eb + first + j)), Ops::Load(maskTab.data() + first + j)));
        for (; j < length; ++j)
            terms[k + j] = s3[k + j] * eb[first + j] * maskTab[first + j];
        k += length;
    }

    // groups of lanes side by side: their chains are independent and overlap
    constexpr std::int32_t G        = 4;
    std::int32_t           rowStart = 0;
    for (std::int32_t b0 = 0; b0 < npart; b0 += G * W) {
        std::array<ChainLanes, G> group;
        std::int32_t steps = 0;
        for (std::int32_t g = 0; g < G; ++g) {
            group[g]     = InitChains(s3ind, npart, maskIdx, b0 + g * W, rowStart);
            group[g].ecb = ChainTerm(group[g], Ops::SetI(0), terms.data());
            if (steps < group[g].maxLength)
                steps = group[g].maxLength;
        }
        for (std::int32_t j = 1; j < steps; ++j) {
            for (std::int32_t g = 0; g < G; ++g) {
                if (j < group[g].maxLength)
                    ChainStep(group[g], j, terms.data(), limits);
            }
        }

        alignas(32) std::array<float, G * W> out;
        for (std::int32_t g = 0; g < G; ++g)
            Ops::Store(out.data() + g * W, group[g].ecb);
        for (std::int32_t l = 0; l < G * W && b0 + l < npart; ++l)
            ecbOut[b0 + l] = out[l];
    }
}

// the long block pre-echo control with one partition per lane; the case of the block type is the
// same for all partitions; the partitions that do not fill a vector take the scalar code
void PSY_SIMD_NAME(MaskingLong)(const MaskingLongArgs& a) noexcept
{
    using V                      = Ops::V;
    const V            zero      = Ops::Zero();
    const V            one       = Ops::Set(1.0f);
    const V            global    = Ops::Set(a.maskingLowerGlobal);
    const bool         afterShort = a.blocktypeOld == SHORT_TYPE;
    const bool         afterNorm  = a.blocktypeOld == NORM_TYPE;
    std::int32_t       b          = 0;
    for (; b + Ops::Width <= a.npart; b += Ops::Width) {
        const V maskingLower = Ops::Mul(Ops::Load(a.maskingLower + b), global);
        const V ecb          = Ops::Load(a.ecb + b);
        const V nb1          = Ops::Load(a.nbL1 + b);
        const V eb           = Ops::Load(a.eb + b);
        V       thr;
        if (afterShort) {
            const V ecbLimit = Ops::Mul(Ops::Set(2.0f), nb1);
            thr              = Ops::Select(Ops::Gt(ecbLimit, zero), Ops::Select(Ops::Lt(ecb, ecbLimit), ecb, ecbLimit), Ops::MinDouble(ecb, eb, 0.3));
        } else {
            V ecbLimit2 = Ops::Mul(Ops::Set(16.0f), Ops::Load(a.nbL2 + b));
            V ecbLimit1 = Ops::Mul(Ops::Set(2.0f), nb1);
            ecbLimit2   = Ops::Select(Ops::Le(ecbLimit2, zero), ecb, ecbLimit2);
            ecbLimit1   = Ops::Select(Ops::Le(ecbLimit1, zero), ecb, ecbLimit1);
            const V ecbLimit = afterNorm ? Ops::Select(Ops::Lt(ecbLimit1, ecbLimit2), ecbLimit1, ecbLimit2) : ecbLimit1;
            thr              = Ops::Select(Ops::Lt(ecb, ecbLimit), ecb, ecbLimit);
        }
        Ops::Store(a.nbL2 + b, nb1);
        Ops::Store(a.nbL1 + b, ecb);
        const V limit = Ops::Load(a.limit + b);
        thr           = Ops::Select(Ops::Gt(thr, limit), limit, thr);
        thr           = Ops::Select(Ops::Gt(maskingLower, one), Ops::Mul(thr, maskingLower), thr);
        thr           = Ops::Select(Ops::Gt(thr, eb), eb, thr);
        thr           = Ops::Select(Ops::Lt(maskingLower, one), Ops::Mul(thr, maskingLower), thr);
        Ops::Store(a.thr + b, thr);
    }
    for (; b < a.npart; ++b)
        MaskingLongOne(a, b);
}

#undef PSY_SIMD_NAME
#undef PSY_SIMD_JOIN
#undef PSY_SIMD_JOIN2
