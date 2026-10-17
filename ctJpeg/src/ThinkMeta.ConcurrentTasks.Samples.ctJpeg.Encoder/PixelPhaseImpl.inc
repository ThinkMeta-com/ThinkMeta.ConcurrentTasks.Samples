// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

// Included by PixelPhaseSse2.cpp and PixelPhaseAvx2.cpp (compiled with /arch:AVX2), one
// instantiation of the pixel phase per feature set.

#include "PixelPhase.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <Windows.h>

#include "LaneMath.h"
#include "VecMath.h"

// Port of jpegli's pixel phase (encode.cc ReadInputRow/PadInputBuffer/ProcessiMCURow,
// color_transform.cc, downsample.cc, adaptive_quantization.cc, dct-inl.h, encode_streaming.cc)
// as a scalar reference: every value goes through the same float operations in the same order as
// jpegli on the Highway target of the feature set (LaneMath.h: MulAdd fused or not, the lane
// order of SumOfLanes, the per-lane accumulation of the block sums).
//
// Padding model: jpegli pads every color-converted row to the right with its last value and one
// value to the left, repeats the last row below the image and row 0 above it. Every access here is
// therefore a read of the color-converted plane at clamp(x, 0, width-1), clamp(y, 0, height-1);
// the pre-erosion rows are clamped to [0, 2 * ysizeBlocks - 1] the same way (CopyRow/PadRow).

