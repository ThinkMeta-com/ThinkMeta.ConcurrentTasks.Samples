// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

// Included at the end of PixelPhaseImpl.inc: the scalar reference kernels of the pixel phase and
// the kernel self-test (PixelKernelSelfTest), which runs every SIMD kernel of PixelPhaseImpl.inc
// against its reference. The encoder itself uses only the SIMD kernels.
//
// The references are jpegli's operations lane by lane: every value goes through the same float
// operations in the same order as jpegli on the Highway target of the feature set (LaneMath.h).

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    namespace {

        // ------------------------------------------------------------------ scalar reference kernels

        // ZeroIfNegative: Highway tests the sign bit.
        float ZeroIfNegative(float v) noexcept { return std::signbit(v) ? 0.0f : v; }

        // jpegli: RatioOfDerivativesOfCubicRootToSimpleGamma() (adaptive_quantization.cc).
        template <SimdLevel L>
        float RatioOfDerivatives(float v, bool invert) noexcept
        {
            v               = ZeroIfNegative(v);
            const float v2  = v * v;
            const float num = Lane<L>::MulAdd(NumMul, v2, NumOffset);
            const float den = Lane<L>::MulAdd(DenMul * v, v2, VOffset);
            return invert ? num / den : den / num;
        }

        // jpegli: MaskingSqrt() (adaptive_quantization.cc).
        template <SimdLevel L>
        float MaskingSqrt(float v) noexcept
        {
            return 0.25f * std::sqrt(Lane<L>::MulAdd(v, std::sqrt(MaskMulV), 28.0f));
        }

        // jpegli: DCT1DImpl<N> (dct-inl.h) on one column of N values.
        template <SimdLevel L, std::int32_t N>
        struct Dct1DImpl {
            static void Run(float* mem) noexcept
            {
                float tmp[N];
                for (std::int32_t i = 0; i < N / 2; ++i)
                    tmp[i] = mem[i] + mem[N - 1 - i];
                Dct1DImpl<L, N / 2>::Run(tmp);
                for (std::int32_t i = 0; i < N / 2; ++i)
                    tmp[N / 2 + i] = mem[i] - mem[N - 1 - i];
                const float* mul = N == 8 ? WcMultipliers8 : WcMultipliers4;
                for (std::int32_t i = 0; i < N / 2; ++i)
                    tmp[N / 2 + i] = tmp[N / 2 + i] * mul[i];
                Dct1DImpl<L, N / 2>::Run(tmp + N / 2);
                float* b = tmp + N / 2; // B<N / 2>
                b[0]     = Lane<L>::MulAdd(b[0], Sqrt2, b[1]);
                for (std::int32_t i = 1; i + 1 < N / 2; ++i)
                    b[i] = b[i] + b[i + 1];
                for (std::int32_t i = 0; i < N / 2; ++i) // InverseEvenOdd
                    mem[2 * i] = tmp[i];
                for (std::int32_t i = N / 2; i < N; ++i)
                    mem[2 * (i - N / 2) + 1] = tmp[i];
            }
        };
        template <SimdLevel L>
        struct Dct1DImpl<L, 2> {
            static void Run(float* mem) noexcept
            {
                const float in1 = mem[0];
                const float in2 = mem[1];
                mem[0]          = in1 + in2;
                mem[1]          = in1 - in2;
            }
        };

        // jpegli: TransformFromPixels() (dct-inl.h): DCT of the columns, transpose, DCT of the
        // columns, transpose.
        template <SimdLevel L>
        void TransformFromPixels(const float* pixels, std::size_t stride, float* coeffs) noexcept
        {
            float scratch[64];
            float col[8];
            for (std::int32_t c = 0; c < 8; ++c) {
                for (std::int32_t r = 0; r < 8; ++r)
                    col[r] = pixels[static_cast<std::size_t>(r) * stride + static_cast<std::size_t>(c)];
                Dct1DImpl<L, 8>::Run(col);
                for (std::int32_t r = 0; r < 8; ++r)
                    scratch[r * 8 + c] = (1.0f / 8) * col[r];
            }
            for (std::int32_t r = 0; r < 8; ++r)
                for (std::int32_t c = 0; c < 8; ++c)
                    coeffs[c * 8 + r] = scratch[r * 8 + c];
            for (std::int32_t c = 0; c < 8; ++c) {
                for (std::int32_t r = 0; r < 8; ++r)
                    col[r] = coeffs[r * 8 + c];
                Dct1DImpl<L, 8>::Run(col);
                for (std::int32_t r = 0; r < 8; ++r)
                    scratch[r * 8 + c] = (1.0f / 8) * col[r];
            }
            for (std::int32_t r = 0; r < 8; ++r)
                for (std::int32_t c = 0; c < 8; ++c)
                    coeffs[c * 8 + r] = scratch[r * 8 + c];
        }

        // jpegli: ComputeCoefficientBlock() and QuantizeBlock() (dct-inl.h); reference of
        // QuantizeBlockV, the result in zigzag order.
        template <SimdLevel L>
        void QuantizeBlockTo(const float* pixels, std::size_t stride, const float* qmc, float aq, const float* zbOffset, const float* zbMul, std::int16_t* out) noexcept
        {
            float        dct[64];
            std::int32_t block[64];
            TransformFromPixels<L>(pixels, stride, dct);
            for (std::int32_t k = 0; k < 64; ++k) {
                const float qval      = dct[k] * qmc[k];
                const float threshold = zbOffset[k] + zbMul[k] * aq;
                block[k]              = std::fabs(qval) >= threshold ? static_cast<std::int32_t>(std::nearbyint(qval)) : 0;
            }
            const float dc = (dct[0] - DcBias) * qmc[0];
            block[0]       = static_cast<std::int32_t>(std::round(dc));
            for (std::int32_t k = 0; k < 64; ++k)
                out[k] = static_cast<std::int16_t>(block[NaturalOrder[static_cast<std::size_t>(k)]]);
        }

        // jpegli: ComputePreErosion() (adaptive_quantization.cc) for one pre-erosion row p (input
        // rows 4p..4p+3 of the Y window); reference of PreErosionRowV.
        template <SimdLevel L>
        void PreErosionRow(Plane& y, std::int32_t p, std::int32_t xsize, float* diff, float* out) noexcept
        {
            for (std::int32_t iy = 0; iy < 4; ++iy) {
                const std::int32_t row = 4 * p + iy;
                const float*       in  = y.Row(row);
                const float*       in1 = y.Row(row + 1);
                const float*       in2 = y.Row(row - 1);
                for (std::int32_t x = 0; x < xsize; ++x) {
                    const float base    = 0.25f * ((in[x + 1] + in[x - 1]) + (in2[x] + in1[x]));
                    const float gammacv = RatioOfDerivatives<L>(in[x] + MatchGamma, false);
                    float       d       = gammacv * (in[x] - base);
                    d                   = d * d;
                    d                   = MinPs(d, 0.2f);
                    d                   = MaskingSqrt<L>(d);
                    if (iy != 0)
                        d = d + diff[x];
                    diff[x] = d;
                }
            }
            const std::int32_t xsizeOut = xsize / 4;
            for (std::int32_t x = 0; x < xsizeOut; ++x)
                out[x] = (diff[x * 4] + diff[x * 4 + 1] + diff[x * 4 + 2] + diff[x * 4 + 3]) * 0.25f;
        }

        // jpegli: Sort4(), UpdateMin4() and FuzzyErosion() (adaptive_quantization.cc) for one
        // output row of the 2x-downsampled grid; reference of FuzzyErosionRowV.
        void FuzzyErosionRow(const float* t, const float* m, const float* b, std::int32_t xsize, float* out) noexcept
        {
            for (std::int32_t x = 0; x < xsize; ++x) {
                float min0 = m[x], min1 = m[x - 1], min2 = m[x + 1], min3 = t[x - 1];
                {
                    const float tmp0 = MinPs(min0, min1), tmp1 = MaxPs(min0, min1);
                    const float tmp2 = MinPs(min2, min3), tmp3 = MaxPs(min2, min3);
                    const float tmp4 = MaxPs(tmp0, tmp2), tmp5 = MinPs(tmp1, tmp3);
                    min0 = MinPs(tmp0, tmp2);
                    min1 = MinPs(tmp4, tmp5);
                    min2 = MaxPs(tmp4, tmp5);
                    min3 = MaxPs(tmp1, tmp3);
                }
                for (const float v : {t[x], t[x + 1], b[x - 1], b[x], b[x + 1]}) {
                    const float tmp0 = MaxPs(min0, v);
                    const float tmp1 = MaxPs(min1, tmp0);
                    const float tmp2 = MaxPs(min2, tmp1);
                    min0             = MinPs(min0, v);
                    min1             = MinPs(min1, tmp0);
                    min2             = MinPs(min2, tmp1);
                    min3             = MinPs(min3, tmp2);
                }
                out[x] = (0.125f * min0 + 0.075f * min1) + (0.06f * min2 + 0.05f * min3);
            }
        }

        // jpegli: HfModulation() and GammaModulation() (adaptive_quantization.cc) of one 8x8 block
        // (x, y in pixels of the Y window), with the per-lane accumulation of a vector of
        // Lane<L>::Width lanes; reference of BlockModulationsV.
        template <SimdLevel L>
        float BlockModulations(Plane& y, std::int32_t x, std::int32_t yy, float outVal) noexcept
        {
            constexpr std::int32_t W = Lane<L>::Width;
            float                  sum[8]{};
            for (std::int32_t dy = 0; dy < 8; ++dy) {
                const float* row     = y.Row(yy + dy) + x;
                const float* rowNext = dy == 7 ? row : y.Row(yy + dy + 1) + x;
                for (std::int32_t dx = 0; dx < 8; dx += W) {
                    for (std::int32_t l = 0; l < W; ++l) {
                        const std::int32_t c = dx + l;
                        const float        p = row[c];
                        sum[l]               = sum[l] + (c == 7 ? 0.0f : std::fabs(p - row[c + 1]));
                        sum[l]               = sum[l] + std::fabs(p - rowNext[c]);
                    }
                }
            }
            outVal = Lane<L>::MulAdd(Lane<L>::SumOfLanes(sum), SumCoeff, outVal);

            float ratio[8]{};
            for (std::int32_t dy = 0; dy < 8; ++dy) {
                const float* row = y.Row(yy + dy) + x;
                for (std::int32_t dx = 0; dx < 8; dx += W)
                    for (std::int32_t l = 0; l < W; ++l)
                        ratio[l] = ratio[l] + RatioOfDerivatives<L>(row[dx + l] + GammaBias, true);
            }
            const float overall = Lane<L>::SumOfLanes(ratio) * GammaScale;
            return Lane<L>::MulAdd(GammaK, FastLog2f<L>(overall), outVal);
        }

        // ------------------------------------------------------------------ kernel self-test
        // New in ctJpeg, not in jpegli: every SIMD kernel against its scalar reference on random
        // data, bit for bit.

        std::uint32_t NextRandom(std::uint32_t& state) noexcept
        {
            state = state * 1664525u + 1013904223u;
            return state >> 8;
        }

        bool SameBits(const float* a, const float* b, std::int32_t n) noexcept { return std::memcmp(a, b, static_cast<std::size_t>(n) * sizeof(float)) == 0; }

        template <SimdLevel L>
        bool KernelSelfTest(std::uint32_t seed) noexcept
        {
            constexpr std::int32_t W = 64, H = 24, Stride = W + 2 + 16;
            static float           planeData[H * Stride];
            std::uint32_t          state = seed;
            for (float& v : planeData)
                v = static_cast<float>(NextRandom(state) % 256) + (NextRandom(state) % 4 == 0 ? 0.25f : 0.0f);
            Plane plane{planeData, -4, Stride};

            // DCT and quantization
            float qmc[64], zbo[64], zbm[64];
            for (std::int32_t k = 0; k < 64; ++k) {
                qmc[k] = 8.0f / static_cast<float>(1 + NextRandom(state) % 40);
                zbo[k] = k == 0 ? 0.0f : 0.5f;
                zbm[k] = k == 0 ? 0.0f : static_cast<float>(NextRandom(state) % 100) / 100.0f;
            }
            std::int16_t a[64], b[64];
            for (std::int32_t bx = 0; bx < 4; ++bx) {
                const float aq = static_cast<float>(NextRandom(state) % 300) / 100.0f;
                QuantizeBlockTo<L>(plane.Row(0) + bx * 8, Stride, qmc, aq, zbo, zbm, a);
                QuantizeBlockV<L>(plane.Row(0) + bx * 8, Stride, qmc, aq, zbo, zbm, b);
                if (std::memcmp(a, b, sizeof(a)) != 0)
                    return false;
            }
            // pre-erosion, fuzzy erosion, block modulations
            static float diffA[W + 16], diffB[W + 16], preA[W + 16], preB[W + 16];
            PreErosionRow<L>(plane, 2, W, diffA, preA);
            PreErosionRowV<L>(plane, 2, 0, W, diffB, preB);
            if (!SameBits(diffA, diffB, W) || !SameBits(preA, preB, W / 4))
                return false;
            static float fa[W + 16], fb[W + 16];
            FuzzyErosionRow(plane.Row(0) + 1, plane.Row(1) + 1, plane.Row(2) + 1, 30, fa);
            FuzzyErosionRowV<L>(plane.Row(0) + 1, plane.Row(1) + 1, plane.Row(2) + 1, 30, fb);
            if (!SameBits(fa, fb, 30))
                return false;
            for (std::int32_t bx = 0; bx < 4; ++bx) {
                const float outVal = static_cast<float>(NextRandom(state) % 1000) / 500.0f - 1.0f;
                const float ra     = BlockModulations<L>(plane, bx * 8, 0, outVal);
                const float rb     = BlockModulationsV<L>(plane, bx * 8, 0, outVal);
                if (!SameBits(&ra, &rb, 1))
                    return false;
            }
            // color transform (ExtRGBToYCbCr)
            std::uint8_t rgb[3 * W];
            for (std::uint8_t& v : rgb)
                v = static_cast<std::uint8_t>(NextRandom(state));
            static float tmp[3 * (W + 16)], y1[W + 16], cb1[W + 16], cr1[W + 16], y2[W + 16], cb2[W + 16], cr2[W + 16];
            ColorRowV<L>(rgb, 37, tmp, W + 16, y2, cb2, cr2);
            const float DiffR = ChromaAmpR + LumaR, DiffB = ChromaAmpB + LumaB;
            const float NormR = 1.0f / (ChromaAmpR + (LumaG + LumaB)), NormB = 1.0f / (LumaR + (LumaG + ChromaAmpB));
            for (std::int32_t x = 0; x < 37; ++x) {
                const float r = rgb[3 * x], g = rgb[3 * x + 1], bb = rgb[3 * x + 2];
                const float yBase = r * LumaR + (g * LumaG + bb * LumaB);
                y1[x]             = yBase;
                cb1[x]            = Lane<L>::MulAdd(bb * DiffB - yBase, NormB, ChromaOffset);
                cr1[x]            = Lane<L>::MulAdd(r * DiffR - yBase, NormR, ChromaOffset);
            }
            return SameBits(y1, y2, 37) && SameBits(cb1, cb2, 37) && SameBits(cr1, cr2, 37);
        }

    } // namespace

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
