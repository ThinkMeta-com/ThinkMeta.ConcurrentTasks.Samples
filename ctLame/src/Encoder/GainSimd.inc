// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

// The SIMD input part of the ReplayGain's Yule filter, included by GainSse41.cpp and GainAvx2.cpp, which set GAIN_SIMD_YULE
// and GAIN_SIMD_WIDE (the traits of the vector). Everything here has internal linkage, so the AVX2
// file's code cannot be merged into other translation units.
//
// Each lane computes one output sample with LAME's expression and order (YuleFir): products of the inputs
// with broadcast coefficients, summed left to right. The samples that do not fill a vector use the scalar expression.

namespace {

    struct Sse {
        using V                    = __m128;
        static constexpr int Width = 4;
        static V Load(const float* p) noexcept { return _mm_loadu_ps(p); }
        static void Store(float* p, V v) noexcept { _mm_storeu_ps(p, v); }
        static V Set(float x) noexcept { return _mm_set1_ps(x); }
        static V Mul(V a, V b) noexcept { return _mm_mul_ps(a, b); }
        static V Add(V a, V b) noexcept { return _mm_add_ps(a, b); }
    };

#ifdef GAIN_SIMD_AVX2
    struct Avx2 {
        using V                    = __m256;
        static constexpr int Width = 8;
        static V Load(const float* p) noexcept { return _mm256_loadu_ps(p); }
        static void Store(float* p, V v) noexcept { _mm256_storeu_ps(p, v); }
        static V Set(float x) noexcept { return _mm256_set1_ps(x); }
        static V Mul(V a, V b) noexcept { return _mm256_mul_ps(a, b); }
        static V Add(V a, V b) noexcept { return _mm256_add_ps(a, b); }
    };
#endif

    template <typename T>
    void YuleFirSimd(const sample_t* input, sample_t* s1, std::size_t count, const float* kernel) noexcept
    {
        using V = typename T::V;
        V k[11];
        for (int j = 0; j < 11; ++j)
            k[j] = T::Set(kernel[j]);
        std::size_t i = 0;
        for (; i + T::Width <= count; i += T::Width) {
            const sample_t* p   = input + i;
            const V         y0  = T::Mul(T::Load(p - 10), k[0]);
            const V         y2  = T::Mul(T::Load(p - 9), k[1]);
            const V         y4  = T::Mul(T::Load(p - 8), k[2]);
            const V         y6  = T::Mul(T::Load(p - 7), k[3]);
            const V         s00 = T::Add(T::Add(T::Add(y0, y2), y4), y6);
            const V         y8  = T::Mul(T::Load(p - 6), k[4]);
            const V         yA  = T::Mul(T::Load(p - 5), k[5]);
            const V         yC  = T::Mul(T::Load(p - 4), k[6]);
            const V         yE  = T::Mul(T::Load(p - 3), k[7]);
            const V         s01 = T::Add(T::Add(T::Add(y8, yA), yC), yE);
            const V         yG  = T::Add(T::Mul(T::Load(p - 2), k[8]), T::Mul(T::Load(p - 1), k[9]));
            const V         yK  = T::Mul(T::Load(p), k[10]);
            T::Store(s1 + i, T::Add(T::Add(T::Add(s00, s01), yG), yK));
        }
        for (; i < count; ++i)
            s1[i] = YuleFir(input + i, kernel);
    }

} // namespace

void GAIN_SIMD_YULE(const sample_t* input, sample_t* s1, std::size_t count, const float* kernel) noexcept
{
    YuleFirSimd<GAIN_SIMD_WIDE>(input, s1, count, kernel);
}
