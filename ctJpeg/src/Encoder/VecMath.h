// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#pragma once

#include <cstdint>

#include <immintrin.h>

#include "Compiler.h"
#include "SimdFeatures.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    /// <summary>
    /// Float vectors with Highway's operation semantics for the two feature sets:
    /// sse2 = 4 lanes, MulAdd as multiply then add (Highway SSE2/SSSE3/SSE4); avx2 = 8 lanes, MulAdd
    /// fused (Highway AVX2). Min/Max are minps/maxps with jpegli's operand order, Round is
    /// round-to-nearest-even (cvtps2dq under the default MXCSR). The avx2 specialization must only
    /// be used in translation units compiled with /arch:AVX2.
    /// </summary>
    template <SimdLevel L>
    struct Vec;

    template <>
    struct Vec<SimdLevel::Sse2> {
        using T                    = __m128;
        static constexpr int Width = 4;

        static CT_FORCEINLINE T    Load(const float* p) noexcept { return _mm_loadu_ps(p); }
        static CT_FORCEINLINE void Store(float* p, T v) noexcept { _mm_storeu_ps(p, v); }
        static CT_FORCEINLINE T    Set(float x) noexcept { return _mm_set1_ps(x); }
        static CT_FORCEINLINE T    Zero() noexcept { return _mm_setzero_ps(); }
        static CT_FORCEINLINE T    Add(T a, T b) noexcept { return _mm_add_ps(a, b); }
        static CT_FORCEINLINE T    Sub(T a, T b) noexcept { return _mm_sub_ps(a, b); }
        static CT_FORCEINLINE T    Mul(T a, T b) noexcept { return _mm_mul_ps(a, b); }
        static CT_FORCEINLINE T    Div(T a, T b) noexcept { return _mm_div_ps(a, b); }
        static CT_FORCEINLINE T    Sqrt(T a) noexcept { return _mm_sqrt_ps(a); }
        static CT_FORCEINLINE T    Min(T a, T b) noexcept { return _mm_min_ps(a, b); }
        static CT_FORCEINLINE T    Max(T a, T b) noexcept { return _mm_max_ps(a, b); }
        static CT_FORCEINLINE T    MulAdd(T mul, T x, T add) noexcept { return _mm_add_ps(_mm_mul_ps(mul, x), add); }
        static CT_FORCEINLINE T    Abs(T a) noexcept { return _mm_and_ps(a, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF))); }
        static CT_FORCEINLINE T    And(T a, T b) noexcept { return _mm_and_ps(a, b); }
        // sign bit set -> 0 (Highway ZeroIfNegative)
        static CT_FORCEINLINE T ZeroIfNegative(T v) noexcept { return _mm_andnot_ps(_mm_castsi128_ps(_mm_srai_epi32(_mm_castps_si128(v), 31)), v); }
        // |qval| >= threshold ? round(qval) : 0, as int32
        static CT_FORCEINLINE void QuantizeStore(T qval, T threshold, std::int32_t* out) noexcept
        {
            const __m128  mask = _mm_cmpge_ps(Abs(qval), threshold);
            const __m128i ival = _mm_and_si128(_mm_cvtps_epi32(qval), _mm_castps_si128(mask));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out), ival);
        }
    };

    template <>
    struct Vec<SimdLevel::Avx2> {
        using T                    = __m256;
        static constexpr int Width = 8;

        static CT_FORCEINLINE T    Load(const float* p) noexcept { return _mm256_loadu_ps(p); }
        static CT_FORCEINLINE void Store(float* p, T v) noexcept { _mm256_storeu_ps(p, v); }
        static CT_FORCEINLINE T    Set(float x) noexcept { return _mm256_set1_ps(x); }
        static CT_FORCEINLINE T    Zero() noexcept { return _mm256_setzero_ps(); }
        static CT_FORCEINLINE T    Add(T a, T b) noexcept { return _mm256_add_ps(a, b); }
        static CT_FORCEINLINE T    Sub(T a, T b) noexcept { return _mm256_sub_ps(a, b); }
        static CT_FORCEINLINE T    Mul(T a, T b) noexcept { return _mm256_mul_ps(a, b); }
        static CT_FORCEINLINE T    Div(T a, T b) noexcept { return _mm256_div_ps(a, b); }
        static CT_FORCEINLINE T    Sqrt(T a) noexcept { return _mm256_sqrt_ps(a); }
        static CT_FORCEINLINE T    Min(T a, T b) noexcept { return _mm256_min_ps(a, b); }
        static CT_FORCEINLINE T    Max(T a, T b) noexcept { return _mm256_max_ps(a, b); }
        static CT_FORCEINLINE T    MulAdd(T mul, T x, T add) noexcept { return _mm256_fmadd_ps(mul, x, add); }
        static CT_FORCEINLINE T    Abs(T a) noexcept { return _mm256_and_ps(a, _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF))); }
        static CT_FORCEINLINE T    And(T a, T b) noexcept { return _mm256_and_ps(a, b); }
        static CT_FORCEINLINE T ZeroIfNegative(T v) noexcept { return _mm256_andnot_ps(_mm256_castsi256_ps(_mm256_srai_epi32(_mm256_castps_si256(v), 31)), v); }
        static CT_FORCEINLINE void QuantizeStore(T qval, T threshold, std::int32_t* out) noexcept
        {
            const __m256  mask = _mm256_cmp_ps(Abs(qval), threshold, _CMP_GE_OQ);
            const __m256i ival = _mm256_and_si256(_mm256_cvtps_epi32(qval), _mm256_castps_si256(mask));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(out), ival);
        }
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
