// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <cstddef>
#include <cstdint>

#include "ByteBuffer.h"

import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    /// <summary>
    /// The node Write file: writes the finished JPEG with AsyncFile::WriteFile on the I/O
    /// scheduler, while the compute workers free the encoder's buffers (Release). NTFS completes
    /// writes that extend a file synchronously, so the copying into the file cache happens on the
    /// I/O thread rather than in flight; either way not in the encoder's time.
    /// </summary>
    class AsyncOutput : public Tasks::Task, public Memory::IReadSequentialMemory
    {
    public:
        AsyncOutput(const wchar_t* path, const ByteBuffer& data) noexcept : path_(path), data_(data) {}

        void Run() noexcept override;

        bool          Failed() const noexcept { return error_ != 0; }
        std::uint32_t Error() const noexcept { return error_; }

        // IReadSequentialMemory (AsyncFile::WriteFile): the JPEG in file order
        std::size_t      GetSize() const noexcept override { return data_.Size(); }
        const std::byte* RequestReadMemory(std::size_t& size) noexcept override;
        void             ReleaseReadMemory(std::size_t size) noexcept override;

    private:
        const wchar_t*    path_;
        const ByteBuffer& data_;
        std::size_t       handedOut_ = 0;
        std::uint32_t     error_     = 0;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
