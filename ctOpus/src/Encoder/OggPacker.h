// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstddef>
#include <cstdint>

#include "HeapArray.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    /// <summary>
    /// Port of libopusenc's Ogg page writer (ogg_packer.c): packets are collected into pages, which
    /// are closed when the lacing table would overflow, when the muxing delay is exceeded, or on
    /// request. Byte-identical to the original, including how it sizes continued pages.
    /// </summary>
    class OggPacker
    {
    public:
        /// <summary>oggp_create; false if out of memory.</summary>
        bool Initialize(std::int32_t serialNumber) noexcept;

        /// <summary>oggp_set_muxing_delay, in granule position units.</summary>
        void SetMuxingDelay(std::uint64_t delay) noexcept { muxingDelay_ = delay; }

        /// <summary>oggp_get_packet_buffer: room for the next packet of at most bytes bytes.</summary>
        std::uint8_t* GetPacketBuffer(std::int32_t bytes) noexcept;

        /// <summary>oggp_commit_packet: the packet buffer now holds bytes bytes.</summary>
        bool CommitPacket(std::int32_t bytes, std::uint64_t granulePosition, bool endOfStream) noexcept;

        /// <summary>oggp_flush_page: closes the open page(s); false if there was nothing to flush.</summary>
        bool FlushPage() noexcept;

        /// <summary>oggp_get_next_page: the next finished page, valid until the next call.</summary>
        bool GetNextPage(const std::uint8_t*& page, std::int32_t& bytes) noexcept;

    private:
        struct Page {
            std::uint64_t granulePosition;
            std::size_t   bufPos;
            std::size_t   bufSize;
            std::size_t   lacingPos;
            std::size_t   lacingSize;
            std::int32_t  flags;
            std::size_t   pageNumber;
        };

        void ShiftBuffer() noexcept;

        std::int32_t            serialNumber_ = 0;
        HeapArray<std::uint8_t> allocBuffer_; // MaxHeaderSize bytes in front of buf
        std::uint8_t*           buf_      = nullptr;
        std::uint8_t*           userBuf_  = nullptr;
        std::size_t             bufSize_  = 0;
        std::size_t             bufFill_  = 0;
        std::size_t             bufBegin_ = 0;
        HeapArray<std::uint8_t> lacing_;
        std::size_t             lacingFill_  = 0;
        std::size_t             lacingBegin_ = 0;
        HeapArray<Page>         pages_;
        std::size_t             pagesFill_   = 0;
        std::uint64_t           muxingDelay_ = 0;
        bool                    isEos_       = false;
        std::uint64_t           currGranule_ = 0;
        std::uint64_t           lastGranule_ = 0;
        std::size_t             pageNumber_  = 0;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
