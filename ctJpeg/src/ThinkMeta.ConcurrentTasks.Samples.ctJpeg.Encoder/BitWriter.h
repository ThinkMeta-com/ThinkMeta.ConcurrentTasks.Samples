// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#pragma once

#include <cstdint>

#include "ByteBuffer.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    /// <summary>
    /// Packs Huffman codes into the entropy-coded segment, port of jpegli's JpegBitWriter
    /// (bit_writer.h): a 64-bit buffer filled from the top, written out big-endian, with a zero
    /// byte stuffed after every 0xFF.
    /// </summary>
    class BitWriter
    {
    public:
        /// <summary>Appends to output; the output must outlive the writer.</summary>
        explicit BitWriter(ByteBuffer& output) noexcept : output_(output) {}

        /// <summary>
        /// Writes the low nbits of bits (nbits 1..64). nbits 0 means a symbol without a Huffman code:
        /// nothing is written and the writer becomes unhealthy, as in jpegli.
        /// </summary>
        void WriteBits(std::int32_t nbits, std::uint64_t bits) noexcept
        {
            if (nbits == 0) {
                healthy_ = false;
                return;
            }
            freeBits_ -= nbits;
            if (freeBits_ < 0) {
                putBuffer_ <<= (freeBits_ + nbits);
                putBuffer_ |= (bits >> -freeBits_);
                DischargeBitBuffer();
                freeBits_ += 64;
                putBuffer_ = static_cast<std::uint64_t>(nbits);
            }
            putBuffer_ <<= nbits;
            putBuffer_ |= bits;
        }

        /// <summary>Pads the last byte with 1 bits and writes out the buffer (JumpToByteBoundary).</summary>
        void JumpToByteBoundary() noexcept;

        /// <summary>False after a symbol without a Huffman code or when the output ran out of memory.</summary>
        bool Healthy() const noexcept { return healthy_; }

    private:
        void DischargeBitBuffer() noexcept;
        void EmitByte(std::uint8_t byte) noexcept;

        ByteBuffer&   output_;
        std::uint64_t putBuffer_ = 0;
        std::int32_t  freeBits_  = 64;
        bool          healthy_   = true;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
