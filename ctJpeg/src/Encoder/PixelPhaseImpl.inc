// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

// Included by PixelPhaseSse2.cpp and PixelPhaseAvx2.cpp (compiled with /arch:AVX2), one
// instantiation of the pixel phase per feature set. The scalar reference kernels and the kernel
// self-test follow in PixelPhaseReference.inc, included at the end.

#include "PixelPhase.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <Windows.h>

#include "LaneMath.h"
#include "VecMath.h"

// Port of jpegli's pixel phase (encode.cc ReadInputRow/PadInputBuffer/ProcessiMCURow,
// color_transform.cc, downsample.cc, adaptive_quantization.cc, dct-inl.h, encode_streaming.cc):
// every value goes through the same float operations in the same order as jpegli on the Highway
// target of the feature set (LaneMath.h: MulAdd fused or not, the lane order of SumOfLanes, the
// per-lane accumulation of the block sums).
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

        constexpr float Sqrt2        = 1.41421356237f; // kSqrt2 of dct-inl.h B<N>()
        constexpr float DcBias       = 128.0f;         // kDCBias of ComputeCoefficientBlock: centers the DC around zero
        constexpr float InputScaling = 1.0f / 255.0f;  // kInputScaling

        // ExtRGBToYCbCr of color_transform.cc: NTSC luma weights (kR, kG, kB), chroma amplitudes
        // (kAmpR, kAmpB) and the chroma offset.
        constexpr float LumaR        = 0.299f;
        constexpr float LumaG        = 0.587f;
        constexpr float LumaB        = 0.114f;
        constexpr float ChromaAmpR   = 0.701f;
        constexpr float ChromaAmpB   = 0.886f;
        constexpr float ChromaOffset = 128.0f;

        // Constants of adaptive_quantization.cc, same expressions and types.
        const float SGmul      = 226.0480446705883f;
        const float SGmul2     = 1.0f / 73.377132366608819f;
        const float SGRetMul   = SGmul2 * 18.6580932135f * InvLog2e;
        const float SGVOffset  = 7.14672470003f;
        const float Epsilon    = 1e-2;
        const float NumOffset  = Epsilon / InputScaling / InputScaling;
        const float NumMul     = SGRetMul * 3 * SGmul;
        const float VOffset    = (SGVOffset * InvLog2e + Epsilon) / InputScaling;
        const float DenMul     = InvLog2e * SGmul * InputScaling * InputScaling;
        const float MatchGamma = 0.019 / InputScaling; // match_gamma_offset
        const float MaskMul    = 211.50759899638012f;  // MaskingSqrt kMul
        const float MaskMulV   = MaskMul * 1e8;         // Set(d, kMul * 1e8)
        const float SumCoeff   = -2.0052193233688884f * InputScaling / 112.0;
        const float GammaBias  = 0.16f / InputScaling;
        const float GammaScale = InputScaling / 64.0f;
        const float GammaK     = -0.15526878023684174f * InvLog2e;

        // PerBlockModulations: the field strength (kAcQuant) and its dampening for coarse Y
        // quantization (kDampenRampStart, kDampenRampEnd, compared with quantval[1] of the Y table).
        constexpr float AcQuant          = 0.841f;
        constexpr float DampenRampStart  = 9.0f;
        constexpr float DampenRampEnd    = 65.0f;
        // PerBlockModulations: jpegli's literal for log2(e), so exp(x) is computed as 2^(x * Log2e).
        constexpr float Log2e            = 1.442695041f;
        // ComputeAdaptiveQuantField: the field of a block is max(0, QuantFieldScale / v - 1).
        constexpr float QuantFieldScale  = 0.6f;

        std::int32_t Clamp(std::int32_t v, std::int32_t lo, std::int32_t hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

        // Highway Min/Max on x86 (minps/maxps): the first operand if it compares less/greater.
        float MinPs(float a, float b) noexcept { return a < b ? a : b; }
        float MaxPs(float a, float b) noexcept { return a > b ? a : b; }

        // jpegli: FastLog2f() (adaptive_quantization.cc) with the 2,2 rational polynomial (EvalRationalPolynomial, Div).
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

        // jpegli: FastPow2f() (adaptive_quantization.cc).
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

        // jpegli: ComputeMask() (adaptive_quantization.cc).
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

        // ------------------------------------------------------------------ SIMD kernels
        // Width lanes at a time with the same operations per lane as the scalar reference kernels
        // of PixelPhaseReference.inc; the block sums of BlockModulationsV keep jpegli's lane layout.

        // jpegli: RatioOfDerivativesOfCubicRootToSimpleGamma() (adaptive_quantization.cc).
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

        // jpegli: DCT1DImpl<N> (dct-inl.h) on N rows of Width columns.
        template <SimdLevel L, std::int32_t N>
        struct DctV {
            using V = Vec<L>;
            using T = typename V::T;
            static __forceinline void Run(T* mem) noexcept
            {
                T tmp[N];
                for (std::int32_t i = 0; i < N / 2; ++i)
                    tmp[i] = V::Add(mem[i], mem[N - 1 - i]);
                DctV<L, N / 2>::Run(tmp);
                for (std::int32_t i = 0; i < N / 2; ++i)
                    tmp[N / 2 + i] = V::Sub(mem[i], mem[N - 1 - i]);
                const float* mul = N == 8 ? WcMultipliers8 : WcMultipliers4;
                for (std::int32_t i = 0; i < N / 2; ++i)
                    tmp[N / 2 + i] = V::Mul(tmp[N / 2 + i], V::Set(mul[i]));
                DctV<L, N / 2>::Run(tmp + N / 2);
                T* b = tmp + N / 2;
                b[0] = V::MulAdd(b[0], V::Set(Sqrt2), b[1]);
                for (std::int32_t i = 1; i + 1 < N / 2; ++i)
                    b[i] = V::Add(b[i], b[i + 1]);
                for (std::int32_t i = 0; i < N / 2; ++i)
                    mem[2 * i] = tmp[i];
                for (std::int32_t i = N / 2; i < N; ++i)
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

        // jpegli: DCT1D() (dct-inl.h): the 1-D DCT of the 8 columns of a block, scaled by 1/8.
        template <SimdLevel L>
        void Dct1DColumnsV(const float* in, std::size_t stride, float* out) noexcept
        {
            using V = Vec<L>;
            for (std::int32_t c0 = 0; c0 < 8; c0 += V::Width) {
                typename V::T rows[8];
                for (std::int32_t r = 0; r < 8; ++r)
                    rows[r] = V::Load(in + static_cast<std::size_t>(r) * stride + static_cast<std::size_t>(c0));
                DctV<L, 8>::Run(rows);
                for (std::int32_t r = 0; r < 8; ++r)
                    V::Store(out + r * 8 + c0, V::Mul(V::Set(1.0f / 8), rows[r]));
            }
        }

        // jpegli: Transpose8x8Block() (transpose-inl.h): pure data movement, the same for every feature set.
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
                for (std::int32_t br = 0; br < 8; br += 4) {
                    for (std::int32_t bc = 0; bc < 8; bc += 4) {
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

        // jpegli: ComputeCoefficientBlock(), TransformFromPixels() and QuantizeBlock() (dct-inl.h)
        // in one: DCT of the columns, transpose, DCT of the columns, transpose, quantization with
        // the zero-bias threshold; the result goes out in zigzag order (the copy of
        // ProcessiMCURow). The DC dead zone of jpegli compares with the previous DC under a
        // threshold that is always 0 here (zero_bias_offset[0] = zero_bias_mul[0] = 0), so the DC
        // is always round(dc).
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
            for (std::int32_t k = 0; k < 64; k += V::Width) {
                const auto qval      = V::Mul(V::Load(dct + k), V::Load(qmc + k));
                const auto threshold = V::Add(V::Load(zbOffset + k), V::Mul(V::Load(zbMul + k), aqV));
                V::QuantizeStore(qval, threshold, block + k);
            }
            const float dc = (dct[0] - DcBias) * qmc[0];
            block[0]       = static_cast<std::int32_t>(std::round(dc));
            for (std::int32_t k = 0; k < 64; ++k)
                out[k] = static_cast<std::int16_t>(block[NaturalOrder[static_cast<std::size_t>(k)]]);
        }

        // jpegli: ComputePreErosion() (adaptive_quantization.cc) for one pre-erosion row p (input
        // rows 4p..4p+3 of the Y window). Columns [xBegin, xEnd) (multiples of 4), output columns
        // [xBegin / 4, xEnd / 4); computes whole vectors, up to Width - 4 columns beyond xEnd.
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

        // jpegli: Sort4(), UpdateMin4() and FuzzyErosion() (adaptive_quantization.cc) for one
        // output row of the 2x-downsampled grid. Writes whole vectors: up to Width - 1 values
        // beyond xsize (the buffers have room for them).
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

        // jpegli: HfModulation() and GammaModulation() (adaptive_quantization.cc) of one 8x8 block
        // (x, y in pixels of the Y window).
        template <SimdLevel L>
        float BlockModulationsV(Plane& y, std::int32_t x, std::int32_t yy, float outVal) noexcept
        {
            using V                                  = Vec<L>;
            alignas(32) static const std::uint32_t maskRight[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, 0u};
            auto                                     sum          = V::Zero();
            for (std::int32_t dy = 0; dy < 8; ++dy) {
                const float* row     = y.Row(yy + dy) + x;
                const float* rowNext = dy == 7 ? row : y.Row(yy + dy + 1) + x;
                for (std::int32_t dx = 0; dx < 8; dx += V::Width) {
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
            for (std::int32_t dy = 0; dy < 8; ++dy) {
                const float* row = y.Row(yy + dy) + x;
                for (std::int32_t dx = 0; dx < 8; dx += V::Width)
                    ratio = V::Add(ratio, RatioV<L>(V::Add(V::Load(row + dx), bias), true));
            }
            V::Store(lanes, ratio);
            const float overall = Lane<L>::SumOfLanes(lanes) * GammaScale;
            return Lane<L>::MulAdd(GammaK, FastLog2f<L>(overall), outVal);
        }

        // jpegli: ReadUint8RowInterleaved3() (input.cc) and ExtRGBToYCbCr() (color_transform.cc)
        // of one RGB row; writes whole vectors beyond width.
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
            const auto vR = V::Set(LumaR), vG = V::Set(LumaG), vB = V::Set(LumaB);
            const auto diffR = V::Set(ChromaAmpR + LumaR), diffB = V::Set(ChromaAmpB + LumaB);
            const auto normR = V::Set(1.0f / (ChromaAmpR + (LumaG + LumaB))), normB = V::Set(1.0f / (LumaR + (LumaG + ChromaAmpB)));
            const auto offset = V::Set(ChromaOffset);
            for (std::int32_t x = 0; x < width; x += V::Width) {
                const auto r = V::Load(rf + x), g = V::Load(gf + x), b = V::Load(bf + x);
                const auto yBase = V::Add(V::Mul(r, vR), V::Add(V::Mul(g, vG), V::Mul(b, vB)));
                V::Store(yRow + x, yBase);
                if (cbRow != nullptr) {
                    V::Store(cbRow + x, V::MulAdd(V::Sub(V::Mul(b, diffB), yBase), normB, offset));
                    V::Store(crRow + x, V::MulAdd(V::Sub(V::Mul(r, diffR), yBase), normR, offset));
                }
            }
        }

        // ------------------------------------------------------------------ tiles
        // New in ctJpeg, not in jpegli: jpegli runs the pixel phase over whole rows of the image.
        // A tile (iMCU rows [r0, r1) x iMCU columns [c0, c1)) reads its own context rows and
        // columns, so the tiles of an image can run in parallel and give jpegli's values.

        // Context rows above and below an iMCU row: pre-erosion row 2 yb - 1 of its first Y block
        // row yb reads the input rows from 8 yb - 5 on, the last one up to 4 rows below the iMCU row.
        constexpr std::int32_t ContextRows = 5;
        // Context columns left of a strip: the adaptive quantization of a block reads up to 5
        // columns beyond it; rounded up to a block.
        constexpr std::int32_t LeftContextColumns = 8;
        // Columns computed right of a strip: its context columns and the whole-vector overreach of
        // the pre-erosion beyond them.
        constexpr std::int32_t RightContextColumns = 16;
        // The computed columns end at most this far past the padded row length xpad: jpegli's
        // PadInputBuffer repeats the last pixel through column xpad (one border column), and the
        // AQ windows work in whole groups of 8 columns beyond it; the window buffers have room.
        constexpr std::int32_t PaddedRowOverhang = 9;
        // Floats of slack after a row of a window or buffer, for whole-vector loads and stores past
        // its last column.
        constexpr std::int32_t VectorSlack = 16;
        // Columns a pre-erosion row is padded with to the right of the image (PadRow), for the
        // whole-vector loads of the fuzzy erosion.
        constexpr std::int32_t PreErosionPadColumns = 8;
        // Floats of slack at the end of a band's scratch block.
        constexpr std::int32_t ScratchSlack = 64;

        // The columns and buffer sizes of a tile. Columns are absolute image columns: the strip's
        // own columns, plus the context columns that are computed with them.
        struct TileWindow {
            std::int32_t width          = 0;     // image width
            std::int32_t height         = 0;     // image height
            std::int32_t comps          = 0;     // 1 or 3
            std::int32_t iMcuHeight     = 0;     // pixel rows of an iMCU row
            std::int32_t paddedWidth    = 0;     // xpad: padded row length (jpegli len1 = xsize_blocks * DCTSIZE)
            bool         leftEdge       = false; // the strip starts at column 0 and pads column -1
            std::int32_t computedBegin  = 0;     // first computed column (the window starts one column before it)
            std::int32_t computedEnd    = 0;     // end of the computed columns
            std::int32_t imageEnd       = 0;     // end of the image columns among them
            std::int32_t stride         = 0;     // floats per row of the color-plane windows
            std::int32_t ring           = 0;     // rows of the color-plane rings: [rH - 5, rH + H + 5), blocks never wrap
            std::int32_t yBlocks        = 0;     // Y blocks of a full row (width_in_blocks[0])
            std::int32_t yBlockBegin    = 0;     // the strip's Y blocks
            std::int32_t yBlockEnd      = 0;
            std::int32_t preCols        = 0;     // pre-erosion columns of a full row (2 per Y block)
            std::int32_t preBegin       = 0;     // the strip's pre-erosion columns: its own and one on either side (inside the image)
            std::int32_t preEnd         = 0;
            std::int32_t preRing        = 0;     // pre-erosion rows [2rv - 1, 2(r + 1)v]
            std::int32_t preStride      = 0;     // columns -1 .. preCols + PreErosionPadColumns
            std::int32_t preLast        = 0;     // last pre-erosion row of the image
            bool         sub420         = false; // 4:2:0 chroma
            std::int32_t chromaBegin    = 0;     // the strip's columns of the 4:2:0 chroma planes
            std::int32_t chromaEnd      = 0;
            std::int32_t chromaStride   = 0;
            std::int32_t colorTmpStride = 0;     // floats per r, g, b row of the color transform
        };

        __forceinline TileWindow MakeTileWindow(const EncoderSetup& s, const Image& image, std::int32_t c0, std::int32_t c1) noexcept
        {
            TileWindow w;
            w.width                  = image.Width();
            w.height                 = image.Height();
            w.comps                  = s.frame.components;
            w.iMcuHeight             = DctSize * s.maxV;
            const std::int32_t mcuW  = DctSize * s.maxH;
            w.paddedWidth            = s.xsizeBlocks * DctSize;
            const std::int32_t xLo   = c0 * mcuW; // the strip's own columns
            const std::int32_t xHi   = c1 * mcuW;
            const std::int32_t right = xHi + RightContextColumns;
            const std::int32_t limit = w.paddedWidth + PaddedRowOverhang;
            w.leftEdge               = c0 == 0;
            w.computedBegin          = c0 > 0 ? xLo - LeftContextColumns : 0;
            w.computedEnd            = right < limit ? right : limit;
            w.imageEnd               = w.width < w.computedEnd ? w.width : w.computedEnd;
            w.stride                 = w.computedEnd - w.computedBegin + 1 + VectorSlack; // + column computedBegin - 1
            w.ring                   = (w.iMcuHeight + 2 * ContextRows + DctSize - 1) / DctSize * DctSize;
            w.yBlocks                = s.widthInBlocks[0];
            w.yBlockBegin            = c0 * s.maxH;
            w.yBlockEnd              = c1 * s.maxH < w.yBlocks ? c1 * s.maxH : w.yBlocks;
            w.preCols                = 2 * w.yBlocks;
            w.preBegin               = 2 * w.yBlockBegin - 1 > 0 ? 2 * w.yBlockBegin - 1 : 0;
            w.preEnd                 = 2 * w.yBlockEnd + 1 < w.preCols ? 2 * w.yBlockEnd + 1 : w.preCols;
            w.preRing                = 2 * s.maxV + 2;
            w.preStride              = w.preCols + 2 + PreErosionPadColumns;
            w.preLast                = 2 * s.ysizeBlocks - 1;
            const std::int32_t c420W = s.xsizeBlocks * DctSize / 2;
            w.sub420                 = w.comps == 3 && s.hFactor[1] == 2;
            w.chromaBegin            = c0 * DctSize;
            w.chromaEnd              = c1 * DctSize < c420W ? c1 * DctSize : c420W;
            w.chromaStride           = (c1 - c0) * DctSize + 1 + VectorSlack;
            w.colorTmpStride         = w.computedEnd - w.computedBegin + VectorSlack;
            return w;
        }

        // The working buffers of a band, carved from its scratch block: per iMCU row the new color
        // rows, the new pre-erosion rows, the field of its block rows and the DCT of its blocks.
        struct BandBuffers {
            Plane        y, cb, cr;          // color-converted rings
            Plane        cb420, cr420;       // the iMCU row's 4:2:0 chroma
            float*       pre       = nullptr; // pre-erosion ring (pre_erosion)
            float*       fuzzyA    = nullptr; // fuzzy-erosion rows of a Y block row (fuzzy_erosion_tmp)
            float*       fuzzyB    = nullptr;
            float*       diff      = nullptr; // diff_buffer of the pre-erosion
            float*       field     = nullptr; // quant field of the current iMCU row's Y block rows
            float*       colorTmp  = nullptr; // r, g, b rows of the color transform
            std::int32_t preRing   = 0;
            std::int32_t preStride = 0;
            std::int32_t preLast   = 0;

            // Pre-erosion row q, clamped to the image (jpegli's CopyRow of the first and last row).
            float* PreRow(std::int32_t q) noexcept { return pre + static_cast<std::size_t>(Clamp(q, 0, preLast) % preRing) * preStride + 1; }
        };

        __forceinline bool AllocateBandBuffers(const EncoderSetup& s, const TileWindow& w, PixelScratch& scratch, BandBuffers& b) noexcept
        {
            std::size_t floats = static_cast<std::size_t>(w.ring) * w.stride * static_cast<std::size_t>(w.comps);
            if (w.sub420)
                floats += 2 * static_cast<std::size_t>(w.iMcuHeight / 2) * w.chromaStride;
            floats += 3 * static_cast<std::size_t>(w.colorTmpStride); // color transform input rows
            floats += static_cast<std::size_t>(w.preRing) * w.preStride + 2 * static_cast<std::size_t>(w.preStride) +
                      static_cast<std::size_t>(w.paddedWidth + VectorSlack) + static_cast<std::size_t>(s.maxV) * w.yBlocks + ScratchSlack;
            float* next = scratch.Reserve(floats);
            if (next == nullptr)
                return false;
            b.y   = {next, 0, w.stride, w.ring, w.computedBegin};
            next += static_cast<std::size_t>(w.ring) * w.stride;
            if (w.comps == 3) {
                b.cb  = {next, 0, w.stride, w.ring, w.computedBegin};
                next += static_cast<std::size_t>(w.ring) * w.stride;
                b.cr  = {next, 0, w.stride, w.ring, w.computedBegin};
                next += static_cast<std::size_t>(w.ring) * w.stride;
                if (w.sub420) {
                    b.cb420  = {next, 0, w.chromaStride, w.iMcuHeight / 2, w.chromaBegin};
                    next    += static_cast<std::size_t>(w.iMcuHeight / 2) * w.chromaStride;
                    b.cr420  = {next, 0, w.chromaStride, w.iMcuHeight / 2, w.chromaBegin};
                    next    += static_cast<std::size_t>(w.iMcuHeight / 2) * w.chromaStride;
                }
            }
            b.pre        = next;
            next        += static_cast<std::size_t>(w.preRing) * w.preStride;
            b.fuzzyA     = next;
            b.fuzzyB     = next + w.preStride;
            next        += 2 * static_cast<std::size_t>(w.preStride);
            b.diff       = next;
            next        += static_cast<std::size_t>(w.paddedWidth + VectorSlack);
            b.field      = next;
            next        += static_cast<std::size_t>(s.maxV) * w.yBlocks;
            b.colorTmp   = next;
            b.preRing    = w.preRing;
            b.preStride  = w.preStride;
            b.preLast    = w.preLast;
            return true;
        }

        // ------------------------------------------------------------------ steps of an iMCU row

        // jpegli: ReadInputRow() (encode.cc), ExtRGBToYCbCr() (color_transform.cc) and
        // PadInputBuffer() (encode.cc) for the new rows of iMCU row r: rows and columns outside the
        // image repeat the edge. The first iMCU row of a band also reads the context above it.
        template <SimdLevel L>
        __forceinline void ReadInputRows(const Image& image, const TileWindow& w, std::int32_t r, bool firstRow, BandBuffers& b) noexcept
        {
            const std::int32_t yFrom = firstRow ? r * w.iMcuHeight - ContextRows : r * w.iMcuHeight + ContextRows;
            for (std::int32_t wy = yFrom; wy < r * w.iMcuHeight + w.iMcuHeight + ContextRows; ++wy) {
                const std::uint8_t* src   = image.Row(Clamp(wy, 0, w.height - 1));
                float*              yRow  = b.y.Row(wy);
                float*              cbRow = w.comps == 3 ? b.cb.Row(wy) : nullptr;
                float*              crRow = w.comps == 3 ? b.cr.Row(wy) : nullptr;
                if (w.comps == 1) {
                    for (std::int32_t x = w.computedBegin; x < w.imageEnd; ++x)
                        yRow[x] = src[x];
                } else
                    ColorRowV<L>(src + 3 * static_cast<std::size_t>(w.computedBegin), w.imageEnd - w.computedBegin, b.colorTmp,
                                 static_cast<std::size_t>(w.colorTmpStride), yRow + w.computedBegin, cbRow + w.computedBegin, crRow + w.computedBegin);
                for (float* row : {yRow, cbRow, crRow}) {
                    if (row == nullptr)
                        continue;
                    if (w.computedEnd > w.width) { // the strip at the right edge
                        const float last = row[w.width - 1];
                        for (std::int32_t x = w.width; x < w.computedEnd; ++x)
                            row[x] = last;
                    }
                    if (w.leftEdge)
                        row[-1] = row[0];
                }
            }
        }

        // jpegli: Downsample2x2() (downsample.cc) of iMCU row r's chroma for 4:2:0.
        __forceinline void Downsample2x2Rows(const TileWindow& w, std::int32_t r, BandBuffers& b) noexcept
        {
            for (std::int32_t yo = r * w.iMcuHeight / 2; yo < (r + 1) * w.iMcuHeight / 2; ++yo) {
                for (std::int32_t c = 0; c < 2; ++c) {
                    Plane&       in   = c == 0 ? b.cb : b.cr;
                    const float* row0 = in.Row(2 * yo);
                    const float* row1 = in.Row(2 * yo + 1);
                    float*       o    = (c == 0 ? b.cb420 : b.cr420).Row(yo);
                    for (std::int32_t x = w.chromaBegin; x < w.chromaEnd; ++x)
                        o[x] = 0.25f * ((row0[2 * x] + row0[2 * x + 1]) + (row1[2 * x] + row1[2 * x + 1]));
                }
            }
        }

        // The scale of the quant field (PerBlockModulations: mul, add).
        struct QuantFieldScaling {
            float mul = 0.0f;
            float add = 0.0f;
        };

        // jpegli: the dampening at the start of PerBlockModulations() (adaptive_quantization.cc).
        __forceinline QuantFieldScaling ComputeQuantFieldScaling(const EncoderSetup& s) noexcept
        {
            QuantFieldScaling scaling;
            float             dampen = 1.0f;
            if (s.yQuant01 >= DampenRampStart) {
                dampen = 1.0f - ((s.yQuant01 - DampenRampStart) / (DampenRampEnd - DampenRampStart));
                if (dampen < 0)
                    dampen = 0;
            }
            const float baseLevel = 0.48f * AcQuant;
            scaling.mul           = AcQuant * dampen;
            scaling.add           = (1.0f - dampen) * baseLevel;
            return scaling;
        }

        // jpegli: ComputePreErosion() (adaptive_quantization.cc) and RowBuffer::PadRow() (common_internal.h) of the new
        // pre-erosion rows of the Y block rows [ybBegin, ybEnd), over the strip's pre-erosion
        // columns. The first iMCU row of a band also computes the row above it.
        template <SimdLevel L>
        __forceinline void ComputePreErosionRows(const TileWindow& w, std::int32_t ybBegin, std::int32_t ybEnd, bool firstRow, BandBuffers& b) noexcept
        {
            float* const       diff  = b.diff;
            const std::int32_t pFrom = firstRow ? 2 * ybBegin - 1 : 2 * ybBegin + 1;
            for (std::int32_t p = pFrom; p <= 2 * ybEnd; ++p) {
                const std::int32_t q   = Clamp(p, 0, w.preLast);
                float*             row = b.PreRow(q);
                PreErosionRowV<L>(b.y, q, 4 * w.preBegin, 4 * w.preEnd, diff, row);
                if (w.preBegin == 0)
                    row[-1] = row[0]; // PadRow, border 1
                if (w.preEnd == w.preCols)
                    for (std::int32_t x = w.preCols; x < w.preCols + PreErosionPadColumns; ++x)
                        row[x] = row[w.preCols - 1];
            }
        }

        // jpegli: FuzzyErosion() (adaptive_quantization.cc) of Y block row yb over the strip's
        // columns: the eroded pre-erosion rows 2 yb (fuzzyA) and 2 yb + 1 (fuzzyB).
        template <SimdLevel L>
        __forceinline void FuzzyErosionBlockRow(const TileWindow& w, std::int32_t yb, BandBuffers& b) noexcept
        {
            const std::int32_t fx0 = 2 * w.yBlockBegin, fn = 2 * (w.yBlockEnd - w.yBlockBegin);
            FuzzyErosionRowV<L>(b.PreRow(2 * yb - 1) + fx0, b.PreRow(2 * yb) + fx0, b.PreRow(2 * yb + 1) + fx0, fn, b.fuzzyA + 1 + fx0);
            FuzzyErosionRowV<L>(b.PreRow(2 * yb) + fx0, b.PreRow(2 * yb + 1) + fx0, b.PreRow(2 * yb + 2) + fx0, fn, b.fuzzyB + 1 + fx0);
        }

        // jpegli: PerBlockModulations() (adaptive_quantization.cc) and the final mapping of
        // ComputeAdaptiveQuantField() for the strip's blocks of Y block row yb.
        template <SimdLevel L>
        __forceinline void PerBlockModulationsRow(const TileWindow& w, std::int32_t yb, std::int32_t ybBegin, const QuantFieldScaling& scaling, BandBuffers& b) noexcept
        {
            float*       fieldRow = b.field + static_cast<std::size_t>(yb - ybBegin) * w.yBlocks;
            const float* even     = b.fuzzyA + 1;
            const float* odd      = b.fuzzyB + 1;
            const float  mul      = scaling.mul;
            const float  add      = scaling.add;
            for (std::int32_t bx = w.yBlockBegin, x = 2 * w.yBlockBegin; bx < w.yBlockEnd; ++bx, x += 2) {
                const float eroded = odd[x] + odd[x + 1] + even[x] + even[x + 1];
                float       outVal = ComputeMask<L>(eroded);
                outVal             = BlockModulationsV<L>(b.y, bx * DctSize, yb * DctSize, outVal);
                const float v      = FastPow2f<L>(outVal * Log2e) * mul + add;
                const float f      = (QuantFieldScale / v) - 1.0f;
                fieldRow[bx]       = 0.0f < f ? f : 0.0f;
            }
        }

        // jpegli: ComputeAdaptiveQuantField() (adaptive_quantization.cc) of the Y block rows
        // [ybBegin, ybEnd) of an iMCU row.
        template <SimdLevel L>
        __forceinline void ComputeAdaptiveQuantField(const TileWindow& w, std::int32_t ybBegin, std::int32_t ybEnd, bool firstRow, const QuantFieldScaling& scaling,
                                                     BandBuffers& b) noexcept
        {
            ComputePreErosionRows<L>(w, ybBegin, ybEnd, firstRow, b);
            for (std::int32_t yb = ybBegin; yb < ybEnd; ++yb) {
                FuzzyErosionBlockRow<L>(w, yb, b);
                PerBlockModulationsRow<L>(w, yb, ybBegin, scaling, b);
            }
        }

        // jpegli: ProcessiMCURow<kStreamingModeCoefficients>() (encode_streaming.cc): DCT and
        // quantization of the strip's blocks of iMCU row r, with the Y field of its block rows.
        template <SimdLevel L>
        __forceinline void ComputeCoefficientsForiMcuRow(const EncoderSetup& s, const TileWindow& w, std::int32_t r, std::int32_t c0, std::int32_t c1, BandBuffers& b,
                                                         Coefficients& out) noexcept
        {
            const bool         aq      = s.adaptiveQuant;
            const float* const field   = b.field;
            const std::int32_t ybBegin = r * s.maxV;
            for (std::int32_t c = 0; c < w.comps; ++c) {
                const auto          ci    = static_cast<std::size_t>(c);
                const ComponentInfo comp  = s.frame.component[ci];
                Plane&              plane = c == 0 ? b.y : (w.sub420 ? (c == 1 ? b.cb420 : b.cr420) : (c == 1 ? b.cb : b.cr));
                for (std::int32_t by = r * comp.vSamp; by < (r + 1) * comp.vSamp && by < s.heightInBlocks[ci]; ++by) {
                    const std::int32_t iy       = by % comp.vSamp;
                    const std::int32_t ybAq     = r * s.maxV + iy; // Y block row of the field
                    const float*       rowStart = plane.Row(by * DctSize);
                    const std::int32_t bxEnd    = c1 * comp.hSamp < s.widthInBlocks[ci] ? c1 * comp.hSamp : s.widthInBlocks[ci];
                    for (std::int32_t bx = c0 * comp.hSamp; bx < bxEnd; ++bx) {
                        const float aqStrength = aq ? field[static_cast<std::size_t>(ybAq - ybBegin) * w.yBlocks + static_cast<std::size_t>(bx * s.hFactor[ci])] : 0.0f;
                        std::int16_t* block = out.Block(c, bx, by);
                        QuantizeBlockV<L>(rowStart + bx * DctSize, static_cast<std::size_t>(plane.stride), s.quantMul[ci].data(), aqStrength,
                                          s.zeroBiasOffset[ci].data(), s.zeroBiasMul[ci].data(), block);
                        out.Dc(c, bx, by) = block[0];
                    }
                }
            }
        }

        // jpegli: ProcessiMCURows() and ProcessiMCURow() (encode.cc) for the tile of iMCU rows
        // [r0, r1) and iMCU columns [c0, c1).
        template <SimdLevel L>
        bool ComputeBand(const EncoderSetup& s, const Image& image, std::int32_t r0, std::int32_t r1, std::int32_t c0, std::int32_t c1, Coefficients& out,
                         PixelScratch& scratch) noexcept
        {
            // Row-wise like jpegli's ProcessiMCURow, with rings instead of whole-band windows so the
            // working set of a worker stays small: per iMCU row the new color rows, the new
            // pre-erosion rows, the field of its block rows and the DCT of its blocks. Only the
            // first iMCU row of a band computes the context above it. A strip of iMCU columns
            // [c0, c1) keeps only its own columns of the color planes, plus context columns on
            // either side (TileWindow), so a wide image does not push the worker out of the cache;
            // at the image edges it pads as jpegli does. The small rows (pre-erosion, fuzzy
            // erosion, field) stay full width.
            const TileWindow w = MakeTileWindow(s, image, c0, c1);
            BandBuffers      b;
            if (!AllocateBandBuffers(s, w, scratch, b))
                return false;
            QuantFieldScaling scaling;
            if (s.adaptiveQuant)
                scaling = ComputeQuantFieldScaling(s);

            for (std::int32_t r = r0; r < r1; ++r) {
                const bool firstRow = r == r0;
                ReadInputRows<L>(image, w, r, firstRow, b);
                if (w.sub420)
                    Downsample2x2Rows(w, r, b);
                if (s.adaptiveQuant)
                    ComputeAdaptiveQuantField<L>(w, r * s.maxV, (r + 1) * s.maxV, firstRow, scaling, b);
                ComputeCoefficientsForiMcuRow<L>(s, w, r, c0, c1, b, out);
            }
            return true;
        }

    } // namespace

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg

#include "PixelPhaseReference.inc"
