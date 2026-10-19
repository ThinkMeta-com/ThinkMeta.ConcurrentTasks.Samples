// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#pragma once

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    // The bit packing shared by BitWriter (the entropy-coded segment) and RawBitWriter (the plain
    // bit stream of a scan chunk, ChunkSplice.cpp). Hot paths: always inlined.

    /// <summary>
    /// Non-zero if and only if one of the 8 bytes of x is zero; HasZeroByte(~x) finds a 0xFF byte.
    /// jpegli: HasZeroByte() (bit_writer.h).
    /// </summary>
    __forceinline std::uint64_t HasZeroByte(std::uint64_t x) noexcept
    {
        return (x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL;
    }

    /// <summary>Stores word big-endian at out (8 bytes).</summary>
    __forceinline void StoreBigEndian64(std::uint64_t word, std::uint8_t* out) noexcept
    {
        for (std::int32_t i = 0; i < 8; ++i)
            out[i] = static_cast<std::uint8_t>(word >> (56 - 8 * i));
    }

    /// <summary>
    /// Appends the low nbits of bits to a 64-bit buffer filled from the top; when the buffer is
    /// full, discharge() writes it out (it reads putBuffer). nbits 0 means a symbol without a
    /// Huffman code: nothing is written and healthy becomes false.
    /// jpegli: WriteBits() (bit_writer.h).
    /// </summary>
    template <class Discharge>
    __forceinline void PackBits(std::uint64_t& putBuffer, std::int32_t& freeBits, bool& healthy, std::int32_t nbits, std::uint64_t bits,
                                Discharge&& discharge) noexcept
    {
        if (nbits == 0) {
            healthy = false;
            return;
        }
        freeBits -= nbits;
        if (freeBits < 0) {
            // the top -freeBits bits of bits complete the buffer, the rest starts the next one
            putBuffer <<= (freeBits + nbits);
            putBuffer |= (bits >> -freeBits);
            discharge();
            freeBits += 64;
            // jpegli's put_buffer = nbits: any value will do. The buffer only counts its low
            // 64 - freeBits bits, and the bits above them (this value and the already written
            // high bits of bits, ORed in below) are shifted out before the buffer is written
            // again: by the next overflow, or by JumpToByteBoundary's shift by freeBits.
            putBuffer = static_cast<std::uint64_t>(nbits);
        }
        putBuffer <<= nbits;
        putBuffer |= bits;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
