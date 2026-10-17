// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

// The SIMD fast Hartley transform, included by FhtSse41.cpp and FhtAvx2.cpp, which set FHT_SIMD_NAME
// and FHT_SIMD_WIDE (the traits of the widest vector). Everything here has internal linkage, so the
// AVX2 file's code cannot be merged into other translation units.
//
// LAME's fht_SSE2 vectorizes one butterfly: it gathers eight values from scattered addresses and
// stores them back one by one. This version vectorizes across neighbouring twiddle indices i
// instead: for lanes i .. i+W-1 the values fi[...] are contiguous and ascending, gi[...] are
// contiguous and descending (one reversal). Each lane computes exactly the scalar butterfly of
// fht, so the result is bit-identical; the twiddles come from fht's recursion, computed once.

namespace {

    // the twiddles of every stage, by fht's recursion (they do not depend on the input)
    struct Twiddles {
        alignas(32) std::array<std::array<float, 256>, FhtTriSize> c1;
        alignas(32) std::array<std::array<float, 256>, FhtTriSize> s1;
        alignas(32) std::array<std::array<float, 256>, FhtTriSize> c2;
        alignas(32) std::array<std::array<float, 256>, FhtTriSize> s2;

        Twiddles() noexcept
        {
            for (std::int32_t stage = 0; stage < FhtTriSize; ++stage) {
                const std::int32_t kx  = 2 << (2 * stage); // 2, 8, 32, 128
                const float*       tri = FhtCostab.data() + 2 * stage;
                float              c   = tri[0];
                float              s   = tri[1];
                for (std::int32_t i = 1; i < kx; i++) {
                    c2[stage][i]  = 1 - (2 * s) * s;
                    s2[stage][i]  = (2 * s) * c;
                    c1[stage][i]  = c;
                    s1[stage][i]  = s;
                    const float t = c;
                    c             = t * tri[0] - s * tri[1];
                    s             = t * tri[1] + s * tri[0];
                }
            }
        }
    };

    const Twiddles& GetTwiddles() noexcept
    {
        static const Twiddles twiddles;
        return twiddles;
    }

    struct Sse {
        using V                     = __m128;
        static constexpr int Width  = 4;
        static V Load(const float* p) noexcept { return _mm_loadu_ps(p); }
        static void Store(float* p, V v) noexcept { _mm_storeu_ps(p, v); }
        static V Mul(V a, V b) noexcept { return _mm_mul_ps(a, b); }
        static V Add(V a, V b) noexcept { return _mm_add_ps(a, b); }
        static V Sub(V a, V b) noexcept { return _mm_sub_ps(a, b); }
        static V Reverse(V v) noexcept { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 1, 2, 3)); }
    };

#ifdef FHT_SIMD_AVX2
    struct Avx2 {
        using V                     = __m256;
        static constexpr int Width  = 8;
        static V Load(const float* p) noexcept { return _mm256_loadu_ps(p); }
        static void Store(float* p, V v) noexcept { _mm256_storeu_ps(p, v); }
        static V Mul(V a, V b) noexcept { return _mm256_mul_ps(a, b); }
        static V Add(V a, V b) noexcept { return _mm256_add_ps(a, b); }
        static V Sub(V a, V b) noexcept { return _mm256_sub_ps(a, b); }
        static V Reverse(V v) noexcept { return _mm256_permutevar8x32_ps(v, _mm256_setr_epi32(7, 6, 5, 4, 3, 2, 1, 0)); }
    };
