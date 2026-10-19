// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "OpusPacket.h"
#include "OpusDefines.h"

#include <cstring>

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    namespace {

        // libopus: encode_size() (src/opus.c)
        std::int32_t EncodeSize(std::int32_t size, std::uint8_t* data) noexcept
        {
            if (size < 252) {
                data[0] = static_cast<std::uint8_t>(size);
                return 1;
            }
            data[0] = static_cast<std::uint8_t>(252 + (size & 0x3));
            data[1] = static_cast<std::uint8_t>((size - static_cast<std::int32_t>(data[0])) >> 2);
            return 2;
        }

        // libopus: parse_size() (src/opus.c)
        std::int32_t ParseSize(const std::uint8_t* data, std::int32_t len, std::int16_t* size) noexcept
        {
            if (len < 1) {
                *size = -1;
                return -1;
            }
            if (data[0] < 252) {
                *size = data[0];
                return 1;
            }
            if (len < 2) {
                *size = -1;
                return -1;
            }
            *size = static_cast<std::int16_t>(4 * data[1] + data[0]);
            return 2;
        }

        // libopus: opus_packet_get_samples_per_frame() (src/opus.c)
        std::int32_t SamplesPerFrame(const std::uint8_t* data, std::int32_t fs) noexcept
        {
            if ((data[0] & 0x80) != 0) {
                const std::int32_t audiosize = (data[0] >> 3) & 0x3;
                return (fs << audiosize) / 400;
            }
            if ((data[0] & 0x60) == 0x60)
                return (data[0] & 0x08) != 0 ? fs / 50 : fs / 100;
            const std::int32_t audiosize = (data[0] >> 3) & 0x3;
            if (audiosize == 3)
                return fs * 60 / 1000;
            return (fs << audiosize) / 100;
        }

        // libopus: opus_packet_get_nb_frames() (src/opus_decoder.c)
        std::int32_t NbFramesOf(const std::uint8_t* packet, std::int32_t len) noexcept
        {
            if (len < 1)
                return OPUS_BAD_ARG;
            const std::int32_t count = packet[0] & 0x3;
            if (count == 0)
                return 1;
            if (count != 3)
                return 2;
            if (len < 2)
                return OPUS_INVALID_PACKET;
            return packet[1] & 0x3F;
        }

        // The padding of the packets of this encoder: 0x01 bytes (one-byte padding) and a 0x00
        // (the rest is padding). Anything else would be an extension, which the port does not handle.
        bool PaddingHasNoExtensions(const std::uint8_t* data, std::int32_t len) noexcept
        {
            for (std::int32_t i = 0; i < len; ++i) {
                if (data[i] == 0x00)
                    return true;
                if (data[i] != 0x01)
                    return false;
            }
            return true;
        }

        // libopus: opus_packet_parse_impl() (src/opus.c) (not self-delimited) with the padding
        std::int32_t ParsePacket(const std::uint8_t* data, std::int32_t len, std::uint8_t* outToc, const std::uint8_t* frames[48], std::int16_t size[48],
                                 const std::uint8_t** padding, std::int32_t* paddingLen) noexcept
        {
            *padding    = nullptr;
            *paddingLen = 0;
            if (len < 0)
                return OPUS_BAD_ARG;
            if (len == 0)
                return OPUS_INVALID_PACKET;

            const std::int32_t framesize = SamplesPerFrame(data, 48000);
            std::int32_t       pad       = 0;
            std::int32_t       count     = 0;
            const std::uint8_t toc       = *data++;
            --len;
            std::int32_t lastSize = len;
            switch (toc & 0x3) {
                case 0: // one frame
                    count = 1;
                    break;
                case 1: // two CBR frames
                    count = 2;
                    if ((len & 0x1) != 0)
                        return OPUS_INVALID_PACKET;
                    lastSize = len / 2;
                    size[0]  = static_cast<std::int16_t>(lastSize);
                    break;
                case 2: { // two VBR frames
                    count                    = 2;
                    const std::int32_t bytes = ParseSize(data, len, size);
                    len -= bytes;
                    if (size[0] < 0 || size[0] > len)
                        return OPUS_INVALID_PACKET;
                    data += bytes;
                    lastSize = len - size[0];
                    break;
                }
                default: { // multiple CBR/VBR frames (0 to 120 ms)
                    if (len < 1)
                        return OPUS_INVALID_PACKET;
                    // the number of frames in bits 0 to 5
                    const std::uint8_t ch = *data++;
                    count                 = ch & 0x3F;
                    if (count <= 0 || framesize * count > 5760)
                        return OPUS_INVALID_PACKET;
                    --len;
                    // the padding flag is bit 6
                    if ((ch & 0x40) != 0) {
                        std::int32_t p = 0;
                        do {
                            if (len <= 0)
                                return OPUS_INVALID_PACKET;
                            p = *data++;
                            --len;
                            const std::int32_t tmp = p == 255 ? 254 : p;
                            len -= tmp;
                            pad += tmp;
                        } while (p == 255);
                    }
                    if (len < 0)
                        return OPUS_INVALID_PACKET;
                    // the VBR flag is bit 7
                    const bool cbr = (ch & 0x80) == 0;
                    if (!cbr) {
                        lastSize = len;
                        for (std::int32_t i = 0; i < count - 1; ++i) {
                            const std::int32_t bytes = ParseSize(data, len, size + i);
                            len -= bytes;
                            if (size[i] < 0 || size[i] > len)
                                return OPUS_INVALID_PACKET;
                            data += bytes;
                            lastSize -= bytes + size[i];
                        }
                        if (lastSize < 0)
                            return OPUS_INVALID_PACKET;
                    } else {
                        lastSize = len / count;
                        if (lastSize * count != len)
                            return OPUS_INVALID_PACKET;
                        for (std::int32_t i = 0; i < count - 1; ++i)
                            size[i] = static_cast<std::int16_t>(lastSize);
                    }
                    break;
                }
            }
            // the last size is implicit: it can exceed 1275
            if (lastSize > 1275)
                return OPUS_INVALID_PACKET;
            size[count - 1] = static_cast<std::int16_t>(lastSize);

            for (std::int32_t i = 0; i < count; ++i) {
                frames[i] = data;
                data += size[i];
            }
            *padding    = data;
            *paddingLen = pad;
            *outToc     = toc;
            return count;
        }

    } // namespace

    // libopus: opus_strerror() (celt/celt.c)
    const char* OpusStrError(std::int32_t error) noexcept
    {
        static const char* const errorStrings[8] = {"success",          "invalid argument",        "buffer too small", "internal error",
                                                    "corrupted stream", "request not implemented", "invalid state",    "memory allocation failed"};
        if (error > 0 || error < -7)
            return "unknown error";
        return errorStrings[-error];
    }

    // libopus: opus_packet_get_nb_samples() (src/opus_decoder.c)
    std::int32_t PacketGetNbSamples(const std::uint8_t* packet, std::int32_t len, std::int32_t fs) noexcept
    {
        const std::int32_t count = NbFramesOf(packet, len);
        if (count < 0)
            return count;
        const std::int32_t samples = count * SamplesPerFrame(packet, fs);
        // no more than 120 ms
        if (samples * 25 > fs * 3)
            return OPUS_INVALID_PACKET;
        return samples;
    }

    // libopus: opus_repacketizer_cat_impl() (src/repacketizer.c)
    std::int32_t Repacketizer::Cat(const std::uint8_t* data, std::int32_t len) noexcept
    {
        // the ToC
        if (len < 1)
            return OPUS_INVALID_PACKET;
        if (nbFrames_ == 0) {
            toc_       = data[0];
            framesize_ = SamplesPerFrame(data, 8000);
        } else if ((toc_ & 0xFC) != (data[0] & 0xFC)) {
            return OPUS_INVALID_PACKET;
        }
        std::int32_t currNbFrames = NbFramesOf(data, len);
        if (currNbFrames < 1)
            return OPUS_INVALID_PACKET;

        // at most 120 ms per packet
        if ((currNbFrames + nbFrames_) * framesize_ > 960)
            return OPUS_INVALID_PACKET;

        std::uint8_t        tmpToc     = 0;
        const std::uint8_t* padding    = nullptr;
        std::int32_t        paddingLen = 0;
        const std::int32_t  ret        = ParsePacket(data, len, &tmpToc, &frames_[nbFrames_], &len_[nbFrames_], &padding, &paddingLen);
        if (ret < 1)
            return ret;
        if (!PaddingHasNoExtensions(padding, paddingLen))
            return OPUS_UNIMPLEMENTED;

        nbFrames_ += currNbFrames;
        return OPUS_OK;
    }

    // libopus: opus_repacketizer_out_range_impl() (src/repacketizer.c)
    std::int32_t Repacketizer::OutRange(std::int32_t begin, std::int32_t end, std::uint8_t* data, std::int32_t maxLen, bool selfDelimited, bool pad) noexcept
    {
        if (begin < 0 || begin >= end || end > nbFrames_)
            return OPUS_BAD_ARG;
        const std::int32_t   count     = end - begin;
        const std::int16_t*  len       = len_ + begin;
        const std::uint8_t** frames    = frames_ + begin;
        std::int32_t         totSize   = selfDelimited ? 1 + (len[count - 1] >= 252) : 0;
        std::int32_t         onesBegin = 0;
        std::int32_t         onesEnd   = 0;

        std::uint8_t* ptr = data;
        if (count == 1) {
            // code 0
            totSize += len[0] + 1;
            if (totSize > maxLen)
                return OPUS_BUFFER_TOO_SMALL;
            *ptr++ = toc_ & 0xFC;
        } else if (count == 2) {
            if (len[1] == len[0]) {
                // code 1
                totSize += 2 * len[0] + 1;
                if (totSize > maxLen)
                    return OPUS_BUFFER_TOO_SMALL;
                *ptr++ = (toc_ & 0xFC) | 0x1;
            } else {
                // code 2
                totSize += len[0] + len[1] + 2 + (len[0] >= 252);
                if (totSize > maxLen)
                    return OPUS_BUFFER_TOO_SMALL;
                *ptr++ = (toc_ & 0xFC) | 0x2;
                ptr += EncodeSize(len[0], ptr);
            }
        }
        if (count > 2 || (pad && totSize < maxLen)) {
            // code 3, restarted for the padding case
            ptr      = data;
            totSize  = selfDelimited ? 1 + (len[count - 1] >= 252) : 0;
            bool vbr = false;
            for (std::int32_t i = 1; i < count; ++i) {
                if (len[i] != len[0]) {
                    vbr = true;
                    break;
                }
            }
            if (vbr) {
                totSize += 2;
                for (std::int32_t i = 0; i < count - 1; ++i)
                    totSize += 1 + (len[i] >= 252) + len[i];
                totSize += len[count - 1];
                if (totSize > maxLen)
                    return OPUS_BUFFER_TOO_SMALL;
                *ptr++ = (toc_ & 0xFC) | 0x3;
                *ptr++ = static_cast<std::uint8_t>(count | 0x80);
            } else {
                totSize += count * len[0] + 2;
                if (totSize > maxLen)
                    return OPUS_BUFFER_TOO_SMALL;
                *ptr++ = (toc_ & 0xFC) | 0x3;
                *ptr++ = static_cast<std::uint8_t>(count);
            }
            const std::int32_t padAmount = pad ? (maxLen - totSize) : 0;
            if (padAmount != 0) {
                data[1] |= 0x40;
                const std::int32_t nb255s = (padAmount - 1) / 255;
                if (totSize + nb255s + 1 > maxLen)
                    return OPUS_BUFFER_TOO_SMALL;
                // 0x01 padding first
                onesBegin = totSize + nb255s + 1;
                onesEnd   = totSize + padAmount;
                for (std::int32_t i = 0; i < nb255s; ++i)
                    *ptr++ = 255;
                *ptr++ = static_cast<std::uint8_t>(padAmount - 255 * nb255s - 1);
                totSize += padAmount;
            }
            if (vbr) {
                for (std::int32_t i = 0; i < count - 1; ++i)
                    ptr += EncodeSize(len[i], ptr);
            }
        }
        if (selfDelimited)
            ptr += EncodeSize(len[count - 1], ptr);
        // the actual data (memmove: in-place padding)
        for (std::int32_t i = 0; i < count; ++i) {
            std::memmove(ptr, frames[i], static_cast<std::size_t>(len[i]));
            ptr += len[i];
        }
        for (std::int32_t i = onesBegin; i < onesEnd; ++i)
            data[i] = 0x01;
        if (pad) {
            // padding with zeros
            while (ptr < data + maxLen)
                *ptr++ = 0;
        }
        return totSize;
    }

    // libopus: opus_packet_pad_impl() (src/repacketizer.c)
    std::int32_t PacketPad(std::uint8_t* data, std::int32_t len, std::int32_t newLen) noexcept
    {
        if (len < 1)
            return OPUS_BAD_ARG;
        if (len == newLen)
            return OPUS_OK;
        if (len > newLen)
            return OPUS_BAD_ARG;
        // the payload copied away for in-place padding
        std::uint8_t copy[1276 * 6];
        if (len > static_cast<std::int32_t>(sizeof(copy)))
            return OPUS_BAD_ARG;
        std::memcpy(copy, data, static_cast<std::size_t>(len));
        Repacketizer rp;
        rp.Init();
        std::int32_t ret = rp.Cat(copy, len);
        if (ret != OPUS_OK)
            return ret;
        ret = rp.OutRange(0, rp.NbFrames(), data, newLen, false, true);
        return ret > 0 ? OPUS_OK : ret;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