// The implicit double -> float conversions of the constants are jpegli's and part of the
// bit-exact result.
#pragma warning(disable : 4244 4305)

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    namespace {

#pragma warning(push)
#pragma warning(disable : 4305) // jpegli spells these as double literals in float arrays
        // WcMultipliers<4> and <8> of dct-inl.h
        constexpr float WcMultipliers4[] = {0.541196100146197, 1.3065629648763764};
        constexpr float WcMultipliers8[] = {0.5097955791041592, 0.6013448869350453, 0.8999762231364156, 2.5629154477415055};
        constexpr float InvLog2e         = 0.6931471805599453; // kInvLog2e
#pragma warning(pop)

        constexpr float InputScaling = 1.0f / 255.0f; // kInputScaling

        // Constants of adaptive_quantization.cc, same expressions and types.
        const float     SGmul      = 226.0480446705883f;
        const float     SGmul2     = 1.0f / 73.377132366608819f;
        const float     SGRetMul   = SGmul2 * 18.6580932135f * InvLog2e;
        const float     SGVOffset  = 7.14672470003f;
        const float     Epsilon    = 1e-2;
        const float     NumOffset  = Epsilon / InputScaling / InputScaling;
        const float     NumMul     = SGRetMul * 3 * SGmul;
        const float     VOffset    = (SGVOffset * InvLog2e + Epsilon) / InputScaling;
        const float     DenMul     = InvLog2e * SGmul * InputScaling * InputScaling;
        const float     MatchGamma = 0.019 / InputScaling; // match_gamma_offset
        const float     MaskMul    = 211.50759899638012f;  // MaskingSqrt kMul
        const float     MaskMulV   = MaskMul * 1e8;         // Set(d, kMul * 1e8)
        const float     SumCoeff   = -2.0052193233688884f * InputScaling / 112.0;
        const float     GammaBias  = 0.16f / InputScaling;
        const float     GammaScale = InputScaling / 64.0f;
        const float     GammaK     = -0.15526878023684174f * InvLog2e;

        std::int32_t Clamp(std::int32_t v, std::int32_t lo, std::int32_t hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

        // ZeroIfNegative: Highway tests the sign bit.
        float ZeroIfNegative(float v) noexcept { return std::signbit(v) ? 0.0f : v; }

        // Highway Min/Max on x86 (minps/maxps): the first operand if it compares less/greater.
        float MinPs(float a, float b) noexcept { return a < b ? a : b; }
        float MaxPs(float a, float b) noexcept { return a > b ? a : b; }

        template <SimdLevel L>
        float RatioOfDerivatives(float v, bool invert) noexcept
        {
            v              = ZeroIfNegative(v);
            const float v2 = v * v;
            const float num = Lane<L>::MulAdd(NumMul, v2, NumOffset);
            const float den = Lane<L>::MulAdd(DenMul * v, v2, VOffset);
            return invert ? num / den : den / num;
        }

        template <SimdLevel L>
        float MaskingSqrt(float v) noexcept
        {
            return 0.25f * std::sqrt(Lane<L>::MulAdd(v, std::sqrt(MaskMulV), 28.0f));
        }

        // FastLog2f with the 2,2 rational polynomial (EvalRationalPolynomial, Div).
        template <SimdLevel L>
        float FastLog2f(float x) noexcept
        {
            constexpr float p[3] = {-1.8503833400518310E-06f, 1.4287160470083755E+00f, 7.4245873327820566E-01f};
            constexpr float q[3] = {9.9032814277590719E-01f, 1.0096718572241148E+00f, 1.7409343003366853E-01f};
            std::int32_t    xBits;
            std::memcpy(&xBits, &x, sizeof(xBits));
            const std::int32_t expBits    = xBits - 0x3f2aaaab;
            const std::int32_t expShifted = expBits >> 23;
            const std::int32_t mBits      = xBits - static_cast<std::int32_t>(static_cast<std::uint32_t>(expShifted) << 23);
            float              mantissa;
            std::memcpy(&mantissa, &mBits, sizeof(mantissa));
            const float expVal = static_cast<float>(expShifted);
            const float t      = mantissa - 1.0f;
            float       yp     = p[2];
            float       yq     = q[2];
            yp                 = Lane<L>::MulAdd(yp, t, p[1]);
            yq                 = Lane<L>::MulAdd(yq, t, q[1]);
            yp                 = Lane<L>::MulAdd(yp, t, p[0]);
            yq                 = Lane<L>::MulAdd(yq, t, q[0]);
            return yp / yq + expVal;
        }

        template <SimdLevel L>
        float FastPow2f(float x) noexcept
        {
            const float        floorx = std::floor(x);
            const std::int32_t eBits  = static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::int32_t>(floorx) + 127) << 23);
            float              e;
            std::memcpy(&e, &eBits, sizeof(e));
            const float frac = x - floorx;
            float       num  = frac + 1.01749063e+01f;
            num              = Lane<L>::MulAdd(num, frac, 4.88687798e+01f);
            num              = Lane<L>::MulAdd(num, frac, 9.85506591e+01f);
            num              = num * e;
            float den        = Lane<L>::MulAdd(frac, 2.10242958e-01f, -2.22328856e-02f);
            den              = Lane<L>::MulAdd(den, frac, -1.94414990e+01f);
            den              = Lane<L>::MulAdd(den, frac, 9.85506633e+01f);
            return num / den;
        }

        template <SimdLevel L>
        float ComputeMask(float outVal) noexcept
        {
            const float Base    = -0.74174993f;
            const float Mul4    = 3.2353257320940401f;
            const float Mul2    = 12.906028311180409f;
            const float Offset2 = 305.04035728311436f;
            const float Mul3    = 5.0220313103171232f;
            const float Offset3 = 2.1925739705298404f;
            const float Offset4 = 0.25f * Offset3;
            const float Mul0    = 0.74760422233706747f;
            const float v1      = MaxPs(outVal * Mul0, 1e-3f);
            const float v2      = 1.0f / (v1 + Offset2);
            const float v3      = 1.0f / Lane<L>::MulAdd(v1, v1, Offset3);
            const float v4      = 1.0f / Lane<L>::MulAdd(v1, v1, Offset4);
            return Base + Lane<L>::MulAdd(Mul4, v4, Lane<L>::MulAdd(Mul2, v2, Mul3 * v3));
        }

        // A window of a color-converted plane, columns [x0 - 1, x0 - 1 + stride) indexed by the
        // absolute column: linear rows from y0, or a ring of ring rows indexed by the absolute row
        // (ring a multiple of 8, so the rows of a block row are contiguous).
        struct Plane {
            float*       data   = nullptr;
            std::int32_t y0     = 0;
            std::int32_t stride = 0;
            std::int32_t ring   = 0;
            std::int32_t x0     = 0;
            float*       Row(std::int32_t y) noexcept
            {
                const std::int32_t i = ring > 0 ? ((y % ring) + ring) % ring : y - y0;
                return data + static_cast<std::ptrdiff_t>(i) * stride + 1 - x0;
            }
        };

        // DCT1DImpl<N> of dct-inl.h on one column of N values.
        template <SimdLevel L, int N>
        struct Dct1DImpl {
            static void Run(float* mem) noexcept
            {
                float tmp[N];
                for (int i = 0; i < N / 2; ++i)
                    tmp[i] = mem[i] + mem[N - 1 - i];
                Dct1DImpl<L, N / 2>::Run(tmp);
                for (int i = 0; i < N / 2; ++i)
                    tmp[N / 2 + i] = mem[i] - mem[N - 1 - i];
                const float* mul = N == 8 ? WcMultipliers8 : WcMultipliers4;
                for (int i = 0; i < N / 2; ++i)
                    tmp[N / 2 + i] = tmp[N / 2 + i] * mul[i];
                Dct1DImpl<L, N / 2>::Run(tmp + N / 2);
                float*          b     = tmp + N / 2; // B<N / 2>
                constexpr float Sqrt2 = 1.41421356237f;
                b[0]                  = Lane<L>::MulAdd(b[0], Sqrt2, b[1]);
                for (int i = 1; i + 1 < N / 2; ++i)
                    b[i] = b[i] + b[i + 1];
                for (int i = 0; i < N / 2; ++i) // InverseEvenOdd
                    mem[2 * i] = tmp[i];
                for (int i = N / 2; i < N; ++i)
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

        // TransformFromPixels: DCT of the columns, transpose, DCT of the columns, transpose.
        template <SimdLevel L>
        void TransformFromPixels(const float* pixels, std::size_t stride, float* coeffs) noexcept
        {
            float scratch[64];
            float col[8];
            for (int c = 0; c < 8; ++c) {
                for (int r = 0; r < 8; ++r)
                    col[r] = pixels[static_cast<std::size_t>(r) * stride + static_cast<std::size_t>(c)];
                Dct1DImpl<L, 8>::Run(col);
                for (int r = 0; r < 8; ++r)
                    scratch[r * 8 + c] = (1.0f / 8) * col[r];
            }
            for (int r = 0; r < 8; ++r)
                for (int c = 0; c < 8; ++c)
                    coeffs[c * 8 + r] = scratch[r * 8 + c];
            for (int c = 0; c < 8; ++c) {
                for (int r = 0; r < 8; ++r)
                    col[r] = coeffs[r * 8 + c];
                Dct1DImpl<L, 8>::Run(col);
                for (int r = 0; r < 8; ++r)
                    scratch[r * 8 + c] = (1.0f / 8) * col[r];
            }
            for (int r = 0; r < 8; ++r)
                for (int c = 0; c < 8; ++c)
                    coeffs[c * 8 + r] = scratch[r * 8 + c];
        }

        // ComputeCoefficientBlock + QuantizeBlock; the result goes out in zigzag order. The DC dead
        // zone of jpegli compares with the previous DC under a threshold that is always 0 here
        // (zero_bias_offset[0] = zero_bias_mul[0] = 0), so the DC is always round(dc).
        template <SimdLevel L>
        void QuantizeBlockTo(const float* pixels, std::size_t stride, const float* qmc, float aq, const float* zbOffset, const float* zbMul, std::int16_t* out) noexcept
        {
            float        dct[64];
            std::int32_t block[64];
            TransformFromPixels<L>(pixels, stride, dct);
            for (int k = 0; k < 64; ++k) {
                const float qval      = dct[k] * qmc[k];
                const float threshold = zbOffset[k] + zbMul[k] * aq;
                block[k]              = std::fabs(qval) >= threshold ? static_cast<std::int32_t>(std::nearbyint(qval)) : 0;
            }
            const float dc = (dct[0] - 128.0f) * qmc[0];
            block[0]       = static_cast<std::int32_t>(std::round(dc));
            for (int k = 0; k < 64; ++k)
                out[k] = static_cast<std::int16_t>(block[NaturalOrder[static_cast<std::size_t>(k)]]);
        }

        // ComputePreErosion for one pre-erosion row p (input rows 4p..4p+3 of the Y window).
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

        // Sort4/UpdateMin4/FuzzyErosion for one output row of the 2x-downsampled grid.
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

        // HfModulation and GammaModulation of one 8x8 block (x, y in pixels of the Y window), with
        // the per-lane accumulation of a vector of Lane<L>::Width lanes.
        template <SimdLevel L>
        float BlockModulations(Plane& y, std::int32_t x, std::int32_t yy, float outVal) noexcept
        {
            constexpr int W = Lane<L>::Width;
            float         sum[8]{};
            for (int dy = 0; dy < 8; ++dy) {
                const float* row     = y.Row(yy + dy) + x;
                const float* rowNext = dy == 7 ? row : y.Row(yy + dy + 1) + x;
                for (int dx = 0; dx < 8; dx += W) {
                    for (int l = 0; l < W; ++l) {
                        const int   c = dx + l;
                        const float p = row[c];
                        sum[l]        = sum[l] + (c == 7 ? 0.0f : std::fabs(p - row[c + 1]));
                        sum[l]        = sum[l] + std::fabs(p - rowNext[c]);
                    }
                }
            }
            outVal = Lane<L>::MulAdd(Lane<L>::SumOfLanes(sum), SumCoeff, outVal);

            float ratio[8]{};
            for (int dy = 0; dy < 8; ++dy) {
                const float* row = y.Row(yy + dy) + x;
                for (int dx = 0; dx < 8; dx += W)
                    for (int l = 0; l < W; ++l)
                        ratio[l] = ratio[l] + RatioOfDerivatives<L>(row[dx + l] + GammaBias, true);
            }
            const float overall = Lane<L>::SumOfLanes(ratio) * GammaScale;
            return Lane<L>::MulAdd(GammaK, FastLog2f<L>(overall), outVal);
        }

        // ------------------------------------------------------------------ SIMD kernels
        // The same operations per lane as the scalar reference functions above, Width lanes at a
        // time; the block sums of BlockModulationsV keep jpegli's lane layout.

        template <SimdLevel L>
        __forceinline typename Vec<L>::T RatioV(typename Vec<L>::T v, bool invert) noexcept
        {
            using V        = Vec<L>;
            v              = V::ZeroIfNegative(v);
            const auto v2  = V::Mul(v, v);
            const auto num = V::MulAdd(V::Set(NumMul), v2, V::Set(NumOffset));
            const auto den = V::MulAdd(V::Mul(V::Set(DenMul), v), v2, V::Set(VOffset));
            return invert ? V::Div(num, den) : V::Div(den, num);
        }

        // DCT1DImpl<N> on N rows of Width columns.
        template <SimdLevel L, int N>
        struct DctV {
            using V = Vec<L>;
            using T = typename V::T;
            static __forceinline void Run(T* mem) noexcept
            {
                T tmp[N];
                for (int i = 0; i < N / 2; ++i)
                    tmp[i] = V::Add(mem[i], mem[N - 1 - i]);
                DctV<L, N / 2>::Run(tmp);
                for (int i = 0; i < N / 2; ++i)
                    tmp[N / 2 + i] = V::Sub(mem[i], mem[N - 1 - i]);
                const float* mul = N == 8 ? WcMultipliers8 : WcMultipliers4;
                for (int i = 0; i < N / 2; ++i)
                    tmp[N / 2 + i] = V::Mul(tmp[N / 2 + i], V::Set(mul[i]));
                DctV<L, N / 2>::Run(tmp + N / 2);
                T* b = tmp + N / 2;
                b[0] = V::MulAdd(b[0], V::Set(1.41421356237f), b[1]);
                for (int i = 1; i + 1 < N / 2; ++i)
                    b[i] = V::Add(b[i], b[i + 1]);
                for (int i = 0; i < N / 2; ++i)
                    mem[2 * i] = tmp[i];
                for (int i = N / 2; i < N; ++i)
                    mem[2 * (i - N / 2) + 1] = tmp[i];
            }
        };
        template <SimdLevel L>
        struct DctV<L, 2> {
            using V = Vec<L>;
            static __forceinline void Run(typename V::T* mem) noexcept
            {
                const auto in1 = mem[0];
                const auto in2 = mem[1];
                mem[0]         = V::Add(in1, in2);
                mem[1]         = V::Sub(in1, in2);
            }
        };

        template <SimdLevel L>
        void Dct1DColumnsV(const float* in, std::size_t stride, float* out) noexcept
        {
            using V = Vec<L>;
            for (int c0 = 0; c0 < 8; c0 += V::Width) {
                typename V::T rows[8];
                for (int r = 0; r < 8; ++r)
                    rows[r] = V::Load(in + static_cast<std::size_t>(r) * stride + static_cast<std::size_t>(c0));
                DctV<L, 8>::Run(rows);
                for (int r = 0; r < 8; ++r)
                    V::Store(out + r * 8 + c0, V::Mul(V::Set(1.0f / 8), rows[r]));
            }
        }

        // 8x8 transpose (Transpose8x8Block): pure data movement, the same for every feature set.
        template <SimdLevel L>
        __forceinline void Transpose8x8(const float* in, float* out) noexcept
        {
            if constexpr (L == SimdLevel::Avx2) {
                const __m256 r0 = _mm256_loadu_ps(in), r1 = _mm256_loadu_ps(in + 8), r2 = _mm256_loadu_ps(in + 16), r3 = _mm256_loadu_ps(in + 24);
                const __m256 r4 = _mm256_loadu_ps(in + 32), r5 = _mm256_loadu_ps(in + 40), r6 = _mm256_loadu_ps(in + 48), r7 = _mm256_loadu_ps(in + 56);
                const __m256 t0 = _mm256_unpacklo_ps(r0, r1), t1 = _mm256_unpackhi_ps(r0, r1), t2 = _mm256_unpacklo_ps(r2, r3), t3 = _mm256_unpackhi_ps(r2, r3);
                const __m256 t4 = _mm256_unpacklo_ps(r4, r5), t5 = _mm256_unpackhi_ps(r4, r5), t6 = _mm256_unpacklo_ps(r6, r7), t7 = _mm256_unpackhi_ps(r6, r7);
                const __m256 u0 = _mm256_shuffle_ps(t0, t2, 0x44), u1 = _mm256_shuffle_ps(t0, t2, 0xEE), u2 = _mm256_shuffle_ps(t1, t3, 0x44), u3 = _mm256_shuffle_ps(t1, t3, 0xEE);
                const __m256 u4 = _mm256_shuffle_ps(t4, t6, 0x44), u5 = _mm256_shuffle_ps(t4, t6, 0xEE), u6 = _mm256_shuffle_ps(t5, t7, 0x44), u7 = _mm256_shuffle_ps(t5, t7, 0xEE);
                _mm256_storeu_ps(out, _mm256_permute2f128_ps(u0, u4, 0x20));
                _mm256_storeu_ps(out + 8, _mm256_permute2f128_ps(u1, u5, 0x20));
                _mm256_storeu_ps(out + 16, _mm256_permute2f128_ps(u2, u6, 0x20));
                _mm256_storeu_ps(out + 24, _mm256_permute2f128_ps(u3, u7, 0x20));
                _mm256_storeu_ps(out + 32, _mm256_permute2f128_ps(u0, u4, 0x31));
                _mm256_storeu_ps(out + 40, _mm256_permute2f128_ps(u1, u5, 0x31));
                _mm256_storeu_ps(out + 48, _mm256_permute2f128_ps(u2, u6, 0x31));
                _mm256_storeu_ps(out + 56, _mm256_permute2f128_ps(u3, u7, 0x31));
            } else {
                for (int br = 0; br < 8; br += 4) {
                    for (int bc = 0; bc < 8; bc += 4) {
                        __m128 a = _mm_loadu_ps(in + br * 8 + bc), b = _mm_loadu_ps(in + (br + 1) * 8 + bc);
                        __m128 c = _mm_loadu_ps(in + (br + 2) * 8 + bc), d = _mm_loadu_ps(in + (br + 3) * 8 + bc);
                        _MM_TRANSPOSE4_PS(a, b, c, d);
                        _mm_storeu_ps(out + bc * 8 + br, a);
                        _mm_storeu_ps(out + (bc + 1) * 8 + br, b);
                        _mm_storeu_ps(out + (bc + 2) * 8 + br, c);
                        _mm_storeu_ps(out + (bc + 3) * 8 + br, d);
                    }
                }
            }
        }

        template <SimdLevel L>
        void QuantizeBlockV(const float* pixels, std::size_t stride, const float* qmc, float aq, const float* zbOffset, const float* zbMul, std::int16_t* out) noexcept
        {
            using V = Vec<L>;
            alignas(32) float        scratch[64];
            alignas(32) float        dct[64];
            alignas(32) std::int32_t block[64];
            Dct1DColumnsV<L>(pixels, stride, scratch);
            Transpose8x8<L>(scratch, dct);
            Dct1DColumnsV<L>(dct, 8, scratch);
            Transpose8x8<L>(scratch, dct);
            const auto aqV = V::Set(aq);
            for (int k = 0; k < 64; k += V::Width) {
                const auto qval      = V::Mul(V::Load(dct + k), V::Load(qmc + k));
                const auto threshold = V::Add(V::Load(zbOffset + k), V::Mul(V::Load(zbMul + k), aqV));
                V::QuantizeStore(qval, threshold, block + k);
            }
            const float dc = (dct[0] - 128.0f) * qmc[0];
            block[0]       = static_cast<std::int32_t>(std::round(dc));
            for (int k = 0; k < 64; ++k)
                out[k] = static_cast<std::int16_t>(block[NaturalOrder[static_cast<std::size_t>(k)]]);
        }

        // Columns [xBegin, xEnd) (multiples of 4), output columns [xBegin / 4, xEnd / 4); computes
        // whole vectors, up to Width - 4 columns beyond xEnd.
        template <SimdLevel L>
        void PreErosionRowV(Plane& y, std::int32_t p, std::int32_t xBegin, std::int32_t xEnd, float* diff, float* out) noexcept
        {
            using V              = Vec<L>;
            const auto quarter   = V::Set(0.25f);
            const auto gammaOff  = V::Set(MatchGamma);
            const auto limit     = V::Set(0.2f);
            const auto maskSqrt  = V::Sqrt(V::Set(MaskMulV));
            const auto logOffset = V::Set(28.0f);
            for (std::int32_t iy = 0; iy < 4; ++iy) {
                const std::int32_t row = 4 * p + iy;
                const float*       in  = y.Row(row);
                const float*       in1 = y.Row(row + 1);
                const float*       in2 = y.Row(row - 1);
                for (std::int32_t x = xBegin; x < xEnd; x += V::Width) {
                    const auto v    = V::Load(in + x);
                    const auto base = V::Mul(quarter, V::Add(V::Add(V::Load(in + x + 1), V::Load(in + x - 1)), V::Add(V::Load(in2 + x), V::Load(in1 + x))));
                    const auto g    = RatioV<L>(V::Add(v, gammaOff), false);
                    auto       d    = V::Mul(g, V::Sub(v, base));
                    d               = V::Mul(d, d);
                    d               = V::Min(d, limit);
                    d               = V::Mul(quarter, V::Sqrt(V::MulAdd(d, maskSqrt, logOffset)));
                    if (iy != 0)
                        d = V::Add(d, V::Load(diff + x));
                    V::Store(diff + x, d);
                }
            }
            for (std::int32_t x = xBegin / 4; x < xEnd / 4; ++x)
                out[x] = (diff[x * 4] + diff[x * 4 + 1] + diff[x * 4 + 2] + diff[x * 4 + 3]) * 0.25f;
        }

        // Writes whole vectors: up to Width - 1 values beyond xsize (the buffers have room for them).
        template <SimdLevel L>
        void FuzzyErosionRowV(const float* t, const float* m, const float* b, std::int32_t xsize, float* out) noexcept
        {
            using V = Vec<L>;
            for (std::int32_t x = 0; x < xsize; x += V::Width) {
                auto min0 = V::Load(m + x), min1 = V::Load(m + x - 1), min2 = V::Load(m + x + 1), min3 = V::Load(t + x - 1);
                {
                    const auto tmp0 = V::Min(min0, min1), tmp1 = V::Max(min0, min1);
                    const auto tmp2 = V::Min(min2, min3), tmp3 = V::Max(min2, min3);
                    const auto tmp4 = V::Max(tmp0, tmp2), tmp5 = V::Min(tmp1, tmp3);
                    min0 = V::Min(tmp0, tmp2);
                    min1 = V::Min(tmp4, tmp5);
                    min2 = V::Max(tmp4, tmp5);
                    min3 = V::Max(tmp1, tmp3);
                }
                const typename V::T values[5] = {V::Load(t + x), V::Load(t + x + 1), V::Load(b + x - 1), V::Load(b + x), V::Load(b + x + 1)};
                for (const auto v : values) {
                    const auto tmp0 = V::Max(min0, v);
                    const auto tmp1 = V::Max(min1, tmp0);
                    const auto tmp2 = V::Max(min2, tmp1);
                    min0            = V::Min(min0, v);
                    min1            = V::Min(min1, tmp0);
                    min2            = V::Min(min2, tmp1);
                    min3            = V::Min(min3, tmp2);
                }
                V::Store(out + x, V::Add(V::Add(V::Mul(V::Set(0.125f), min0), V::Mul(V::Set(0.075f), min1)), V::Add(V::Mul(V::Set(0.06f), min2), V::Mul(V::Set(0.05f), min3))));
            }
        }

        template <SimdLevel L>
        float BlockModulationsV(Plane& y, std::int32_t x, std::int32_t yy, float outVal) noexcept
        {
            using V                                  = Vec<L>;
            alignas(32) static const std::uint32_t maskRight[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, 0u};
            auto                                     sum          = V::Zero();
            for (int dy = 0; dy < 8; ++dy) {
                const float* row     = y.Row(yy + dy) + x;
                const float* rowNext = dy == 7 ? row : y.Row(yy + dy + 1) + x;
                for (int dx = 0; dx < 8; dx += V::Width) {
                    const auto p    = V::Load(row + dx);
                    const auto mask = V::Load(reinterpret_cast<const float*>(maskRight + dx));
                    sum             = V::Add(sum, V::And(mask, V::Abs(V::Sub(p, V::Load(row + dx + 1)))));
                    sum             = V::Add(sum, V::Abs(V::Sub(p, V::Load(rowNext + dx))));
                }
            }
            alignas(32) float lanes[8];
            V::Store(lanes, sum);
            outVal = Lane<L>::MulAdd(Lane<L>::SumOfLanes(lanes), SumCoeff, outVal);

            auto       ratio = V::Zero();
            const auto bias  = V::Set(GammaBias);
            for (int dy = 0; dy < 8; ++dy) {
                const float* row = y.Row(yy + dy) + x;
                for (int dx = 0; dx < 8; dx += V::Width)
                    ratio = V::Add(ratio, RatioV<L>(V::Add(V::Load(row + dx), bias), true));
            }
            V::Store(lanes, ratio);
            const float overall = Lane<L>::SumOfLanes(lanes) * GammaScale;
            return Lane<L>::MulAdd(GammaK, FastLog2f<L>(overall), outVal);
        }

        // Color transform of one RGB row (ExtRGBToYCbCr); writes whole vectors beyond width.
        template <SimdLevel L>
        void ColorRowV(const std::uint8_t* src, std::int32_t width, float* tmp, std::size_t tmpStride, float* yRow, float* cbRow, float* crRow) noexcept
        {
            using V   = Vec<L>;
            float*       rf = tmp;
            float*       gf = tmp + tmpStride;
            float*       bf = tmp + 2 * tmpStride;
            std::int32_t x0 = 0;
            if constexpr (L == SimdLevel::Avx2) {
                // 8 pixels: bytes 0..15 hold R/G/B of pixels 0..4, bytes 8..23 those of 5..7
                const __m128i rLo = _mm_setr_epi8(0, 3, 6, 9, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
                const __m128i rHi = _mm_setr_epi8(-1, -1, -1, -1, -1, 7, 10, 13, -1, -1, -1, -1, -1, -1, -1, -1);
                const __m128i gLo = _mm_setr_epi8(1, 4, 7, 10, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
                const __m128i gHi = _mm_setr_epi8(-1, -1, -1, -1, -1, 8, 11, 14, -1, -1, -1, -1, -1, -1, -1, -1);
                const __m128i bLo = _mm_setr_epi8(2, 5, 8, 11, 14, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
                const __m128i bHi = _mm_setr_epi8(-1, -1, -1, -1, -1, 9, 12, 15, -1, -1, -1, -1, -1, -1, -1, -1);
                for (; x0 + 8 <= width; x0 += 8) {
                    const __m128i lo = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 3 * x0));
                    const __m128i hi = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 3 * x0 + 8));
                    const __m128i r  = _mm_or_si128(_mm_shuffle_epi8(lo, rLo), _mm_shuffle_epi8(hi, rHi));
                    const __m128i g  = _mm_or_si128(_mm_shuffle_epi8(lo, gLo), _mm_shuffle_epi8(hi, gHi));
                    const __m128i b  = _mm_or_si128(_mm_shuffle_epi8(lo, bLo), _mm_shuffle_epi8(hi, bHi));
                    _mm256_storeu_ps(rf + x0, _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(r)));
                    _mm256_storeu_ps(gf + x0, _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(g)));
                    _mm256_storeu_ps(bf + x0, _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(b)));
                }
            }
            for (std::int32_t x = x0; x < width; ++x) {
                rf[x] = src[3 * x];
                gf[x] = src[3 * x + 1];
                bf[x] = src[3 * x + 2];
            }
            constexpr float R = 0.299f, G = 0.587f, B = 0.114f, AmpR = 0.701f, AmpB = 0.886f;
            const auto      vR = V::Set(R), vG = V::Set(G), vB = V::Set(B);
            const auto      diffR = V::Set(AmpR + R), diffB = V::Set(AmpB + B);
            const auto      normR = V::Set(1.0f / (AmpR + (G + B))), normB = V::Set(1.0f / (R + (G + AmpB)));
            const auto      c128 = V::Set(128.0f);
            for (std::int32_t x = 0; x < width; x += V::Width) {
                const auto r = V::Load(rf + x), g = V::Load(gf + x), b = V::Load(bf + x);
                const auto yBase = V::Add(V::Mul(r, vR), V::Add(V::Mul(g, vG), V::Mul(b, vB)));
                V::Store(yRow + x, yBase);
                if (cbRow != nullptr) {
                    V::Store(cbRow + x, V::MulAdd(V::Sub(V::Mul(b, diffB), yBase), normB, c128));
                    V::Store(crRow + x, V::MulAdd(V::Sub(V::Mul(r, diffR), yBase), normR, c128));
                }
            }
        }

        template <SimdLevel L>
        bool ComputeBand(const EncoderSetup& s, const Image& image, std::int32_t r0, std::int32_t r1, std::int32_t c0, std::int32_t c1, Coefficients& out,
                         PixelScratch& scratch) noexcept
        {
            // Row-wise like jpegli's ProcessiMCURow, with rings instead of whole-band windows so the
            // working set of a worker stays small: per iMCU row the new color rows, the new
            // pre-erosion rows, the field of its block rows and the DCT of its blocks. Only the
            // first iMCU row of a band computes the context above it. A strip of iMCU columns
            // [c0, c1) keeps only its own columns of the color planes, plus 8 context columns on
            // either side (the adaptive quantization of a block reads 5 columns beyond it), so a
            // wide image does not push the worker out of the cache; at the image edges it pads as
            // jpegli does. The small rows (pre-erosion, fuzzy erosion, field) stay full width.
            const std::int32_t width     = image.Width();
            const std::int32_t height    = image.Height();
            const std::int32_t comps     = s.frame.components;
            const std::int32_t iMcuH     = DctSize * s.maxV;
            const std::int32_t mcuW      = DctSize * s.maxH;
            const std::int32_t xpad      = s.xsizeBlocks * DctSize; // padded row length (len1)
            const std::int32_t xLo       = c0 * mcuW;               // the strip's own columns
            const std::int32_t xHi       = c1 * mcuW;
            const std::int32_t wx0       = c0 > 0 ? xLo - 8 : 0;                     // first computed column (the window starts at wx0 - 1)
            const std::int32_t wx1       = xHi + 16 < xpad + 9 ? xHi + 16 : xpad + 9; // end of the computed columns (jpegli pads to xpad + 8)
            const std::int32_t stride    = wx1 - wx0 + 1 + 16;
            const std::int32_t ring      = (iMcuH + 10 + 7) / 8 * 8; // rows [rH - 5, rH + H + 5), blocks never wrap
            const bool         aq        = s.adaptiveQuant;
            const std::int32_t wibY      = s.widthInBlocks[0];
            const std::int32_t yb0       = c0 * s.maxH;                                  // the strip's Y blocks
            const std::int32_t yb1       = c1 * s.maxH < wibY ? c1 * s.maxH : wibY;
            const std::int32_t preRing   = 2 * s.maxV + 2;           // pre rows [2rv - 1, 2(r + 1)v]
            const std::int32_t preStride = 2 * wibY + 2 + 8;
            const std::int32_t c420W     = s.xsizeBlocks * DctSize / 2;
            const bool         sub420    = comps == 3 && s.hFactor[1] == 2;
            const std::int32_t c420Lo    = c0 * DctSize;             // the strip's columns of the 4:2:0 chroma planes
            const std::int32_t c420Hi    = c1 * DctSize < c420W ? c1 * DctSize : c420W;
            const std::int32_t c420Str   = (c1 - c0) * DctSize + 1 + 16;
            const std::int32_t tmpStride = wx1 - wx0 + 16;

            std::size_t floats = static_cast<std::size_t>(ring) * stride * static_cast<std::size_t>(comps);
            if (sub420)
                floats += 2 * static_cast<std::size_t>(iMcuH / 2) * c420Str;
            floats += 3 * static_cast<std::size_t>(tmpStride); // color transform input rows
            floats += static_cast<std::size_t>(preRing) * preStride + 2 * static_cast<std::size_t>(preStride) + static_cast<std::size_t>(xpad + 16) +
                      static_cast<std::size_t>(s.maxV) * wibY + 64;
            float* next = scratch.Reserve(floats);
            if (next == nullptr)
                return false;
            Plane yPlane{next, 0, stride, ring, wx0};
            next += static_cast<std::size_t>(ring) * stride;
            Plane cbPlane{}, crPlane{}, cb420{}, cr420{};
            if (comps == 3) {
                cbPlane = {next, 0, stride, ring, wx0};
                next += static_cast<std::size_t>(ring) * stride;
                crPlane = {next, 0, stride, ring, wx0};
                next += static_cast<std::size_t>(ring) * stride;
                if (sub420) {
                    cb420 = {next, 0, c420Str, iMcuH / 2, c420Lo};
                    next += static_cast<std::size_t>(iMcuH / 2) * c420Str;
                    cr420 = {next, 0, c420Str, iMcuH / 2, c420Lo};
                    next += static_cast<std::size_t>(iMcuH / 2) * c420Str;
                }
            }
            float* pre      = next; // pre-erosion ring, columns -1 .. 2 * wibY
            next           += static_cast<std::size_t>(preRing) * preStride;
            float* fuzzyA   = next; // two fuzzy-erosion rows
            float* fuzzyB   = next + preStride;
            next           += 2 * static_cast<std::size_t>(preStride);
            float* diff     = next;
            next           += static_cast<std::size_t>(xpad + 16);
            float* field    = next; // quant field of the current iMCU row's Y block rows
            next           += static_cast<std::size_t>(s.maxV) * wibY;
            float* colorTmp = next; // r, g, b rows of the color transform

            const std::int32_t preLast = 2 * s.ysizeBlocks - 1;
            auto preSlot = [&](std::int32_t q) noexcept { return pre + static_cast<std::size_t>(Clamp(q, 0, preLast) % preRing) * preStride + 1; };

            float mul = 0.0f, add = 0.0f;
            if (aq) {
                float dampen = 1.0f;
                const float kDampenRampStart = 9.0f, kDampenRampEnd = 65.0f;
                if (s.yQuant01 >= kDampenRampStart) {
                    dampen = 1.0f - ((s.yQuant01 - kDampenRampStart) / (kDampenRampEnd - kDampenRampStart));
                    if (dampen < 0)
                        dampen = 0;
                }
                static const float kAcQuant  = 0.841f;
                const float        baseLevel = 0.48f * kAcQuant;
                mul                          = kAcQuant * dampen;
                add                          = (1.0f - dampen) * baseLevel;
            }

            for (std::int32_t r = r0; r < r1; ++r) {
                // Read, color transform and pad the new rows (ReadInputRow, color_transform,
                // PadInputBuffer): rows and columns outside the image repeat the edge.
                const std::int32_t yFrom = r == r0 ? r * iMcuH - 5 : r * iMcuH + 5;
                const std::int32_t cx1   = width < wx1 ? width : wx1; // the image columns of the window
                for (std::int32_t wy = yFrom; wy < r * iMcuH + iMcuH + 5; ++wy) {
                    const std::uint8_t* src   = image.Row(Clamp(wy, 0, height - 1));
                    float*              yRow  = yPlane.Row(wy);
                    float*              cbRow = comps == 3 ? cbPlane.Row(wy) : nullptr;
                    float*              crRow = comps == 3 ? crPlane.Row(wy) : nullptr;
                    if (comps == 1) {
                        for (std::int32_t x = wx0; x < cx1; ++x)
                            yRow[x] = src[x];
                    } else
                        ColorRowV<L>(src + 3 * static_cast<std::size_t>(wx0), cx1 - wx0, colorTmp, static_cast<std::size_t>(tmpStride), yRow + wx0, cbRow + wx0,
                                     crRow + wx0);
                    for (float* row : {yRow, cbRow, crRow}) {
                        if (row == nullptr)
                            continue;
                        if (wx1 > width) { // the strip at the right edge
                            const float last = row[width - 1];
                            for (std::int32_t x = width; x < wx1; ++x)
                                row[x] = last;
                        }
                        if (c0 == 0)
                            row[-1] = row[0];
                    }
                }

                // Downsampling 2x2 (Downsample2x2) of this iMCU row's chroma for 4:2:0.
                if (sub420) {
                    for (std::int32_t yo = r * iMcuH / 2; yo < (r + 1) * iMcuH / 2; ++yo) {
                        for (int c = 0; c < 2; ++c) {
                            Plane&       in   = c == 0 ? cbPlane : crPlane;
                            const float* row0 = in.Row(2 * yo);
                            const float* row1 = in.Row(2 * yo + 1);
                            float*       o    = (c == 0 ? cb420 : cr420).Row(yo);
                            for (std::int32_t x = c420Lo; x < c420Hi; ++x)
                                o[x] = 0.25f * ((row0[2 * x] + row0[2 * x + 1]) + (row1[2 * x] + row1[2 * x + 1]));
                        }
                    }
                }

                // Adaptive quantization field (ComputeAdaptiveQuantField) of this iMCU row.
                const std::int32_t ybBegin = r * s.maxV, ybEnd = (r + 1) * s.maxV;
                if (aq) {
                    // the strip's pre-erosion columns: its own and one on either side (inside the image)
                    const std::int32_t xsizeOut = 2 * wibY;
                    const std::int32_t pa       = 2 * yb0 - 1 > 0 ? 2 * yb0 - 1 : 0;
                    const std::int32_t pb       = 2 * yb1 + 1 < xsizeOut ? 2 * yb1 + 1 : xsizeOut;
                    const std::int32_t pFrom    = r == r0 ? 2 * ybBegin - 1 : 2 * ybBegin + 1;
                    for (std::int32_t p = pFrom; p <= 2 * ybEnd; ++p) {
                        const std::int32_t q   = Clamp(p, 0, preLast);
                        float*             row = preSlot(q);
                        PreErosionRowV<L>(yPlane, q, 4 * pa, 4 * pb, diff, row);
                        if (pa == 0)
                            row[-1] = row[0]; // PadRow, border 1
                        if (pb == xsizeOut)
                            for (std::int32_t x = xsizeOut; x < xsizeOut + 8; ++x)
                                row[x] = row[xsizeOut - 1];
                    }
                    const std::int32_t fx0 = 2 * yb0, fn = 2 * (yb1 - yb0);
                    for (std::int32_t yb = ybBegin; yb < ybEnd; ++yb) {
                        FuzzyErosionRowV<L>(preSlot(2 * yb - 1) + fx0, preSlot(2 * yb) + fx0, preSlot(2 * yb + 1) + fx0, fn, fuzzyA + 1 + fx0);
                        FuzzyErosionRowV<L>(preSlot(2 * yb) + fx0, preSlot(2 * yb + 1) + fx0, preSlot(2 * yb + 2) + fx0, fn, fuzzyB + 1 + fx0);
                        float*       fieldRow = field + static_cast<std::size_t>(yb - ybBegin) * wibY;
                        const float* even     = fuzzyA + 1;
                        const float* odd      = fuzzyB + 1;
                        for (std::int32_t bx = yb0, x = 2 * yb0; bx < yb1; ++bx, x += 2) {
                            const float eroded = odd[x] + odd[x + 1] + even[x] + even[x + 1];
                            float       outVal = ComputeMask<L>(eroded);
                            outVal             = BlockModulationsV<L>(yPlane, bx * DctSize, yb * DctSize, outVal);
                            const float v      = FastPow2f<L>(outVal * 1.442695041f) * mul + add;
                            const float f      = (0.6f / v) - 1.0f;
                            fieldRow[bx]       = 0.0f < f ? f : 0.0f;
                        }
                    }
                }

                // DCT and quantization of this iMCU row's blocks (ProcessiMCURow).
                for (std::int32_t c = 0; c < comps; ++c) {
                    const auto          ci    = static_cast<std::size_t>(c);
                    const ComponentInfo comp  = s.frame.component[ci];
                    Plane&              plane = c == 0 ? yPlane : (sub420 ? (c == 1 ? cb420 : cr420) : (c == 1 ? cbPlane : crPlane));
                    for (std::int32_t by = r * comp.vSamp; by < (r + 1) * comp.vSamp && by < s.heightInBlocks[ci]; ++by) {
                        const std::int32_t iy       = by % comp.vSamp;
                        const std::int32_t ybAq     = r * s.maxV + iy; // Y block row of the field
                        const float*       rowStart = plane.Row(by * DctSize);
                        const std::int32_t bxEnd    = c1 * comp.hSamp < s.widthInBlocks[ci] ? c1 * comp.hSamp : s.widthInBlocks[ci];
                        for (std::int32_t bx = c0 * comp.hSamp; bx < bxEnd; ++bx) {
                            const float aqStrength = aq ? field[static_cast<std::size_t>(ybAq - ybBegin) * wibY + static_cast<std::size_t>(bx * s.hFactor[ci])] : 0.0f;
                            std::int16_t* block = out.Block(c, bx, by);
                            QuantizeBlockV<L>(rowStart + bx * DctSize, static_cast<std::size_t>(plane.stride), s.quantMul[ci].data(), aqStrength,
                                              s.zeroBiasOffset[ci].data(), s.zeroBiasMul[ci].data(), block);
                            out.Dc(c, bx, by) = block[0];
                        }
                    }
                }
            }
            return true;
        }

        // ------------------------------------------------------------------ kernel self-test
        // Every SIMD kernel against its scalar reference on random data, bit for bit.

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
            for (int k = 0; k < 64; ++k) {
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
            // color transform
            std::uint8_t rgb[3 * W];
            for (std::uint8_t& v : rgb)
                v = static_cast<std::uint8_t>(NextRandom(state));
            static float tmp[3 * (W + 16)], y1[W + 16], cb1[W + 16], cr1[W + 16], y2[W + 16], cb2[W + 16], cr2[W + 16];
            ColorRowV<L>(rgb, 37, tmp, W + 16, y2, cb2, cr2);
            constexpr float R = 0.299f, G = 0.587f, B = 0.114f, AmpR = 0.701f, AmpB = 0.886f;
            const float     DiffR = AmpR + R, DiffB = AmpB + B, NormR = 1.0f / (AmpR + (G + B)), NormB = 1.0f / (R + (G + AmpB));
            for (std::int32_t x = 0; x < 37; ++x) {
                const float r = rgb[3 * x], g = rgb[3 * x + 1], bb = rgb[3 * x + 2];
                const float yBase = r * R + (g * G + bb * B);
                y1[x]             = yBase;
                cb1[x]            = Lane<L>::MulAdd(bb * DiffB - yBase, NormB, 128.0f);
                cr1[x]            = Lane<L>::MulAdd(r * DiffR - yBase, NormR, 128.0f);
            }
            return SameBits(y1, y2, 37) && SameBits(cb1, cb2, 37) && SameBits(cr1, cr2, 37);
        }

    } // namespace

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