#endif

    // the butterflies of twiddle indices i .. i+W-1 in one block; fht's butterfly in each lane
    template <class O>
    void ButterflyLanes(float* base, std::int32_t i, std::int32_t k1, std::int32_t k2, std::int32_t k3, const float* tc1, const float* ts1, const float* tc2,
                        const float* ts2) noexcept
    {
        using V          = typename O::V;
        float* const f   = base + i;                      // lane l: fi = base + i + l
        float* const g   = base + k1 - i - (O::Width - 1); // lane l: gi = base + k1 - i - l, so g reversed
        const V      c1  = O::Load(tc1 + i);
        const V      s1  = O::Load(ts1 + i);
        const V      c2  = O::Load(tc2 + i);
        const V      s2  = O::Load(ts2 + i);
        const V      fi0 = O::Load(f);
        const V      fi1 = O::Load(f + k1);
        const V      fi2 = O::Load(f + k2);
        const V      fi3 = O::Load(f + k3);
        const V      gi0 = O::Reverse(O::Load(g));
        const V      gi1 = O::Reverse(O::Load(g + k1));
        const V      gi2 = O::Reverse(O::Load(g + k2));
        const V      gi3 = O::Reverse(O::Load(g + k3));

        V       b  = O::Sub(O::Mul(s2, fi1), O::Mul(c2, gi1));
        V       a  = O::Add(O::Mul(c2, fi1), O::Mul(s2, gi1));
        const V f1 = O::Sub(fi0, a);
        const V f0 = O::Add(fi0, a);
        const V g1 = O::Sub(gi0, b);
        const V g0 = O::Add(gi0, b);
        b          = O::Sub(O::Mul(s2, fi3), O::Mul(c2, gi3));
        a          = O::Add(O::Mul(c2, fi3), O::Mul(s2, gi3));
        const V f3 = O::Sub(fi2, a);
        const V f2 = O::Add(fi2, a);
        const V g3 = O::Sub(gi2, b);
        const V g2 = O::Add(gi2, b);
        b          = O::Sub(O::Mul(s1, f2), O::Mul(c1, g3));
        a          = O::Add(O::Mul(c1, f2), O::Mul(s1, g3));
        O::Store(f + k2, O::Sub(f0, a));
        O::Store(f, O::Add(f0, a));
        O::Store(g + k3, O::Reverse(O::Sub(g1, b)));
        O::Store(g + k1, O::Reverse(O::Add(g1, b)));
        b = O::Sub(O::Mul(c1, g2), O::Mul(s1, f3));
        a = O::Add(O::Mul(s1, g2), O::Mul(c1, f3));
        O::Store(g + k2, O::Reverse(O::Sub(g0, a)));
        O::Store(g, O::Reverse(O::Add(g0, a)));
        O::Store(f + k3, O::Sub(f1, b));
        O::Store(f + k1, O::Add(f1, b));
    }

    // fht's butterfly for one twiddle index (the indices that do not fill a vector)
    void ButterflyScalar(float* fi, float* gi, std::int32_t k1, std::int32_t k2, std::int32_t k3, float c1, float s1, float c2, float s2) noexcept
    {
        float a, b, g0, f0, f1, g1, f2, g2, f3, g3;
        b      = s2 * fi[k1] - c2 * gi[k1];
        a      = c2 * fi[k1] + s2 * gi[k1];
        f1     = fi[0] - a;
        f0     = fi[0] + a;
        g1     = gi[0] - b;
        g0     = gi[0] + b;
        b      = s2 * fi[k3] - c2 * gi[k3];
        a      = c2 * fi[k3] + s2 * gi[k3];
        f3     = fi[k2] - a;
        f2     = fi[k2] + a;
        g3     = gi[k2] - b;
        g2     = gi[k2] + b;
        b      = s1 * f2 - c1 * g3;
        a      = c1 * f2 + s1 * g3;
        fi[k2] = f0 - a;
        fi[0]  = f0 + a;
        gi[k3] = g1 - b;
        gi[k1] = g1 + b;
        b      = c1 * g2 - s1 * f3;
        a      = s1 * g2 + c1 * f3;
        gi[k2] = g0 - a;
        gi[0]  = g0 + a;
        fi[k3] = f1 - b;
        fi[k1] = f1 + b;
    }

} // namespace

void FHT_SIMD_NAME(float* fz, std::int32_t n) noexcept
{
    const Twiddles& tw  = GetTwiddles();
    const float*    tri = FhtCostab.data();
    n <<= 1; // to get BLKSIZE, because of 3DNow! ASM routine
    float* const fn    = fz + n;
    std::int32_t k4    = 4;
    std::int32_t stage = 0;
    do {
        const std::int32_t kx = k4 >> 1;
        const std::int32_t k1 = k4;
        const std::int32_t k2 = k4 << 1;
        const std::int32_t k3 = k2 + k1;
        k4                    = k2 << 1;

        // the first butterflies of each block, as in fht
        float* fi = fz;
        float* gi = fi + kx;
        do {
            float f0, f1, f2, f3;
            f1     = fi[0] - fi[k1];
            f0     = fi[0] + fi[k1];
            f3     = fi[k2] - fi[k3];
            f2     = fi[k2] + fi[k3];
            fi[k2] = f0 - f2;
            fi[0]  = f0 + f2;
            fi[k3] = f1 - f3;
            fi[k1] = f1 + f3;
            f1     = gi[0] - gi[k1];
            f0     = gi[0] + gi[k1];
            f3     = static_cast<float>(SQRT2 * gi[k3]);
            f2     = static_cast<float>(SQRT2 * gi[k2]);
            gi[k2] = f0 - f2;
            gi[0]  = f0 + f2;
            gi[k3] = f1 - f3;
            gi[k1] = f1 + f3;
            gi += k4;
            fi += k4;
        } while (fi < fn);

        // the butterflies with twiddles; each index i touches its own values, so the order of
        // indices and blocks does not matter
        const float* const tc1 = tw.c1[stage].data();
        const float* const ts1 = tw.s1[stage].data();
        const float* const tc2 = tw.c2[stage].data();
        const float* const ts2 = tw.s2[stage].data();
        for (float* base = fz; base < fn; base += k4) {
            std::int32_t i = 1;
            for (; i + FHT_SIMD_WIDE::Width - 1 < kx; i += FHT_SIMD_WIDE::Width)
                ButterflyLanes<FHT_SIMD_WIDE>(base, i, k1, k2, k3, tc1, ts1, tc2, ts2);
#ifdef FHT_SIMD_AVX2
            for (; i + Sse::Width - 1 < kx; i += Sse::Width)
                ButterflyLanes<Sse>(base, i, k1, k2, k3, tc1, ts1, tc2, ts2);
#endif
            for (; i < kx; i++)
                ButterflyScalar(base + i, base + k1 - i, k1, k2, k3, tc1[i], ts1[i], tc2[i], ts2[i]);
        }
        tri += 2;
        ++stage;
    } while (k4 < n);
}
