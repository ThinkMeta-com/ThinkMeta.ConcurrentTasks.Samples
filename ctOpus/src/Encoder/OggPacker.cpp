// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "OggPacker.h"

#include <cstdint>
#include <cstring>

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    namespace {

        constexpr std::size_t MaxHeaderSize = 27 + 255;
        constexpr std::size_t MaxPageSize   = 255 * 255 + MaxHeaderSize;

        // Ogg CRC-32: polynomial 0x04c11db7, not reflected, initial value 0.
        struct CrcTable {
            std::uint32_t values[256];
            constexpr CrcTable() : values()
            {
                for (std::uint32_t i = 0; i < 256; ++i) {
                    std::uint32_t r = i << 24;
                    for (std::int32_t k = 0; k < 8; ++k)
                        r = (r & 0x80000000u) != 0 ? (r << 1) ^ 0x04c11db7u : r << 1;
                    values[i] = r;
                }
            }
        };
        constexpr CrcTable Crc;

        // libopusenc: ogg_page_checksum_set() (src/ogg_packer.c)
        void SetChecksum(std::uint8_t* page, std::size_t length) noexcept
        {
            page[22] = page[23] = page[24] = page[25] = 0;
            std::uint32_t crc                         = 0;
            for (std::size_t i = 0; i < length; ++i)
                crc = (crc << 8) ^ Crc.values[((crc >> 24) & 0xff) ^ page[i]];
            page[22] = static_cast<std::uint8_t>(crc & 0xff);
            page[23] = static_cast<std::uint8_t>((crc >> 8) & 0xff);
            page[24] = static_cast<std::uint8_t>((crc >> 16) & 0xff);
            page[25] = static_cast<std::uint8_t>((crc >> 24) & 0xff);
        }

    } // namespace

    // libopusenc: oggp_create() (src/ogg_packer.c)
    bool OggPacker::Initialize(std::int32_t serialNumber) noexcept
    {
        bufSize_ = MaxPageSize;
        if (!allocBuffer_.Allocate(bufSize_ + MaxHeaderSize) || !lacing_.Allocate(256) || !pages_.Allocate(10))
            return false;
        buf_          = allocBuffer_.Data() + MaxHeaderSize;
        userBuf_      = nullptr;
        serialNumber_ = serialNumber;
        bufFill_ = bufBegin_ = 0;
        lacingFill_ = lacingBegin_ = 0;
        pagesFill_                 = 0;
        isEos_                     = false;
        currGranule_ = lastGranule_ = 0;
        pageNumber_                 = 0;
        muxingDelay_                = 0;
        return true;
    }

    // libopusenc: shift_buffer() (src/ogg_packer.c)
    void OggPacker::ShiftBuffer() noexcept
    {
        const std::size_t bufShift    = pagesFill_ != 0 ? pages_[0].bufPos : bufBegin_;
        const std::size_t lacingShift = pagesFill_ != 0 ? pages_[0].lacingPos : lacingBegin_;
        if (4 * lacingShift > lacingFill_) {
            std::memmove(lacing_.Data(), lacing_.Data() + lacingShift, lacingFill_ - lacingShift);
            for (std::size_t i = 0; i < pagesFill_; ++i)
                pages_[i].lacingPos -= lacingShift;
            lacingFill_ -= lacingShift;
            lacingBegin_ -= lacingShift;
        }
        if (4 * bufShift > bufFill_) {
            std::memmove(buf_, buf_ + bufShift, bufFill_ - bufShift);
            for (std::size_t i = 0; i < pagesFill_; ++i)
                pages_[i].bufPos -= bufShift;
            bufFill_ -= bufShift;
            bufBegin_ -= bufShift;
        }
    }

    // libopusenc: oggp_get_packet_buffer() (src/ogg_packer.c)
    std::uint8_t* OggPacker::GetPacketBuffer(std::int32_t bytes) noexcept
    {
        if (bufFill_ + bytes > bufSize_) {
            ShiftBuffer();
            // still not enough room: grow (newsize covers MaxHeaderSize as in the original)
            if (bufFill_ + bytes > bufSize_) {
                std::size_t newSize = bufFill_ + bytes + MaxHeaderSize;
                newSize             = newSize * 3 / 2;
                if (!allocBuffer_.Grow(newSize))
                    return nullptr;
                bufSize_ = newSize;
                buf_     = allocBuffer_.Data() + MaxHeaderSize;
            }
        }
        userBuf_ = buf_ + bufFill_;
        return userBuf_;
    }

    // libopusenc: oggp_commit_packet() (src/ogg_packer.c)
    bool OggPacker::CommitPacket(std::int32_t bytes, std::uint64_t granulePosition, bool endOfStream) noexcept
    {
        const std::size_t nb255s = static_cast<std::size_t>(bytes) / 255;
        if (lacingFill_ - lacingBegin_ + nb255s + 1 > 255 || (muxingDelay_ != 0 && granulePosition - lastGranule_ > muxingDelay_))
            FlushPage();
        bufFill_ += static_cast<std::size_t>(bytes);
        if (lacingFill_ + nb255s + 1 > lacing_.Size()) {
            ShiftBuffer();
            if (lacingFill_ + nb255s + 1 > lacing_.Size()) {
                std::size_t newSize = lacingFill_ + nb255s + 1;
                newSize             = newSize * 3 / 2;
                if (!lacing_.Grow(newSize))
                    return false;
            }
        }
        // the original's handling of a packet buffer that ShiftBuffer moved
        if (userBuf_ > buf_ + bufFill_)
            std::memmove(buf_ + bufFill_, userBuf_, static_cast<std::size_t>(bytes));
        for (std::size_t i = 0; i < nb255s; ++i)
            lacing_[lacingFill_ + i] = 255;
        lacing_[lacingFill_ + nb255s] = static_cast<std::uint8_t>(bytes - 255 * static_cast<std::int32_t>(nb255s));
        lacingFill_ += nb255s + 1;
        currGranule_ = granulePosition;
        isEos_       = endOfStream;
        if (muxingDelay_ != 0 && granulePosition - lastGranule_ >= muxingDelay_)
            FlushPage();
        return true;
    }

    // libopusenc: oggp_flush_page() (src/ogg_packer.c)
    bool OggPacker::FlushPage() noexcept
    {
        if (lacingFill_ == lacingBegin_)
            return false;
        std::size_t  nbLacing = lacingFill_ - lacingBegin_;
        std::int32_t cont     = 0;
        do {
            if (pagesFill_ >= pages_.Size()) {
                const std::size_t newSize = 1 + pages_.Size() * 3 / 2;
                if (!pages_.Grow(newSize))
                    return false;
            }
            Page& p           = pages_[pagesFill_++];
            p.granulePosition = currGranule_;
            p.lacingPos       = lacingBegin_;
            p.lacingSize      = nbLacing;
            p.flags           = cont;
            p.bufPos          = bufBegin_;
            if (p.lacingSize > 255) {
                // as in ogg_packer.c, which sums lacing[lacing_begin + 1] 255 times
                std::size_t bytes = 0;
                for (std::int32_t i = 0; i < 255; ++i)
                    bytes += lacing_[lacingBegin_ + 1];
                p.bufSize         = bytes;
                p.lacingSize      = 255;
                p.granulePosition = static_cast<std::uint64_t>(-1);
                cont              = 1;
            } else {
                p.bufSize = bufFill_ - bufBegin_;
                if (isEos_)
                    p.flags |= 0x04;
            }
            nbLacing -= p.lacingSize;
            lacingBegin_ += p.lacingSize;
            bufBegin_ += p.bufSize;
            p.pageNumber = pageNumber_++;
            if (p.pageNumber == 0)
                p.flags |= 0x02;
        } while (nbLacing > 0);
        lastGranule_ = currGranule_;
        return true;
    }

    // libopusenc: oggp_get_next_page() (src/ogg_packer.c)
    bool OggPacker::GetNextPage(const std::uint8_t*& page, std::int32_t& bytes) noexcept
    {
        if (pagesFill_ == 0) {
            page  = nullptr;
            bytes = 0;
            return false;
        }
        const Page&       p          = pages_[0];
        const std::size_t headerSize = 27 + p.lacingSize;
        std::uint8_t*     ptr        = buf_ + p.bufPos - headerSize;
        const std::size_t length     = p.bufSize + headerSize;
        std::memcpy(ptr + 27, lacing_.Data() + p.lacingPos, p.lacingSize);
        std::memcpy(ptr, "OggS", 4);
        ptr[4]                = 0x00; // stream structure version
        ptr[5]                = static_cast<std::uint8_t>(p.flags);
        std::uint64_t granule = p.granulePosition;
        for (std::int32_t i = 6; i < 14; ++i) {
            ptr[i] = static_cast<std::uint8_t>(granule & 0xff);
            granule >>= 8;
        }
        auto serial = static_cast<std::uint32_t>(serialNumber_);
        for (std::int32_t i = 14; i < 18; ++i) {
            ptr[i] = static_cast<std::uint8_t>(serial & 0xff);
            serial >>= 8;
        }
        auto pageNumber = static_cast<std::uint32_t>(p.pageNumber);
        for (std::int32_t i = 18; i < 22; ++i) {
            ptr[i] = static_cast<std::uint8_t>(pageNumber & 0xff);
            pageNumber >>= 8;
        }
        ptr[26] = static_cast<std::uint8_t>(p.lacingSize);
        SetChecksum(ptr, length);

        page  = ptr;
        bytes = static_cast<std::int32_t>(length);
        --pagesFill_;
        std::memmove(pages_.Data(), pages_.Data() + 1, pagesFill_ * sizeof(Page));
        return true;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
