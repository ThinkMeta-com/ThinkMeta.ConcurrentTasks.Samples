// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstddef>
#include <cstdint>

#include "HeapArray.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    /// <summary>The OpusHead identification header (RFC 7845), as libopusenc's opus_header.c writes it.</summary>
    struct OpusHead {
        std::int32_t  channels        = 0;
        std::int32_t  preskip         = 0;
        std::uint32_t inputSampleRate = 0;
        std::int32_t  gain            = 0;
        std::int32_t  channelMapping  = 0;
        std::int32_t  nbStreams       = 0;
        std::int32_t  nbCoupled       = 0;
        std::uint8_t  streamMap[255]  = {};

        /// <summary>opeint_opus_header_get_size: the buffer size libopusenc reserves (not the packet size).</summary>
        std::int32_t BufferSize() const noexcept { return 21 + channels; }

        /// <summary>opeint_opus_header_to_packet; returns the packet size, 0 if it does not fit.</summary>
        std::int32_t ToPacket(std::uint8_t* packet, std::int32_t length) const noexcept;
    };

    /// <summary>The OpusTags comment header (libopusenc's OggOpusComments and opeint_comment_*).</summary>
    class OpusTags
    {
    public:
        /// <summary>ope_comments_create with the vendor string; false if out of memory.</summary>
        bool Initialize(const char* vendor) noexcept;

        /// <summary>A deep copy (stream_create copies the comments); false if out of memory.</summary>
        bool CopyFrom(const OpusTags& other) noexcept;

        /// <summary>ope_comments_add: TAG=value; false if the tag contains '=' or out of memory.</summary>
        bool Add(const char* tag, const char* value) noexcept;

        /// <summary>ope_comments_add_string: an entry already in the form TAG=value.</summary>
        bool AddString(const char* tagAndValue) noexcept;

        /// <summary>opeint_comment_pad: at least amount bytes of zero padding, rounded up to fill the Ogg segments.</summary>
        bool Pad(std::int32_t amount) noexcept;

        const std::uint8_t* Data() const noexcept { return data_.Data(); }
        std::int32_t        Length() const noexcept { return length_; }

    private:
        bool AddEntry(const char* tag, const char* value) noexcept;

        HeapArray<std::uint8_t> data_;
        std::int32_t            length_ = 0;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
