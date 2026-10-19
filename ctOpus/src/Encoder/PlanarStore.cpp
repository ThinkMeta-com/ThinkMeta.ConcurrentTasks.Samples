// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#include "PlanarStore.h"

#include <Windows.h>

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    namespace {

        constexpr std::int64_t ChunkFrames = 64 * 1024; // 256 KiB per channel and chunk

        std::int64_t RoundUp(std::int64_t frames) noexcept
        {
            return (frames + ChunkFrames - 1) / ChunkFrames * ChunkFrames;
        }

    } // namespace

    PlanarStore::~PlanarStore()
    {
        for (std::int32_t c = 0; c < count_; ++c)
            if (channels_[c] != nullptr)
                VirtualFree(channels_[c], 0, MEM_RELEASE);
    }

    bool PlanarStore::Initialize(std::int32_t channels, std::int64_t capacity) noexcept
    {
        capacity_ = RoundUp(capacity);
        if (!channels_.Allocate(static_cast<std::size_t>(channels)))
            return false;
        count_ = channels;
        for (std::int32_t c = 0; c < channels; ++c) {
            channels_[c] = static_cast<float*>(VirtualAlloc(nullptr, static_cast<std::size_t>(capacity_) * sizeof(float), MEM_RESERVE, PAGE_READWRITE));
            if (channels_[c] == nullptr)
                return false;
        }
        committed_ = decommitted_ = 0;
        return true;
    }

    bool PlanarStore::Commit(std::int64_t frames) noexcept
    {
        if (frames <= committed_)
            return true;
        const std::int64_t end = RoundUp(frames) < capacity_ ? RoundUp(frames) : capacity_;
        if (end < frames)
            return false;
        for (std::int32_t c = 0; c < count_; ++c) {
            if (VirtualAlloc(channels_[c] + committed_, static_cast<std::size_t>(end - committed_) * sizeof(float), MEM_COMMIT, PAGE_READWRITE) == nullptr)
                return false;
        }
        committed_ = end;
        return true;
    }

    void PlanarStore::Decommit(std::int64_t frame) noexcept
    {
        std::int64_t end = frame / ChunkFrames * ChunkFrames;
        end              = end < committed_ ? end : committed_;
        if (end <= decommitted_)
            return;
        for (std::int32_t c = 0; c < count_; ++c)
            VirtualFree(channels_[c] + decommitted_, static_cast<std::size_t>(end - decommitted_) * sizeof(float), MEM_DECOMMIT);
        decommitted_ = end;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
