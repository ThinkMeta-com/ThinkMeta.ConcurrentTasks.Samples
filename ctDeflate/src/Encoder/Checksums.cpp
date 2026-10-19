//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#include "Checksums.h"

#include <cstring>

#include <immintrin.h>

namespace ThinkMeta::ConcurrentTasks::Samples::Deflate {

    namespace {

        constexpr std::uint32_t CrcPoly = 0xEDB88320u; // reflected

        /// <summary>Slice-by-8 tables: t[k][b] is the CRC of byte b followed by k zero bytes.</summary>
        struct CrcTables {
            std::uint32_t t[8][256];
        };

        constexpr CrcTables MakeCrcTables() noexcept
        {
            CrcTables tables{};
            for (std::uint32_t b = 0; b < 256; ++b) {
                std::uint32_t c = b;
                for (std::int32_t i = 0; i < 8; ++i)
                    c = (c & 1) != 0 ? (c >> 1) ^ CrcPoly : c >> 1;
                tables.t[0][b] = c;
            }
            for (std::uint32_t b = 0; b < 256; ++b)
                for (std::int32_t k = 1; k < 8; ++k)
                    tables.t[k][b] = (tables.t[k - 1][b] >> 8) ^ tables.t[0][tables.t[k - 1][b] & 0xFF];
            return tables;
        }

        constexpr CrcTables Tables = MakeCrcTables();

        /// <summary>Not in libdeflate; zlib's multmodp(). a * b modulo the CRC polynomial (reflected representation,
        /// x^0 is bit 31).</summary>
        std::uint32_t MultModP(std::uint32_t a, std::uint32_t b) noexcept
        {
            std::uint32_t m = 1u << 31;
            std::uint32_t p = 0;
            for (;;) {
                if ((a & m) != 0) {
                    p ^= b;
                    if ((a & (m - 1)) == 0)
                        break;
                }
                m >>= 1;
                b = (b & 1) != 0 ? (b >> 1) ^ CrcPoly : b >> 1;
            }
            return p;
        }

        /// <summary>Not in libdeflate; zlib's x2nmodp(). x^(n * 2^k) modulo the CRC polynomial.</summary>
        std::uint32_t X2nModP(std::uint64_t n, std::uint32_t k) noexcept
        {
            std::uint32_t x2n[32];
            std::uint32_t p = 1u << 30; // x^1
            x2n[0]          = p;
            for (std::uint32_t i = 1; i < 32; ++i)
                x2n[i] = p = MultModP(p, p);

            p = 1u << 31; // x^0
            while (n != 0) {
                if ((n & 1) != 0)
                    p = MultModP(x2n[k & 31], p);
                n >>= 1;
                ++k;
            }
            return p;
        }

