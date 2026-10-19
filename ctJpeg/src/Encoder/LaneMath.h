// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#pragma once

#include <immintrin.h>

#include "SimdFeatures.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    /// <summary>
    /// The arithmetic in which jpegli's Highway targets differ, for the scalar reference path.
    /// sse2 (Highway SSE2/SSSE3/SSE4): 4 lanes, MulAdd is a multiply followed by an add.
    /// avx2 (Highway AVX2): 8 lanes, MulAdd is a fused multiply-add.
    /// SumOfLanes follows Highway's generic reduction: within a 4-lane block
    /// (v0 + v3) + (v1 + v2); with two blocks the blocks are added first (v_i + v_{i+4}).
    /// </summary>
    template <SimdLevel L>
    struct Lane {
        static constexpr int Width = L == SimdLevel::Avx2 ? 8 : 4;

        static __forceinline float MulAdd(float mul, float x, float add) noexcept
        {
            if constexpr (L == SimdLevel::Avx2) // vfmadd: the same rounding as std::fma, without the CRT call
                return _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(mul), _mm_set_ss(x), _mm_set_ss(add)));
            else
                return mul * x + add;
        }

        static __forceinline float SumOfLanes(const float* v) noexcept
        {
            if constexpr (L == SimdLevel::Avx2) {
                const float u0 = v[0] + v[4], u1 = v[1] + v[5], u2 = v[2] + v[6], u3 = v[3] + v[7];
                return (u0 + u3) + (u1 + u2);
            }
            else
                return (v[0] + v[3]) + (v[1] + v[2]);
        }
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
