// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

// The polyphase window of WindowSubband with its 15 pairs of subbands in SIMD lanes, included by
// MdctSse41.cpp and MdctAvx2.cpp (MDCT_SIMD_SUFFIX, MDCT_SIMD_AVX2). Lane p computes pair p with
// the scalar order of operations: its samples are contiguous across lanes (x2 ascending, x1
// descending, one reversal), its window entries come from a transposed copy of the window.

#define MDCT_SIMD_JOIN2(name, suffix) name##suffix
#define MDCT_SIMD_JOIN(name, suffix)  MDCT_SIMD_JOIN2(name, suffix)
#define MDCT_SIMD_NAME(name)          MDCT_SIMD_JOIN(name, MDCT_SIMD_SUFFIX)

namespace {

#ifdef MDCT_SIMD_AVX2
    struct Ops {
        using V                             = __m256;
        static constexpr std::int32_t Width = 8;
        static V    Load(const float* p) noexcept { return _mm256_loadu_ps(p); }
        static V    Add(V a, V b) noexcept { return _mm256_add_ps(a, b); }
        static V    Sub(V a, V b) noexcept { return _mm256_sub_ps(a, b); }
        static V    Mul(V a, V b) noexcept { return _mm256_mul_ps(a, b); }
        static V    Reverse(V v) noexcept { return _mm256_permutevar8x32_ps(v, _mm256_setr_epi32(7, 6, 5, 4, 3, 2, 1, 0)); }
        // even[l] to out[2l], odd[l] to out[2l + 1]
        static void StoreInterleaved(float* out, V even, V odd) noexcept
        {
            const __m256 lo = _mm256_unpacklo_ps(even, odd);
            const __m256 hi = _mm256_unpackhi_ps(even, odd);
            _mm256_storeu_ps(out, _mm256_permute2f128_ps(lo, hi, 0x20));
            _mm256_storeu_ps(out + 8, _mm256_permute2f128_ps(lo, hi, 0x31));
        }
    };
    // the groups of pairs; the last overlaps the first (lane 7 twice, same result) so that no
    // lane reads beyond the samples the scalar code reads
    constexpr std::array<std::int32_t, 2> groups = {0, 7};
#else
    struct Ops {
        using V                             = __m128;
        static constexpr std::int32_t Width = 4;
        static V    Load(const float* p) noexcept { return _mm_loadu_ps(p); }
        static V    Add(V a, V b) noexcept { return _mm_add_ps(a, b); }
        static V    Sub(V a, V b) noexcept { return _mm_sub_ps(a, b); }
        static V    Mul(V a, V b) noexcept { return _mm_mul_ps(a, b); }
        static V    Reverse(V v) noexcept { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 1, 2, 3)); }
        static void StoreInterleaved(float* out, V even, V odd) noexcept
        {
            _mm_storeu_ps(out, _mm_unpacklo_ps(even, odd));
            _mm_storeu_ps(out + 4, _mm_unpackhi_ps(even, odd));
        }
    };
    constexpr std::array<std::int32_t, 4> groups = {0, 4, 8, 11}; // 11 twice, see above
#endif

    // the window entries of pair p in column p: tap q of pair p is enwindow[18 * p + q]
    struct WindowColumns {
        alignas(32) std::array<std::array<float, 16>, 18> tap;
        const float*      source;

        explicit WindowColumns(const float* enwindow) noexcept
            : source(enwindow)
        {
            for (std::int32_t q = 0; q < 18; ++q) {
                for (std::int32_t p = 0; p < 16; ++p)
                    tap[q][p] = p < 15 ? enwindow[18 * p + q] : 0.0f;
            }
        }
    };

} // namespace

void MDCT_SIMD_NAME(WindowSubband)(const float* enwindow, const sample_t* x1, float* a) noexcept
{
    // newmdct.c has one window table; another one (never passed by the encoder) takes the scalar path
    static const WindowColumns window(enwindow);
    if (enwindow != window.source) {
        WindowSubband(enwindow, x1, a);
        return;
    }
    const sample_t* const x2 = &x1[238 - 14 - 286];

    for (const std::int32_t p0 : groups) {
        using V = Ops::V;
        // lane l is pair p0 + l: x2 + p0 + l ascending, x1 - p0 - l descending
        auto up   = [&](std::int32_t offset) { return Ops::Load(x2 + p0 + offset); };
        auto down = [&](std::int32_t offset) { return Ops::Reverse(Ops::Load(x1 - p0 - (Ops::Width - 1) + offset)); };
        auto tap  = [&](std::int32_t q) { return Ops::Load(&window.tap[q][p0]); };

        V w = tap(0);
        V s = Ops::Mul(up(-224), w);
        V t = Ops::Mul(down(224), w);
        for (std::int32_t k = 1; k < 8; ++k) {
            w = tap(k);
            s = Ops::Add(s, Ops::Mul(up(-224 + 64 * k), w));
            t = Ops::Add(t, Ops::Mul(down(224 - 64 * k), w));
        }
        for (std::int32_t k = 0; k < 8; ++k) {
            w = tap(8 + k);
            s = Ops::Add(s, Ops::Mul(down(-256 + 64 * k), w));
            t = Ops::Sub(t, Ops::Mul(up(256 - 64 * k), w));
        }

        s = Ops::Mul(s, tap(16));
        w = Ops::Sub(t, s);
        // pair p is a[30 + 2i], a[31 + 2i] with i = p - 15, that is a[2p], a[2p + 1]
        Ops::StoreInterleaved(a + 2 * p0, Ops::Add(t, s), Ops::Mul(tap(17), w));
    }

    MdctKernels::WindowSubbandTail(enwindow, x1, a);
}

#undef MDCT_SIMD_NAME
#undef MDCT_SIMD_JOIN
#undef MDCT_SIMD_JOIN2
