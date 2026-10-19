//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#pragma once

#include <cstdint>
#include <cstring>

namespace ThinkMeta::ConcurrentTasks::Samples::Deflate {

    /// <summary>
    /// libdeflate: struct deflate_output_bitstream (deflate_compress.c). DEFLATE output bitstream (LSB first) over a
    /// caller-owned buffer. Bits collect in a 64-bit buffer; Flush() writes the whole bytes and keeps at most 7 bits.
    /// The buffer must hold the output plus 8 bytes of slack for whole-word stores; there is no overflow check.
    /// </summary>
    class BitWriter
    {
    public:
        BitWriter(std::uint8_t* out, std::uint8_t* end) noexcept : next_(out), end_(end) {}

        /// <summary>libdeflate: ADD_BITS() (deflate_compress.c). Adds n bits (n &lt;= 56 - BitCount()).</summary>
        void AddBits(std::uint64_t bits, std::uint32_t n) noexcept
        {
            bitbuf_ |= bits << bitcount_;
            bitcount_ += n;
        }

        // libdeflate: FLUSH_BITS() (deflate_compress.c)
        void Flush() noexcept
        {
            if (next_ + sizeof(std::uint64_t) <= end_) {
                std::memcpy(next_, &bitbuf_, sizeof(bitbuf_));
                next_ += bitcount_ >> 3;
                bitbuf_ = (bitcount_ & ~7u) == 64 ? 0 : bitbuf_ >> (bitcount_ & ~7u);
                bitcount_ &= 7;
            } else {
                while (bitcount_ >= 8) {
                    *next_++ = static_cast<std::uint8_t>(bitbuf_);
                    bitcount_ -= 8;
                    bitbuf_ >>= 8;
                }
            }
        }

        /// <summary>libdeflate: the final byte of libdeflate_deflate_compress() and the alignment in deflate_flush_block()
        /// (deflate_compress.c). Pads the pending bits (after Flush) with zeros to a byte boundary.</summary>
        void AlignToByte() noexcept
        {
            if (bitcount_ != 0) {
                *next_++  = static_cast<std::uint8_t>(bitbuf_);
                bitbuf_   = 0;
                bitcount_ = 0;
            }
        }

        /// <summary>Pending bits not yet written (0..7 after Flush).</summary>
        std::uint32_t BitCount() const noexcept { return bitcount_; }
        std::uint8_t* Next() const noexcept { return next_; }
        void          SetNext(std::uint8_t* next) noexcept { next_ = next; }

    private:
        std::uint8_t* next_;
        std::uint8_t* end_;
        std::uint64_t bitbuf_   = 0;
        std::uint32_t bitcount_ = 0;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Deflate
