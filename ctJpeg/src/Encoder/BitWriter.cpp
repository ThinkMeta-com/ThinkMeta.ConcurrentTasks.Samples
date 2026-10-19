// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#include "BitWriter.h"

#include <cstddef>
#include <cstdint>

#include "BitPacking.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    void BitWriter::EmitByte(std::uint8_t byte) noexcept
    {
        if (!output_.Reserve(2)) {
            healthy_ = false;
            return;
        }
        std::uint8_t* data = output_.Data();
        std::size_t   pos  = output_.Size();
        data[pos++]        = byte;
        if (byte == 0xFF)
            data[pos++] = 0;
        output_.SetSize(pos);
    }

    void BitWriter::DischargeBitBuffer() noexcept
    {
        // Every 0xFF byte in the entropy-coded segment is followed by a zero byte; check the
        // whole buffer once and take the byte-wise path only if it holds a 0xFF.
        if (HasZeroByte(~putBuffer_)) {
            for (std::int32_t shift = 56; shift >= 0; shift -= 8)
                EmitByte(static_cast<std::uint8_t>((putBuffer_ >> shift) & 0xFF));
            return;
        }
        if (!output_.Reserve(8)) {
            healthy_ = false;
            return;
        }
        StoreBigEndian64(putBuffer_, output_.Data() + output_.Size());
        output_.SetSize(output_.Size() + 8);
    }

    void BitWriter::JumpToByteBoundary() noexcept
    {
        const std::int32_t nBits = freeBits_ & 7;
        if (nBits > 0)
            WriteBits(nBits, (1u << nBits) - 1);
        putBuffer_ <<= freeBits_;
        while (freeBits_ <= 56) {
            EmitByte(static_cast<std::uint8_t>((putBuffer_ >> 56) & 0xFF));
            putBuffer_ <<= 8;
            freeBits_ += 8;
        }
        putBuffer_ = 0;
        freeBits_  = 64;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
