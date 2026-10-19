// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "RangeEncoder.h"

#include <cstddef>
#include <cstring>

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    namespace {

        // mfrngcod.h
        constexpr std::uint32_t SymBits    = 8;
        constexpr std::uint32_t CodeBits   = 32;
        constexpr std::uint32_t SymMax     = (1U << SymBits) - 1;
        constexpr std::uint32_t CodeShift  = CodeBits - SymBits - 1;
        constexpr std::uint32_t CodeTop    = 1U << (CodeBits - 1);
        constexpr std::uint32_t CodeBot    = CodeTop >> SymBits;
        constexpr std::uint32_t UintBits   = 8;
        constexpr std::int32_t  WindowSize = 32;

    } // namespace

    // libopus: ec_write_byte() (celt/entenc.c)
    std::int32_t RangeEncoder::WriteByte(std::uint32_t value) noexcept
    {
        if (offs_ + endOffs_ >= storage_)
            return -1;
        buf_[offs_++] = static_cast<std::uint8_t>(value);
        return 0;
    }

    // libopus: ec_write_byte_at_end() (celt/entenc.c)
    std::int32_t RangeEncoder::WriteByteAtEnd(std::uint32_t value) noexcept
    {
        if (offs_ + endOffs_ >= storage_)
            return -1;
        buf_[storage_ - ++endOffs_] = static_cast<std::uint8_t>(value);
        return 0;
    }

    // libopus: ec_enc_carry_out() (celt/entenc.c)
    // A symbol with a carry bit. Symbols that may propagate a carry are buffered until it is
    // known whether a carry occurs.
    void RangeEncoder::CarryOut(std::int32_t c) noexcept
    {
        if (static_cast<std::uint32_t>(c) != SymMax) {
            // no further carry propagation possible: flush the buffer
            const std::int32_t carry = c >> SymBits;
            // no byte on the first write
            if (rem_ >= 0)
                error_ |= WriteByte(static_cast<std::uint32_t>(rem_ + carry));
            if (ext_ > 0) {
                const std::uint32_t sym = (SymMax + static_cast<std::uint32_t>(carry)) & SymMax;
                do
                    error_ |= WriteByte(sym);
                while (--ext_ > 0);
            }
            rem_ = static_cast<std::int32_t>(static_cast<std::uint32_t>(c) & SymMax);
        } else {
            ++ext_;
        }
    }

    // libopus: ec_enc_normalize() (celt/entenc.c)
    void RangeEncoder::Normalize() noexcept
    {
        // a range too small: output some bits and rescale it
        while (rng_ <= CodeBot) {
            CarryOut(static_cast<std::int32_t>(val_ >> CodeShift));
            // the next-to-high-order symbol into the high-order position
            val_ = (val_ << SymBits) & (CodeTop - 1);
            rng_ <<= SymBits;
            nbitsTotal_ += static_cast<std::int32_t>(SymBits);
        }
    }

    // libopus: ec_enc_init() (celt/entenc.c)
    void RangeEncoder::Init(std::uint8_t* buffer, std::uint32_t size) noexcept
    {
        buf_       = buffer;
        endOffs_   = 0;
        endWindow_ = 0;
        nendBits_  = 0;
        // the offset from which ec_tell() subtracts partial bits
        nbitsTotal_ = static_cast<std::int32_t>(CodeBits) + 1;
        offs_       = 0;
        rng_        = CodeTop;
        rem_        = -1;
        val_        = 0;
        ext_        = 0;
        storage_    = size;
        error_      = 0;
    }

    void SymbolLog::AppendFrom(const SymbolLog& other, std::size_t from, std::size_t to) noexcept
    {
        for (std::size_t i = from; i < to; ++i) {
            if (count_ < capacity_ && i < other.capacity_)
                ops_[count_] = other.ops_[i];
            ++count_;
        }
        if (other.Overflowed())
            count_ = capacity_ + 1;
    }

    void SymbolLog::Erase(std::size_t from, std::size_t to) noexcept
    {
        if (Overflowed())
            return;
        std::memmove(ops_ + from, ops_ + to, (count_ - to) * sizeof(Op));
        count_ -= to - from;
    }

    void SymbolLog::ReplayInto(RangeEncoder& enc, std::size_t from, std::size_t to) const noexcept
    {
        SymbolLog* const saved = enc.Log();
        enc.SetLog(nullptr);
        for (std::size_t i = from; i < to; ++i) {
            const Op& op = ops_[i];
            switch (op.kind) {
                case Kind::Encode: enc.Encode(op.a, op.b, op.c); break;
                case Kind::EncodeBin: enc.EncodeBin(op.a, op.b, op.c); break;
                case Kind::BitLogp: enc.EncodeBitLogp(static_cast<std::int32_t>(op.a), op.b); break;
                case Kind::Icdf: enc.EncodeIcdf(static_cast<std::int32_t>(op.a), op.icdf, op.b); break;
                case Kind::Uint: enc.EncodeUint(op.a, op.b); break;
                case Kind::Bits: enc.EncodeBits(op.a, op.b); break;
            }
        }
        enc.SetLog(saved);
    }

    // libopus: ec_encode() (celt/entenc.c)
    void RangeEncoder::Encode(std::uint32_t fl, std::uint32_t fh, std::uint32_t ft) noexcept
    {
        if (log_ != nullptr)
            log_->Append(SymbolLog::Kind::Encode, fl, fh, ft);
        const std::uint32_t r = rng_ / ft;
        if (fl > 0) {
            val_ += rng_ - r * (ft - fl);
            rng_ = r * (fh - fl);
        } else {
            rng_ -= r * (ft - fh);
        }
        Normalize();
    }

    // libopus: ec_encode_bin() (celt/entenc.c)
    void RangeEncoder::EncodeBin(std::uint32_t fl, std::uint32_t fh, std::uint32_t bits) noexcept
    {
        if (log_ != nullptr)
            log_->Append(SymbolLog::Kind::EncodeBin, fl, fh, bits);
        const std::uint32_t r = rng_ >> bits;
        if (fl > 0) {
            val_ += rng_ - r * ((1U << bits) - fl);
            rng_ = r * (fh - fl);
        } else {
            rng_ -= r * ((1U << bits) - fh);
        }
        Normalize();
    }

    // libopus: ec_enc_bit_logp() (celt/entenc.c)
    void RangeEncoder::EncodeBitLogp(std::int32_t val, std::uint32_t logp) noexcept
    {
        if (log_ != nullptr)
            log_->Append(SymbolLog::Kind::BitLogp, static_cast<std::uint32_t>(val), logp, 0);
        std::uint32_t       r = rng_;
        const std::uint32_t l = val_;
        const std::uint32_t s = r >> logp;
        r -= s;
        if (val != 0)
            val_ = l + r;
        rng_ = val != 0 ? s : r;
        Normalize();
    }

    // libopus: ec_enc_icdf() (celt/entenc.c)
    void RangeEncoder::EncodeIcdf(std::int32_t s, const std::uint8_t* icdf, std::uint32_t ftb) noexcept
    {
        if (log_ != nullptr)
            log_->Append(SymbolLog::Kind::Icdf, static_cast<std::uint32_t>(s), ftb, 0, icdf);
        const std::uint32_t r = rng_ >> ftb;
        if (s > 0) {
            val_ += rng_ - r * icdf[s - 1];
            rng_ = r * static_cast<std::uint32_t>(icdf[s - 1] - icdf[s]);
        } else {
            rng_ -= r * icdf[s];
        }
        Normalize();
    }

    // libopus: ec_enc_uint() (celt/entenc.c)
    void RangeEncoder::EncodeUint(std::uint32_t fl, std::uint32_t ft) noexcept
    {
        // one operation in the log, not its parts
        SymbolLog* const log = log_;
        if (log != nullptr) {
            log->Append(SymbolLog::Kind::Uint, fl, ft, 0);
            log_ = nullptr;
        }
        EncodeUintParts(fl, ft);
        log_ = log;
    }

    void RangeEncoder::EncodeUintParts(std::uint32_t fl, std::uint32_t ft) noexcept
    {
        // EC_ILOG is undefined for 0 in libopus: ft > 1
        --ft;
        std::int32_t ftb = EcIlog(ft);
        if (ftb > static_cast<std::int32_t>(UintBits)) {
            ftb -= static_cast<std::int32_t>(UintBits);
            const std::uint32_t ft1 = (ft >> ftb) + 1;
            const std::uint32_t fl1 = fl >> ftb;
            Encode(fl1, fl1 + 1, ft1);
            EncodeBits(fl & ((1U << ftb) - 1U), static_cast<std::uint32_t>(ftb));
        } else {
            Encode(fl, fl + 1, ft + 1);
        }
    }

    // libopus: ec_enc_bits() (celt/entenc.c)
    void RangeEncoder::EncodeBits(std::uint32_t fl, std::uint32_t bits) noexcept
    {
        if (log_ != nullptr)
            log_->Append(SymbolLog::Kind::Bits, fl, bits, 0);
        std::uint32_t window = endWindow_;
        std::int32_t  used   = nendBits_;
        if (used + static_cast<std::int32_t>(bits) > WindowSize) {
            do {
                error_ |= WriteByteAtEnd(window & SymMax);
                window >>= SymBits;
                used -= static_cast<std::int32_t>(SymBits);
            } while (used >= static_cast<std::int32_t>(SymBits));
        }
        window |= fl << used;
        used += static_cast<std::int32_t>(bits);
        endWindow_ = window;
        nendBits_  = used;
        nbitsTotal_ += static_cast<std::int32_t>(bits);
    }

    // libopus: ec_enc_patch_initial_bits() (celt/entenc.c)
    void RangeEncoder::PatchInitialBits(std::uint32_t val, std::uint32_t nbits) noexcept
    {
        const std::uint32_t shift = SymBits - nbits;
        const std::uint32_t mask  = ((1U << nbits) - 1) << shift;
        if (offs_ > 0) {
            // the first byte has been finalised
            buf_[0] = static_cast<std::uint8_t>((buf_[0] & ~mask) | val << shift);
        } else if (rem_ >= 0) {
            // the first byte still awaits carry propagation
            rem_ = static_cast<std::int32_t>((static_cast<std::uint32_t>(rem_) & ~mask) | val << shift);
        } else if (rng_ <= (CodeTop >> nbits)) {
            // the renormalisation loop has never run
            val_ = (val_ & ~(mask << CodeShift)) | val << (CodeShift + shift);
        } else {
            // not even nbits of data encoded yet
            error_ = -1;
        }
    }

    // libopus: ec_enc_shrink() (celt/entenc.c)
    void RangeEncoder::Shrink(std::uint32_t size) noexcept
    {
        std::memmove(buf_ + size - endOffs_, buf_ + storage_ - endOffs_, endOffs_);
        storage_ = size;
    }

    // libopus: ec_enc_done() (celt/entenc.c)
    void RangeEncoder::Done() noexcept
    {
        // the minimum number of bits that decodes the symbols so far correctly, whatever follows
        std::int32_t  l   = static_cast<std::int32_t>(CodeBits) - EcIlog(rng_);
        std::uint32_t msk = (CodeTop - 1) >> l;
        std::uint32_t end = (val_ + msk) & ~msk;
        if ((end | msk) >= val_ + rng_) {
            ++l;
            msk >>= 1;
            end = (val_ + msk) & ~msk;
        }
        while (l > 0) {
            CarryOut(static_cast<std::int32_t>(end >> CodeShift));
            end = (end << SymBits) & (CodeTop - 1);
            l -= static_cast<std::int32_t>(SymBits);
        }
        // a buffered byte goes into the output buffer
        if (rem_ >= 0 || ext_ > 0)
            CarryOut(0);
        // buffered extra bits as well
        std::uint32_t window = endWindow_;
        std::int32_t  used   = nendBits_;
        while (used >= static_cast<std::int32_t>(SymBits)) {
            error_ |= WriteByteAtEnd(window & SymMax);
            window >>= SymBits;
            used -= static_cast<std::int32_t>(SymBits);
        }
        // clear the excess space and add the remaining extra bits to the last byte
        if (error_ == 0) {
            if (buf_ != nullptr)
                std::memset(buf_ + offs_, 0, storage_ - offs_ - endOffs_);
            if (used > 0) {
                // no range coder data at all: give up
                if (endOffs_ >= storage_) {
                    error_ = -1;
                } else {
                    l = -l;
                    // busted: not too many extra bits in the last byte, the range coder data matters more
                    if (offs_ + endOffs_ >= storage_ && l < used) {
                        window &= (1U << l) - 1;
                        error_ = -1;
                    }
                    buf_[storage_ - endOffs_ - 1] |= static_cast<std::uint8_t>(window);
                }
            }
        }
    }

    // libopus: ec_tell_frac() (celt/entcode.c)
    std::uint32_t RangeEncoder::TellFrac() const noexcept
    {
        static constexpr std::uint32_t correction[8] = {35733, 38967, 42495, 46340, 50535, 55109, 60097, 65535};
        const std::uint32_t            nbits         = static_cast<std::uint32_t>(nbitsTotal_) << BitRes;
        std::int32_t                   l             = EcIlog(rng_);
        const std::uint32_t            r             = rng_ >> (l - 16);
        std::uint32_t                  b             = (r >> 12) - 8;
        b += r > correction[b] ? 1 : 0;
        l = (l << 3) + static_cast<std::int32_t>(b);
        return nbits - static_cast<std::uint32_t>(l);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
