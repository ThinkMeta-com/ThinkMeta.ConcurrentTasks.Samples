//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#pragma once

#include <cstddef>
#include <cstdint>

// Constants and tables of the DEFLATE format and of the libdeflate 1.26 compressor (lib/deflate_constants.h,
// lib/deflate_compress.c). The tables are taken over as data.

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    constexpr std::uint32_t BlockTypeUncompressed    = 0;
    constexpr std::uint32_t BlockTypeStaticHuffman   = 1;
    constexpr std::uint32_t BlockTypeDynamicHuffman  = 2;
    constexpr std::uint32_t MinMatchLen              = 3;
    constexpr std::uint32_t MaxMatchLen              = 258;
    constexpr std::uint32_t MaxMatchOffset           = 32768;
    constexpr std::uint32_t NumPrecodeSyms           = 19;
    constexpr std::uint32_t NumLitlenSyms            = 288;
    constexpr std::uint32_t NumOffsetSyms            = 32;
    constexpr std::uint32_t MaxNumSyms               = 288;
    constexpr std::uint32_t NumLiterals              = 256;
    constexpr std::uint32_t EndOfBlock               = 256;
    constexpr std::uint32_t FirstLenSym              = 257;
    constexpr std::uint32_t MaxCodewordLen           = 15;
    constexpr std::uint32_t MaxLitlenCodewordLen     = 14; // libdeflate limits litlen codewords to 14 bits
    constexpr std::uint32_t MaxOffsetCodewordLen     = 15;
    constexpr std::uint32_t MaxPreCodewordLen        = 7;

    // Compressor parameters (deflate_compress.c).
    constexpr std::uint32_t MinBlockLength           = 5000;
    constexpr std::uint32_t SoftMaxBlockLength       = 300000;
    constexpr std::uint32_t SeqStoreLength           = 50000;
    constexpr std::uint32_t FastSoftMaxBlockLength   = 65535;
    constexpr std::uint32_t FastSeqStoreLength       = 8192;
    constexpr std::uint32_t NumLiteralObservations   = 8;
    constexpr std::uint32_t NumMatchObservations     = 2;
    constexpr std::uint32_t NumObservationTypes      = NumLiteralObservations + NumMatchObservations;
    constexpr std::uint32_t ObservationsPerBlockCheck = 512;

    /// <summary>libdeflate: deflate_length_slot_base[] (deflate_compress.c). Length slot => length slot base value.</summary>
    inline constexpr std::uint32_t LengthSlotBase[] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
    };

    /// <summary>libdeflate: deflate_extra_length_bits[] (deflate_compress.c). Length slot => number of extra length bits.</summary>
    inline constexpr std::uint8_t ExtraLengthBits[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
    };

    /// <summary>libdeflate: deflate_offset_slot_base[] (deflate_compress.c). Offset slot => offset slot base value.</summary>
    inline constexpr std::uint32_t OffsetSlotBase[] = {
        1,   2,   3,   4,   5,    7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
    };

    /// <summary>libdeflate: deflate_extra_offset_bits[] (deflate_compress.c). Offset slot => number of extra offset bits.</summary>
    inline constexpr std::uint8_t ExtraOffsetBits[] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
    };

    namespace Detail {
        // libdeflate: deflate_length_slot[] (deflate_compress.c), computed instead of written out
        constexpr auto MakeLengthSlotTable() noexcept
        {
            struct Table {
                std::uint8_t v[MaxMatchLen + 1];
            } t{};
            std::uint32_t slot = 0;
            for (std::uint32_t len = MinMatchLen; len <= MaxMatchLen; ++len) {
                while (slot + 1 < sizeof(LengthSlotBase) / sizeof(LengthSlotBase[0]) && LengthSlotBase[slot + 1] <= len)
                    ++slot;
                t.v[len] = static_cast<std::uint8_t>(slot);
            }
            return t;
        }

        // libdeflate: deflate_offset_slot[] (deflate_compress.c), computed instead of written out
        constexpr auto MakeOffsetSlotTable() noexcept
        {
            struct Table {
                std::uint8_t v[256];
            } t{};
            std::uint32_t slot = 0;
            for (std::uint32_t offset = 1; offset <= 256; ++offset) {
                while (OffsetSlotBase[slot + 1] <= offset)
                    ++slot;
                t.v[offset - 1] = static_cast<std::uint8_t>(slot);
            }
            return t;
        }

        inline constexpr auto LengthSlotTable = MakeLengthSlotTable();
        inline constexpr auto OffsetSlotTable = MakeOffsetSlotTable();
    } // namespace Detail

    /// <summary>libdeflate: deflate_length_slot[] (deflate_compress.c). Match length (3..258) => length slot.</summary>
    inline std::uint32_t LengthSlot(std::uint32_t length) noexcept
    {
        return Detail::LengthSlotTable.v[length];
    }

    /// <summary>libdeflate: deflate_get_offset_slot() (deflate_compress.c). Match offset (1..32768) => offset slot.
    /// Offsets above 256 share the small table: each slot from 16 on is 128 times wider than the slot 14 below it.</summary>
    inline std::uint32_t OffsetSlot(std::uint32_t offset) noexcept
    {
        const std::uint32_t n = (256 - offset) >> 29; // 0 for offset <= 256, else 7
        return Detail::OffsetSlotTable.v[(offset - 1) >> n] + (n << 1);
    }

    /// <summary>libdeflate: deflate_precode_lens_permutation[] (deflate_compress.c). The order in which precode
    /// codeword lengths are stored.</summary>
    inline constexpr std::uint8_t PrecodeLensPermutation[NumPrecodeSyms] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

    /// <summary>libdeflate: deflate_extra_precode_bits[] (deflate_compress.c). Precode symbol => number of extra bits.</summary>
    inline constexpr std::uint8_t ExtraPrecodeBits[NumPrecodeSyms] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 7};

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
