// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#include "Coefficients.h"

#include <cstddef>
#include <cstdint>

#include <Windows.h>

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    bool Coefficients::Allocate(const EncoderSetup& setup) noexcept
    {
        Reset();
        components_ = setup.frame.components;
        for (std::int32_t c = 0; c < components_; ++c) {
            const auto        i     = static_cast<std::size_t>(c);
            const std::size_t count = static_cast<std::size_t>(setup.widthInBlocks[i]) * static_cast<std::size_t>(setup.heightInBlocks[i]) * DctSize2;
            planes_[i]              = static_cast<std::int16_t*>(HeapAlloc(GetProcessHeap(), 0, count * sizeof(std::int16_t)));
            dc_[i]                  = static_cast<std::int16_t*>(HeapAlloc(GetProcessHeap(), 0, count / DctSize2 * sizeof(std::int16_t)));
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
            HeapFree(GetProcessHeap(), 0, planes_[i]);
        if (dc_[i] != nullptr)
            HeapFree(GetProcessHeap(), 0, dc_[i]);
        planes_[i] = nullptr;
        dc_[i]     = nullptr;
    }

    void Coefficients::Reset() noexcept
    {
        for (std::int16_t*& plane : planes_) {
            if (plane != nullptr)
                HeapFree(GetProcessHeap(), 0, plane);
            plane = nullptr;
        }
        for (std::int16_t*& plane : dc_) {
            if (plane != nullptr)
                HeapFree(GetProcessHeap(), 0, plane);
            plane = nullptr;
        }
        width_      = {};
        height_     = {};
        components_ = 0;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
