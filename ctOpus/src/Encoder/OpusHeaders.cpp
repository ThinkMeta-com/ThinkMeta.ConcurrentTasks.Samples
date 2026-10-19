// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "OpusHeaders.h"

#include <cstdint>
#include <cstring>

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    namespace {

        // libopusenc: write_uint32() (src/opus_header.c)
        void WriteLe32(std::uint8_t* p, std::uint32_t value) noexcept
        {
            p[0] = static_cast<std::uint8_t>(value & 0xff);
            p[1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
            p[2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
            p[3] = static_cast<std::uint8_t>((value >> 24) & 0xff);
        }

        std::uint32_t ReadLe32(const std::uint8_t* p) noexcept
        {
            return (static_cast<std::uint32_t>(p[3]) << 24) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) | p[0];
        }

        // The Packet writer of opus_header.c: every write checks the remaining space.
        struct PacketWriter {
            std::uint8_t* data;
            std::int32_t  maxLength;
            std::int32_t  pos;

            bool Bytes(const std::uint8_t* bytes, std::int32_t count) noexcept
            {
                if (pos > maxLength - count)
                    return false;
                for (std::int32_t i = 0; i < count; ++i)
                    data[pos++] = bytes[i];
                return true;
            }

            bool Byte(std::uint8_t value) noexcept { return Bytes(&value, 1); }

            bool U16(std::uint32_t value) noexcept
            {
                if (pos > maxLength - 2)
                    return false;
                data[pos]     = static_cast<std::uint8_t>(value & 0xff);
                data[pos + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
                pos += 2;
                return true;
            }

            bool U32(std::uint32_t value) noexcept
            {
                if (pos > maxLength - 4)
                    return false;
                WriteLe32(data + pos, value);
                pos += 4;
                return true;
            }
        };

    } // namespace

    // libopusenc: opeint_opus_header_to_packet() (src/opus_header.c)
    std::int32_t OpusHead::ToPacket(std::uint8_t* packet, std::int32_t length) const noexcept
    {
        PacketWriter p{packet, length, 0};
        if (length < 19)
            return 0;
        if (!p.Bytes(reinterpret_cast<const std::uint8_t*>("OpusHead"), 8) || !p.Byte(1) // version
            || !p.Byte(static_cast<std::uint8_t>(channels)) || !p.U16(static_cast<std::uint16_t>(preskip)) || !p.U32(inputSampleRate)
            || !p.U16(static_cast<std::uint16_t>(gain)) || !p.Byte(static_cast<std::uint8_t>(channelMapping)))
            return 0;
        if (channelMapping != 0) {
            if (!p.Byte(static_cast<std::uint8_t>(nbStreams)) || !p.Byte(static_cast<std::uint8_t>(nbCoupled)))
                return 0;
            for (std::int32_t i = 0; i < channels; ++i)
                if (!p.Byte(streamMap[i]))
                    return 0;
        }
        return p.pos;
    }

    // libopusenc: opeint_comment_init() (src/opus_header.c)
    bool OpusTags::Initialize(const char* vendor) noexcept
    {
        const auto         vendorLength = static_cast<std::int32_t>(std::strlen(vendor));
        const std::int32_t length       = 8 + 4 + vendorLength + 4;
        if (!data_.Allocate(static_cast<std::size_t>(length))) {
            length_ = 0;
            return false;
        }
        std::memcpy(data_.Data(), "OpusTags", 8);
        WriteLe32(data_.Data() + 8, static_cast<std::uint32_t>(vendorLength));
        std::memcpy(data_.Data() + 12, vendor, static_cast<std::size_t>(vendorLength));
        WriteLe32(data_.Data() + 12 + vendorLength, 0);
        length_ = length;
        return true;
    }

    // libopusenc: ope_comments_copy() (src/opusenc.c)
    bool OpusTags::CopyFrom(const OpusTags& other) noexcept
    {
        if (!data_.Allocate(static_cast<std::size_t>(other.length_)))
            return false;
        std::memcpy(data_.Data(), other.data_.Data(), static_cast<std::size_t>(other.length_));
        length_ = other.length_;
        return true;
    }

    // libopusenc: ope_comments_add() (src/opusenc.c)
    bool OpusTags::Add(const char* tag, const char* value) noexcept
    {
        if (tag == nullptr || value == nullptr || std::strchr(tag, '=') != nullptr)
            return false;
        return AddEntry(tag, value);
    }

    // libopusenc: ope_comments_add_string() (src/opusenc.c)
    bool OpusTags::AddString(const char* tagAndValue) noexcept
    {
        if (std::strchr(tagAndValue, '=') == nullptr)
            return false;
        return AddEntry(nullptr, tagAndValue);
    }

    // libopusenc: opeint_comment_add() (src/opus_header.c)
    bool OpusTags::AddEntry(const char* tag, const char* value) noexcept
    {
        std::uint8_t*       p            = data_.Data();
        const std::uint32_t vendorLength = ReadLe32(p + 8);
        const std::uint32_t count        = ReadLe32(p + 8 + 4 + vendorLength);
        const std::int32_t  tagLength    = tag != nullptr ? static_cast<std::int32_t>(std::strlen(tag)) + 1 : 0;
        const auto          valueLength  = static_cast<std::int32_t>(std::strlen(value));
        const std::int32_t  length       = length_ + 4 + tagLength + valueLength;
        if (!data_.Grow(static_cast<std::size_t>(length)))
            return false;
        p = data_.Data();
        WriteLe32(p + length_, static_cast<std::uint32_t>(tagLength + valueLength));
        if (tag != nullptr) {
            std::memcpy(p + length_ + 4, tag, static_cast<std::size_t>(tagLength));
            p[length_ + 4 + tagLength - 1] = '=';
        }
        std::memcpy(p + length_ + 4 + tagLength, value, static_cast<std::size_t>(valueLength));
        WriteLe32(p + 8 + 4 + vendorLength, count + 1);
        length_ = length;
        return true;
    }

    // libopusenc: opeint_comment_pad() (src/opus_header.c)
    bool OpusTags::Pad(std::int32_t amount) noexcept
    {
        if (amount <= 0)
            return true;
        const std::int32_t newLength = (length_ + amount + 255) / 255 * 255 - 1;
        if (!data_.Grow(static_cast<std::size_t>(newLength)))
            return false;
        std::memset(data_.Data() + length_, 0, static_cast<std::size_t>(newLength - length_));
        length_ = newLength;
        return true;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
