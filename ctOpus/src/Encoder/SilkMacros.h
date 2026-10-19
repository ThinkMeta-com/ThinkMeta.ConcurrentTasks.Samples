// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <type_traits>

#include "CeltMath.h"

// The fixed-point macros of SILK (libopus 1.6.1 silk/SigProc_FIX.h, silk/macros.h,
// silk/Inlines.h) and the float helpers of silk/float/SigProc_FLP.h, for Win64
// (OPUS_FAST_INT64). The macros are type-generic in C; where they are, these functions are
// templates with auto results, so the integer promotions are exactly those of the C
// expressions (an int16 + int16 stays an int, an int64 operand keeps the result 64 bit).

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk {

    using std::int16_t;
    using std::int32_t;
    using std::int64_t;
    using std::int8_t;
    using std::uint16_t;
    using std::uint32_t;
    using std::uint64_t;
    using std::uint8_t;

    constexpr int64_t Int64Max = 0x7FFFFFFFFFFFFFFFLL;
    constexpr int64_t Int64Min = static_cast<int64_t>(0x8000000000000000ULL);
    constexpr int32_t Int32Max = 0x7FFFFFFF;
    constexpr int32_t Int32Min = static_cast<int32_t>(0x80000000U);
    constexpr int32_t Int16Max = 0x7FFF;
    constexpr int16_t Int16Min = static_cast<int16_t>(0x8000);
    constexpr int32_t Int8Max  = 0x7F;
    constexpr int8_t  Int8Min  = static_cast<int8_t>(0x80);
    constexpr int32_t Uint8Max = 0xFF;

    // --- macros.h (OPUS_FAST_INT64) ------------------------------------------------------------

    constexpr int32_t Smulwb(int32_t a32, int32_t b32) noexcept
    {
        return static_cast<int32_t>((a32 * static_cast<int64_t>(static_cast<int16_t>(b32))) >> 16);
    }
    constexpr int32_t Smlawb(int32_t a32, int32_t b32, int32_t c32) noexcept
    {
        return static_cast<int32_t>(a32 + ((b32 * static_cast<int64_t>(static_cast<int16_t>(c32))) >> 16));
    }
    constexpr int32_t Smulwt(int32_t a32, int32_t b32) noexcept
    {
        return static_cast<int32_t>((a32 * static_cast<int64_t>(b32 >> 16)) >> 16);
    }
    constexpr int32_t Smlawt(int32_t a32, int32_t b32, int32_t c32) noexcept
    {
        return static_cast<int32_t>(a32 + ((b32 * (static_cast<int64_t>(c32) >> 16)) >> 16));
    }
    constexpr int32_t Smulbb(int32_t a32, int32_t b32) noexcept
    {
        return static_cast<int32_t>(static_cast<int16_t>(a32)) * static_cast<int32_t>(static_cast<int16_t>(b32));
    }
    constexpr int32_t Smlabb(int32_t a32, int32_t b32, int32_t c32) noexcept
    {
        return a32 + static_cast<int32_t>(static_cast<int16_t>(b32)) * static_cast<int32_t>(static_cast<int16_t>(c32));
    }
    constexpr int32_t Smulbt(int32_t a32, int32_t b32) noexcept
    {
        return static_cast<int32_t>(static_cast<int16_t>(a32)) * (b32 >> 16);
    }
    constexpr int32_t Smlabt(int32_t a32, int32_t b32, int32_t c32) noexcept
    {
        return a32 + static_cast<int32_t>(static_cast<int16_t>(b32)) * (c32 >> 16);
    }
    constexpr int64_t Smlal(int64_t a64, int32_t b32, int32_t c32) noexcept
    {
        return a64 + static_cast<int64_t>(b32) * static_cast<int64_t>(c32);
    }
    constexpr int32_t Smulww(int32_t a32, int32_t b32) noexcept
    {
        return static_cast<int32_t>((static_cast<int64_t>(a32) * b32) >> 16);
    }
    constexpr int32_t Smlaww(int32_t a32, int32_t b32, int32_t c32) noexcept
    {
        return static_cast<int32_t>(a32 + ((static_cast<int64_t>(b32) * c32) >> 16));
    }

    constexpr int32_t AddSat32(int32_t a, int32_t b) noexcept
    {
        return ((static_cast<uint32_t>(a) + static_cast<uint32_t>(b)) & 0x80000000U) == 0
            ? (((a & b) & 0x80000000) != 0 ? Int32Min : static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)))
            : (((a | b) & 0x80000000) == 0 ? Int32Max : static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)));
    }
    constexpr int32_t SubSat32(int32_t a, int32_t b) noexcept
    {
        return ((static_cast<uint32_t>(a) - static_cast<uint32_t>(b)) & 0x80000000U) == 0
            ? (((a & (b ^ static_cast<int32_t>(0x80000000U)) & static_cast<int32_t>(0x80000000U)) != 0)
                   ? Int32Min
                   : static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b)))
            : ((((a ^ static_cast<int32_t>(0x80000000U)) & b & static_cast<int32_t>(0x80000000U)) != 0)
                   ? Int32Max
                   : static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b)));
    }

    inline int32_t Clz16(int16_t in16) noexcept
    {
        return 32 - static_cast<int32_t>(std::bit_width((static_cast<uint32_t>(static_cast<uint16_t>(in16)) << 16) | 0x8000U));
    }
    inline int32_t Clz32(int32_t in32) noexcept
    {
        return in32 != 0 ? 32 - static_cast<int32_t>(std::bit_width(static_cast<uint32_t>(in32))) : 32;
    }

    // --- SigProc_FIX.h ---------------------------------------------------------------------

    inline int32_t Ror32(int32_t a32, int32_t rot) noexcept
    {
        const auto x = static_cast<uint32_t>(a32);
        const auto r = static_cast<uint32_t>(rot);
        const auto m = static_cast<uint32_t>(-rot);
        if (rot == 0)
            return a32;
        if (rot < 0)
            return static_cast<int32_t>((x << m) | (x >> (32 - m)));
        return static_cast<int32_t>((x << (32 - r)) | (x >> r));
    }

    template <typename A, typename B>
    constexpr auto Mul(A a32, B b32) noexcept
    {
        return a32 * b32;
    }
    template <typename A, typename B, typename C>
    constexpr auto Mla(A a32, B b32, C c32) noexcept
    {
        return a32 + b32 * c32;
    }
    constexpr int32_t Smultt(int32_t a32, int32_t b32) noexcept
    {
        return (a32 >> 16) * (b32 >> 16);
    }
    constexpr int32_t Smlatt(int32_t a32, int32_t b32, int32_t c32) noexcept
    {
        return a32 + (b32 >> 16) * (c32 >> 16);
    }
    constexpr int64_t Smlalbb(int64_t a64, int32_t b16, int32_t c16) noexcept
    {
        return a64 + static_cast<int64_t>(static_cast<int32_t>(b16) * static_cast<int32_t>(c16));
    }
    constexpr int64_t Smull(int32_t a32, int32_t b32) noexcept
    {
        return static_cast<int64_t>(a32) * b32;
    }

    constexpr int32_t Add32Ovflw(int32_t a, int32_t b) noexcept
    {
        return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
    }
    constexpr int32_t Sub32Ovflw(int32_t a, int32_t b) noexcept
    {
        return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
    }
    constexpr int32_t MlaOvflw(int32_t a32, int32_t b32, int32_t c32) noexcept
    {
        return Add32Ovflw(a32, static_cast<int32_t>(static_cast<uint32_t>(b32) * static_cast<uint32_t>(c32)));
    }
    constexpr int32_t SmlabbOvflw(int32_t a32, int32_t b32, int32_t c32) noexcept
    {
        return Add32Ovflw(a32, static_cast<int32_t>(static_cast<int16_t>(b32)) * static_cast<int32_t>(static_cast<int16_t>(c32)));
    }

    template <typename A, typename B>
    constexpr int32_t Div32_16(A a32, B b16) noexcept
    {
        return static_cast<int32_t>(a32 / b16);
    }
    template <typename A, typename B>
    constexpr int32_t Div32(A a32, B b32) noexcept
    {
        return static_cast<int32_t>(a32 / b32);
    }

    template <typename A, typename B>
    constexpr auto Add32(A a, B b) noexcept
    {
        return a + b;
    }
    template <typename A, typename B>
    constexpr auto Sub32(A a, B b) noexcept
    {
        return a - b;
    }

    template <typename T>
    constexpr auto Sat8(T a) noexcept -> std::common_type_t<int32_t, T>
    {
        return a > Int8Max ? Int8Max : (a < Int8Min ? Int8Min : a);
    }
    template <typename T>
    constexpr auto Sat16(T a) noexcept -> std::common_type_t<int32_t, T>
    {
        return a > Int16Max ? Int16Max : (a < Int16Min ? Int16Min : a);
    }
    template <typename T>
    constexpr auto Sat32(T a) noexcept -> std::common_type_t<int32_t, T>
    {
        return a > Int32Max ? Int32Max : (a < Int32Min ? Int32Min : a);
    }

    template <typename B>
    constexpr int16_t AddSat16(int32_t a, B b) noexcept
    {
        return static_cast<int16_t>(Sat16(static_cast<int32_t>(a) + b));
    }
    template <typename B>
    constexpr int16_t SubSat16(int32_t a, B b) noexcept
    {
        return static_cast<int16_t>(Sat16(static_cast<int32_t>(a) - b));
    }
    constexpr int64_t AddSat64(int64_t a, int64_t b) noexcept
    {
        const int64_t s = static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
        return (s & Int64Min) == 0 ? (((a & b) & Int64Min) != 0 ? Int64Min : s) : (((a | b) & Int64Min) == 0 ? Int64Max : s);
    }
    constexpr int64_t SubSat64(int64_t a, int64_t b) noexcept
    {
        const int64_t d = static_cast<int64_t>(static_cast<uint64_t>(a) - static_cast<uint64_t>(b));
        return (d & Int64Min) == 0 ? ((a & (b ^ Int64Min) & Int64Min) != 0 ? Int64Min : d) : (((a ^ Int64Min) & b & Int64Min) != 0 ? Int64Max : d);
    }

    template <typename T>
    constexpr auto PosSat32(T a) noexcept -> std::common_type_t<int32_t, T>
    {
        return a > Int32Max ? Int32Max : a;
    }
    template <typename A, typename B>
    constexpr auto AddPosSat8(A a, B b) noexcept -> std::common_type_t<int32_t, decltype(a + b)>
    {
        return ((a + b) & 0x80) != 0 ? Int8Max : (a + b);
    }
    template <typename A, typename B>
    constexpr auto AddPosSat16(A a, B b) noexcept -> std::common_type_t<int32_t, decltype(a + b)>
    {
        return ((a + b) & 0x8000) != 0 ? Int16Max : (a + b);
    }
    constexpr int32_t AddPosSat32(int32_t a, int32_t b) noexcept
    {
        return ((static_cast<uint32_t>(a) + static_cast<uint32_t>(b)) & 0x80000000U) != 0
            ? Int32Max
            : static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
    }

    template <typename T>
    constexpr int8_t Lshift8(T a, int32_t shift) noexcept
    {
        return static_cast<int8_t>(static_cast<uint8_t>(a) << shift);
    }
    template <typename T>
    constexpr int16_t Lshift16(T a, int32_t shift) noexcept
    {
        return static_cast<int16_t>(static_cast<uint16_t>(a) << shift);
    }
    template <typename T>
    constexpr int32_t Lshift32(T a, int32_t shift) noexcept
    {
        return static_cast<int32_t>(static_cast<uint32_t>(a) << shift);
    }
    template <typename T>
    constexpr int64_t Lshift64(T a, int32_t shift) noexcept
    {
        return static_cast<int64_t>(static_cast<uint64_t>(a) << shift);
    }
    template <typename T>
    constexpr int32_t Lshift(T a, int32_t shift) noexcept
    {
        return Lshift32(a, shift);
    }

    template <typename T>
    constexpr auto Rshift(T a, int32_t shift) noexcept
    {
        return a >> shift;
    }
    template <typename T>
    constexpr auto Rshift32(T a, int32_t shift) noexcept
    {
        return a >> shift;
    }
    template <typename T>
    constexpr auto Rshift64(T a, int32_t shift) noexcept
    {
        return a >> shift;
    }
    template <typename T>
    constexpr auto RshiftUint(T a, int32_t shift) noexcept
    {
        return a >> shift;
    }
    template <typename T>
    constexpr auto LshiftUint(T a, int32_t shift) noexcept
    {
        return a << shift;
    }

    template <typename A, typename L1, typename L2>
    constexpr auto Limit(A a, L1 limit1, L2 limit2) noexcept -> std::common_type_t<A, L1, L2>
    {
        return limit1 > limit2 ? (a > limit1 ? limit1 : (a < limit2 ? limit2 : a)) : (a > limit2 ? limit2 : (a < limit1 ? limit1 : a));
    }

    constexpr int32_t LshiftSat32(int32_t a, int32_t shift) noexcept
    {
        return Lshift32(Limit(a, Int32Min >> shift, Int32Max >> shift), shift);
    }
    template <typename T>
    constexpr int32_t LshiftOvflw(T a, int32_t shift) noexcept
    {
        return static_cast<int32_t>(static_cast<uint32_t>(a) << shift);
    }

    template <typename A, typename B>
    constexpr auto AddLshift(A a, B b, int32_t shift) noexcept
    {
        return a + Lshift(b, shift);
    }
    template <typename A, typename B>
    constexpr auto AddLshift32(A a, B b, int32_t shift) noexcept
    {
        return a + Lshift32(b, shift);
    }
    template <typename A, typename B>
    constexpr auto AddRshift(A a, B b, int32_t shift) noexcept
    {
        return a + (b >> shift);
    }
    template <typename A, typename B>
    constexpr auto AddRshift32(A a, B b, int32_t shift) noexcept
    {
        return a + (b >> shift);
    }
    template <typename A, typename B>
    constexpr auto SubLshift32(A a, B b, int32_t shift) noexcept
    {
        return a - Lshift32(b, shift);
    }
    template <typename A, typename B>
    constexpr auto SubRshift32(A a, B b, int32_t shift) noexcept
    {
        return a - (b >> shift);
    }

    // shift > 0
    template <typename T>
    constexpr auto RshiftRound(T a, int32_t shift) noexcept
    {
        return shift == 1 ? (a >> 1) + (a & 1) : ((a >> (shift - 1)) + 1) >> 1;
    }
    template <typename T>
    constexpr auto RshiftRound64(T a, int32_t shift) noexcept
    {
        return shift == 1 ? (a >> 1) + (a & 1) : ((a >> (shift - 1)) + 1) >> 1;
    }

    template <typename A, typename B>
    constexpr auto Min(A a, B b) noexcept -> std::common_type_t<A, B>
    {
        return a < b ? a : b;
    }
    template <typename A, typename B>
    constexpr auto Max(A a, B b) noexcept -> std::common_type_t<A, B>
    {
        return a > b ? a : b;
    }
    constexpr int32_t MinInt(int32_t a, int32_t b) noexcept
    {
        return a < b ? a : b;
    }
    constexpr int32_t MaxInt(int32_t a, int32_t b) noexcept
    {
        return a > b ? a : b;
    }
    constexpr int16_t Min16(int16_t a, int16_t b) noexcept
    {
        return a < b ? a : b;
    }
    constexpr int16_t Max16(int16_t a, int16_t b) noexcept
    {
        return a > b ? a : b;
    }
    constexpr int32_t Min32(int32_t a, int32_t b) noexcept
    {
        return a < b ? a : b;
    }
    constexpr int32_t Max32(int32_t a, int32_t b) noexcept
    {
        return a > b ? a : b;
    }
    constexpr int64_t Min64(int64_t a, int64_t b) noexcept
    {
        return a < b ? a : b;
    }
    constexpr int64_t Max64(int64_t a, int64_t b) noexcept
    {
        return a > b ? a : b;
    }

    // wrong for the minimum value of the type, as in SILK
    template <typename T>
    constexpr auto Abs(T a) noexcept
    {
        return a > 0 ? a : -a;
    }
    constexpr int32_t AbsInt32(int32_t a) noexcept
    {
        return (a ^ (a >> 31)) - (a >> 31);
    }
    constexpr int64_t AbsInt64(int64_t a) noexcept
    {
        return a > 0 ? a : -a;
    }

    template <typename T>
    constexpr int32_t Sign(T a) noexcept
    {
        return a > 0 ? 1 : (a < 0 ? -1 : 0);
    }

    // the pseudo-random generator: keep the result as the next seed
    constexpr int32_t RandMultiplier = 196314165;
    constexpr int32_t RandIncrement  = 907633515;
    constexpr int32_t Rand(int32_t seed) noexcept
    {
        return MlaOvflw(RandIncrement, seed, RandMultiplier);
    }

    constexpr int32_t Smmul(int32_t a32, int32_t b32) noexcept
    {
        return static_cast<int32_t>(Smull(a32, b32) >> 32);
    }

    // SILK_FIX_CONST: a float or double C times 2^Q (as float or double), plus 0.5
    template <typename T>
    constexpr int32_t FixConst(T c, int32_t q) noexcept
    {
        return static_cast<int32_t>(c * (static_cast<int64_t>(1) << q) + 0.5);
    }

    // --- Inlines.h -------------------------------------------------------------------------

    inline int32_t Clz64(int64_t in) noexcept
    {
        const auto inUpper = static_cast<int32_t>(in >> 32);
        if (inUpper == 0)
            return 32 + Clz32(static_cast<int32_t>(in)); // search in the lower 32 bits
        return Clz32(inUpper);
    }

    // the number of leading zeros and the 7 bits right after the leading one
    inline void ClzFrac(int32_t in, int32_t* lz, int32_t* fracQ7) noexcept
    {
        const int32_t lzeros = Clz32(in);
        *lz                  = lzeros;
        *fracQ7              = Ror32(in, 24 - lzeros) & 0x7f;
    }

    // square root approximation: < +/- 10% for outputs > 15, < +/- 2.5% for outputs > 120
    inline int32_t SqrtApprox(int32_t x) noexcept
    {
        if (x <= 0)
            return 0;
        int32_t lz     = 0;
        int32_t fracQ7 = 0;
        ClzFrac(x, &lz, &fracQ7);
        int32_t y = (lz & 1) != 0 ? 32768 : 46214; // 46214 = sqrt(2) * 32768
        // the scaling
        y >>= (lz >> 1);
        // increment by the fractional part of the input
        y = Smlawb(y, y, Smulbb(213, fracQ7));
        return y;
    }

    // a good approximation of (a32 << Qres) / b32
    inline int32_t Div32VarQ(int32_t a32, int32_t b32, int32_t qres) noexcept
    {
        // the bits of head room, normalised inputs
        const int32_t aHeadrm = Clz32(Abs(a32)) - 1;
        int32_t       a32Nrm  = Lshift(a32, aHeadrm);
        const int32_t bHeadrm = Clz32(Abs(b32)) - 1;
        const int32_t b32Nrm  = Lshift(b32, bHeadrm);
        // the inverse of b32 with 14 bits of precision
        const int32_t b32Inv = Div32_16(Int32Max >> 2, b32Nrm >> 16);
        // the first approximation
        int32_t result = Smulwb(a32Nrm, b32Inv);
        // the residual: subtracting denominator times first approximation (may overflow: the
        // final value of a32_nrm is always small)
        a32Nrm = Sub32Ovflw(a32Nrm, LshiftOvflw(Smmul(b32Nrm, result), 3));
        // refinement
        result = Smlawb(result, a32Nrm, b32Inv);
        // to the Qres domain
        const int32_t lshift = 29 + aHeadrm - bHeadrm - qres;
        if (lshift < 0)
            return LshiftSat32(result, -lshift);
        if (lshift < 32)
            return result >> lshift;
        return 0; // avoids an undefined result
    }

    // a good approximation of (1 << Qres) / b32
    inline int32_t Inverse32VarQ(int32_t b32, int32_t qres) noexcept
    {
        const int32_t bHeadrm = Clz32(Abs(b32)) - 1;
        const int32_t b32Nrm  = Lshift(b32, bHeadrm);
        // the inverse of b32 with 14 bits of precision
        const int32_t b32Inv = Div32_16(Int32Max >> 2, b32Nrm >> 16);
        // the first approximation
        int32_t result = Lshift(b32Inv, 16);
        // the residual: one minus denominator times first approximation
        const int32_t errQ32 = Lshift((static_cast<int32_t>(1) << 29) - Smulwb(b32Nrm, b32Inv), 3);
        // refinement
        result = Smlaww(result, errQ32, b32Inv);
        // to the Qres domain
        const int32_t lshift = 61 - bHeadrm - qres;
        if (lshift <= 0)
            return LshiftSat32(result, -lshift);
        if (lshift < 32)
            return result >> lshift;
        return 0; // avoids an undefined result
    }

    // --- SigProc_FLP.h ---------------------------------------------------------------------

    constexpr float Pi = 3.1415926536f;

    // macros in C: a double operand keeps the comparison and the result double
    template <typename A, typename B>
    constexpr auto MinFloat(A a, B b) noexcept -> std::common_type_t<A, B>
    {
        return a < b ? a : b;
    }
    template <typename A, typename B>
    constexpr auto MaxFloat(A a, B b) noexcept -> std::common_type_t<A, B>
    {
        return a > b ? a : b;
    }
    inline float AbsFloat(float a) noexcept
    {
        return static_cast<float>(std::fabs(a));
    }
    inline float Sigmoid(float x) noexcept
    {
        return static_cast<float>(1.0 / (1.0 + std::exp(-static_cast<double>(x))));
    }
    inline int32_t Float2Int(float x) noexcept
    {
        return Opus::Float2Int(x);
    }
    inline void Float2ShortArray(int16_t* out, const float* in, int32_t length) noexcept
    {
        for (int32_t k = length - 1; k >= 0; --k)
            out[k] = static_cast<int16_t>(Sat16(Opus::Float2Int(in[k])));
    }
    inline void Short2FloatArray(float* out, const int16_t* in, int32_t length) noexcept
    {
        for (int32_t k = length - 1; k >= 0; --k)
            out[k] = static_cast<float>(in[k]);
    }
    inline float Log2(double x) noexcept
    {
        return static_cast<float>(3.32192809488736 * std::log10(x));
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::silk