        /// <summary>
        /// Not ported from libdeflate. Folds 64-byte blocks with carry-less multiplication and reduces with Barrett
        /// (Intel's CRC folding as used in zlib/Chromium). size: a multiple of 16, at least 64. crc: the inverted
        /// running CRC.
        /// </summary>
        std::uint32_t Crc32Pclmul(std::uint32_t crc, const std::uint8_t* data, std::size_t size) noexcept
        {
            const __m128i k1k2 = _mm_set_epi64x(0x01C6E41596, 0x0154442BD4);
            const __m128i k3k4 = _mm_set_epi64x(0x00CCAA009E, 0x01751997D0);
            const __m128i k5k0 = _mm_set_epi64x(0x0000000000, 0x0163CD6124);
            const __m128i poly = _mm_set_epi64x(0x01F7011641, 0x01DB710641);

            auto load = [](const std::uint8_t* p) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)); };

            __m128i x1 = load(data);
            __m128i x2 = load(data + 16);
            __m128i x3 = load(data + 32);
            __m128i x4 = load(data + 48);
            x1         = _mm_xor_si128(x1, _mm_cvtsi32_si128(static_cast<int>(crc)));
            data += 64;
            size -= 64;

            auto fold = [](__m128i x, __m128i k, __m128i next) {
                const __m128i lo = _mm_clmulepi64_si128(x, k, 0x00);
                const __m128i hi = _mm_clmulepi64_si128(x, k, 0x11);
                return _mm_xor_si128(_mm_xor_si128(lo, hi), next);
            };

            while (size >= 64) {
                x1 = fold(x1, k1k2, load(data));
                x2 = fold(x2, k1k2, load(data + 16));
                x3 = fold(x3, k1k2, load(data + 32));
                x4 = fold(x4, k1k2, load(data + 48));
                data += 64;
                size -= 64;
            }

            x1 = fold(x1, k3k4, x2);
            x1 = fold(x1, k3k4, x3);
            x1 = fold(x1, k3k4, x4);
            while (size >= 16) {
                x1 = fold(x1, k3k4, load(data));
                data += 16;
                size -= 16;
            }

            // 128 -> 64 bits
            const __m128i mask32 = _mm_setr_epi32(-1, 0, -1, 0);
            __m128i       t      = _mm_clmulepi64_si128(x1, k3k4, 0x10);
            x1                   = _mm_xor_si128(_mm_srli_si128(x1, 8), t);
            t                    = _mm_srli_si128(x1, 4);
            x1                   = _mm_and_si128(x1, mask32);
            x1                   = _mm_xor_si128(_mm_clmulepi64_si128(x1, k5k0, 0x00), t);

            // Barrett reduction to 32 bits
            t  = _mm_and_si128(x1, mask32);
            t  = _mm_clmulepi64_si128(t, poly, 0x10);
            t  = _mm_and_si128(t, mask32);
            t  = _mm_clmulepi64_si128(t, poly, 0x00);
            x1 = _mm_xor_si128(x1, t);
            return static_cast<std::uint32_t>(_mm_extract_epi32(x1, 1));
        }

    } // namespace

    // Own implementation, not ported from libdeflate; same result as libdeflate_crc32() (crc32.c)
    std::uint32_t Crc32(std::uint32_t crc, const std::uint8_t* data, std::size_t size) noexcept
    {
        const auto& t = Tables.t;
        crc           = ~crc;
        if (size >= 64) {
            const std::size_t n = size & ~static_cast<std::size_t>(15);
            crc                 = Crc32Pclmul(crc, data, n);
            data += n;
            size -= n;
        }
        while (size >= 8) {
            std::uint32_t lo;
            std::uint32_t hi;
            std::memcpy(&lo, data, 4);
            std::memcpy(&hi, data + 4, 4);
            lo ^= crc;
            crc = t[7][lo & 0xFF] ^ t[6][(lo >> 8) & 0xFF] ^ t[5][(lo >> 16) & 0xFF] ^ t[4][lo >> 24] ^ t[3][hi & 0xFF] ^ t[2][(hi >> 8) & 0xFF]
                ^ t[1][(hi >> 16) & 0xFF] ^ t[0][hi >> 24];
            data += 8;
            size -= 8;
        }
        while (size-- != 0)
            crc = (crc >> 8) ^ t[0][(crc ^ *data++) & 0xFF];
        return ~crc;
    }

    // Not in libdeflate; zlib's crc32_combine()
    std::uint32_t Crc32Combine(std::uint32_t crcA, std::uint32_t crcB, std::uint64_t sizeB) noexcept
    {
        return MultModP(X2nModP(sizeB, 3), crcA) ^ crcB;
    }

    // libdeflate: adler32_generic() (adler32.c), without its four-byte unrolling
    std::uint32_t Adler32(std::uint32_t adler, const std::uint8_t* data, std::size_t size) noexcept
    {
        constexpr std::uint32_t Base = 65521;
        constexpr std::size_t   NMax = 5552; // largest n with 255n(n+1)/2 + (n+1)(Base-1) < 2^32

        std::uint32_t s1 = adler & 0xFFFF;
        std::uint32_t s2 = adler >> 16;
        while (size != 0) {
            const std::size_t n = size < NMax ? size : NMax;
            for (std::size_t i = 0; i < n; ++i) {
                s1 += data[i];
                s2 += s1;
            }
            s1 %= Base;
            s2 %= Base;
            data += n;
            size -= n;
        }
        return (s2 << 16) | s1;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Deflate
