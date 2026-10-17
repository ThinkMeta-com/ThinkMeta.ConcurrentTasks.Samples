// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

#include "HeapArray.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    /// <summary>
    /// One float array per channel in reserved virtual memory: committed as it fills, decommitted
    /// behind the reader, so a long input needs only its window in physical memory. Fresh pages
    /// read as zeros. One task commits and decommits; others may read committed ranges.
    /// </summary>
    class PlanarStore
    {
    public:
        PlanarStore() noexcept                     = default;
        PlanarStore(const PlanarStore&)            = delete;
        PlanarStore& operator=(const PlanarStore&) = delete;
        ~PlanarStore();

        /// <summary>Reserves capacity frames per channel; false if the address space is not available.</summary>
        bool Initialize(std::int32_t channels, std::int64_t capacity) noexcept;

        float*              Channel(std::int32_t c) noexcept { return channels_[c]; }
        const float* const* Channels() const noexcept { return channels_.Data(); }
        std::int64_t        Capacity() const noexcept { return capacity_; }

        /// <summary>Makes frames [0, frames) usable; false if out of memory or beyond the capacity.</summary>
        bool Commit(std::int64_t frames) noexcept;

        /// <summary>Gives back the memory of whole chunks below frame (the contents are lost).</summary>
        void Decommit(std::int64_t frame) noexcept;

    private:
        HeapArray<float*> channels_;
        std::int32_t      count_       = 0;
        std::int64_t      capacity_    = 0;
        std::int64_t      committed_   = 0; // frames committed from the start
        std::int64_t      decommitted_ = 0; // frames decommitted from the start
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
