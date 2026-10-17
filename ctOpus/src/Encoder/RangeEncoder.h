// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    constexpr std::int32_t BitRes = 3; // 1/8 bit resolution of ec_tell_frac

    /// <summary>EC_ILOG: the number of significant bits of x (0 for 0).</summary>
    inline std::int32_t EcIlog(std::uint32_t x) noexcept
    {
        return static_cast<std::int32_t>(std::bit_width(x));
    }

    class RangeEncoder;

    /// <summary>
    /// The coding operations of a range encoder in order (RangeEncoder::SetLog), to be applied
    /// to another encoder later (ReplayInto): the symbols of a frame part computed elsewhere.
    /// The storage is the caller's.
    /// </summary>
    class SymbolLog
    {
    public:
        enum class Kind : std::uint8_t { Encode, EncodeBin, BitLogp, Icdf, Uint, Bits };

        struct Op {
            Kind                kind;
            std::uint32_t       a;
            std::uint32_t       b;
            std::uint32_t       c;
            const std::uint8_t* icdf;
        };

        void Attach(Op* storage, std::size_t capacity) noexcept
        {
            ops_      = storage;
            capacity_ = capacity;
            count_    = 0;
        }

        void        Clear() noexcept { count_ = 0; }
        std::size_t Size() const noexcept { return count_; }
        /// <summary>More operations than the storage holds (the log is unusable then).</summary>
        bool Overflowed() const noexcept { return count_ > capacity_; }

        void Append(Kind kind, std::uint32_t a, std::uint32_t b, std::uint32_t c, const std::uint8_t* icdf = nullptr) noexcept
        {
            if (count_ < capacity_)
                ops_[count_] = Op{kind, a, b, c, icdf};
            ++count_;
        }
        /// <summary>The operations from..to of another log appended.</summary>
        void AppendFrom(const SymbolLog& other, std::size_t from, std::size_t to) noexcept;
        /// <summary>Only the first size operations kept.</summary>
        void Truncate(std::size_t size) noexcept { count_ = size; }
        /// <summary>The operations from..to removed (the later ones move up).</summary>
        void Erase(std::size_t from, std::size_t to) noexcept;

        /// <summary>The operations from..to applied to enc (which does not log them).</summary>
        void ReplayInto(RangeEncoder& enc, std::size_t from, std::size_t to) const noexcept;

    private:
        Op*         ops_      = nullptr;
        std::size_t capacity_ = 0;
        std::size_t count_    = 0;
    };

    /// <summary>
    /// Port of the range encoder of libopus 1.6.1 (celt/entenc.c, celt/entcode.c), shared by the
    /// SILK and CELT encoders of a frame.
    /// </summary>
    class RangeEncoder
    {
    public:
        /// <summary>ec_enc_init.</summary>
        void Init(std::uint8_t* buffer, std::uint32_t size) noexcept;

        /// <summary>ec_encode: a symbol [fl, fh) out of ft.</summary>
        void Encode(std::uint32_t fl, std::uint32_t fh, std::uint32_t ft) noexcept;
        /// <summary>ec_encode_bin: as Encode with ft = 1 &lt;&lt; bits.</summary>
        void EncodeBin(std::uint32_t fl, std::uint32_t fh, std::uint32_t bits) noexcept;
        /// <summary>ec_enc_bit_logp: a bit that is one with probability 1/(1 &lt;&lt; logp).</summary>
        void EncodeBitLogp(std::int32_t val, std::uint32_t logp) noexcept;
        /// <summary>ec_enc_icdf: symbol s of an inverse CDF table with 2^ftb total.</summary>
        void EncodeIcdf(std::int32_t s, const std::uint8_t* icdf, std::uint32_t ftb) noexcept;
        /// <summary>ec_enc_uint: fl uniformly out of [0, ft).</summary>
        void EncodeUint(std::uint32_t fl, std::uint32_t ft) noexcept;
        /// <summary>ec_enc_bits: raw bits at the end of the buffer.</summary>
        void EncodeBits(std::uint32_t fl, std::uint32_t bits) noexcept;
        /// <summary>ec_enc_patch_initial_bits: overwrites the first nbits (at most 8) of the stream.</summary>
        void PatchInitialBits(std::uint32_t val, std::uint32_t nbits) noexcept;
        /// <summary>ec_enc_shrink: moves the raw bits to a smaller buffer end.</summary>
        void Shrink(std::uint32_t size) noexcept;
        /// <summary>ec_enc_done: flushes everything.</summary>
        void Done() noexcept;

        /// <summary>ec_tell: the bits used so far, rounded up.</summary>
        std::int32_t Tell() const noexcept { return nbitsTotal_ - EcIlog(rng_); }
        /// <summary>ec_tell_frac: the bits used so far in 1/8 bits.</summary>
        std::uint32_t TellFrac() const noexcept;

        std::uint32_t Storage() const noexcept { return storage_; }
        std::uint32_t Range() const noexcept { return rng_; }
        std::int32_t  Error() const noexcept { return error_; }
        std::uint8_t* Buffer() const noexcept { return buf_; }
        /// <summary>ec_range_bytes: the range coder bytes written so far.</summary>
        std::uint32_t RangeBytes() const noexcept { return offs_; }
        /// <summary>The raw bytes written at the end of the buffer so far.</summary>
        std::uint32_t EndBytes() const noexcept { return endOffs_; }
        /// <summary>Another buffer of the same size (a copy of the coder writing elsewhere; the coder never reads the buffer).</summary>
        void Rebase(std::uint8_t* buffer) noexcept { buf_ = buffer; }
        /// <summary>Logs every coding operation into log from now on (nullptr: no more); a copy of the encoder logs into the same log.</summary>
        void       SetLog(SymbolLog* log) noexcept { log_ = log; }
        SymbolLog* Log() const noexcept { return log_; }

        /// <summary>nbits_total += bits (CELT pretends to have filled silent frames).</summary>
        void AddTotalBits(std::int32_t bits) noexcept { nbitsTotal_ += bits; }

    private:
        void         EncodeUintParts(std::uint32_t fl, std::uint32_t ft) noexcept;
        std::int32_t WriteByte(std::uint32_t value) noexcept;
        std::int32_t WriteByteAtEnd(std::uint32_t value) noexcept;
        void         CarryOut(std::int32_t c) noexcept;
        void         Normalize() noexcept;

        // the fields of ec_ctx, in its order
        std::uint8_t* buf_        = nullptr;
        std::uint32_t storage_    = 0;
        std::uint32_t endOffs_    = 0;
        std::uint32_t endWindow_  = 0;
        std::int32_t  nendBits_   = 0;
        std::int32_t  nbitsTotal_ = 0;
        std::uint32_t offs_       = 0;
        std::uint32_t rng_        = 0;
        std::uint32_t val_        = 0;
        std::uint32_t ext_        = 0;
        std::int32_t  rem_        = 0;
        std::int32_t  error_      = 0;
        SymbolLog*    log_        = nullptr;

        friend struct RangeEncoderLayout;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
