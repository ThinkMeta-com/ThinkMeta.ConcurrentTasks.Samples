// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "Coefficients.h"

#include <cstddef>
#include <cstdint>

#include "Memory.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    bool Coefficients::Allocate(const EncoderSetup& setup) noexcept
    {
        Reset();
        components_ = setup.frame.components;
        for (std::int32_t c = 0; c < components_; ++c) {
            const auto        i     = static_cast<std::size_t>(c);
            const std::size_t count = static_cast<std::size_t>(setup.widthInBlocks[i]) * static_cast<std::size_t>(setup.heightInBlocks[i]) * DctSize2;
            planes_[i]              = static_cast<std::int16_t*>(Shared::Heap::Allocate(count * sizeof(std::int16_t)));
            dc_[i]                  = static_cast<std::int16_t*>(Shared::Heap::Allocate(count / DctSize2 * sizeof(std::int16_t)));
            if (planes_[i] == nullptr || dc_[i] == nullptr) {
                Reset();
                return false;
            }
            width_[i]  = setup.widthInBlocks[i];
            height_[i] = setup.heightInBlocks[i];
        }
        return true;
    }

    void Coefficients::ReleaseComponent(std::int32_t c) noexcept
    {
        const auto i = static_cast<std::size_t>(c);
        if (planes_[i] != nullptr)
            Shared::Heap::Free(planes_[i]);
        if (dc_[i] != nullptr)
            Shared::Heap::Free(dc_[i]);
        planes_[i] = nullptr;
        dc_[i]     = nullptr;
    }

    void Coefficients::Reset() noexcept
    {
        for (std::int16_t*& plane : planes_) {
            if (plane != nullptr)
                Shared::Heap::Free(plane);
            plane = nullptr;
        }
        for (std::int16_t*& plane : dc_) {
            if (plane != nullptr)
                Shared::Heap::Free(plane);
            plane = nullptr;
        }
        width_      = {};
        height_     = {};
        components_ = 0;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
