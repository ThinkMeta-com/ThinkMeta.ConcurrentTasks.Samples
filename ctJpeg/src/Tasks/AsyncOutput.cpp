// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "AsyncOutput.h"

#include <cstddef>
#include <cstdint>

import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    using Windows::Error;

    void AsyncOutput::Run() noexcept
    {
        const std::uint32_t result = AsyncIo::AsyncFile::WriteFile(path_, GENERIC_WRITE, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, this);
        if (Error::Failed(result))
            error_ = result;
    }

    const std::byte* AsyncOutput::RequestReadMemory(std::size_t& size) noexcept
    {
        const std::size_t left = data_.Size() - handedOut_;
        if (left == 0) {
            size = 0;
            return nullptr;
        }
        if (size > left)
            size = left;
        const auto* at = reinterpret_cast<const std::byte*>(data_.Data()) + handedOut_;
        handedOut_ += size;
        return at;
    }

    void AsyncOutput::ReleaseReadMemory(std::size_t) noexcept
    {}

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
