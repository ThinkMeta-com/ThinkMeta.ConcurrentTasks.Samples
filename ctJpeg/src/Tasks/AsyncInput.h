// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <cstddef>
#include <cstdint>
#include <memory>

import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;
import ThinkMeta.ConcurrentTasks.Synchronization.ManualResetEvent;

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    /// <summary>
    /// The node Read: reads the input file with AsyncFile::ReadFile (overlapped reads in flight,
    /// on the I/O scheduler) into one buffer in file order. Each block ReadFile releases goes to
    /// the waiters at once, so the Pixel jobs start on their rows while the rest of the file is
    /// still on its way.
    /// <para>
    /// Synchronization: the buffer and the error are written on the I/O thread before the event of
    /// their step is set, and read only after a WaitFor on that step returned; the event's Set and
    /// Wait order them (no other fence is needed).
    /// </para>
    /// </summary>
    class AsyncInput : public Tasks::Task, public Memory::IWriteSequentialMemory
    {
    public:
        AsyncInput() noexcept = default;
        ~AsyncInput();

        AsyncInput(const AsyncInput&)            = delete;
        AsyncInput& operator=(const AsyncInput&) = delete;

        /// <summary>
        /// Sizes the file; granularity is the step in which waiters are woken. False on an error
        /// (a Windows error code in Error()).
        /// </summary>
        bool Open(const wchar_t* path, std::size_t granularity) noexcept;

        void Run() noexcept override;

        /// <summary>Waits until the first bytes of the file have arrived or the read has failed.</summary>
        void WaitFor(std::size_t bytes) noexcept;

        /// <summary>The bytes that have arrived once WaitFor(bytes) returned without a failure: whole steps.</summary>
        std::size_t ArrivedAfter(std::size_t bytes) const noexcept
        {
            const std::size_t end = bytes == 0 ? 0 : ((bytes - 1) / step_ + 1) * step_;
            return end < size_ ? end : size_;
        }

        /// <summary>The file contents; valid after the first WaitFor.</summary>
        const std::uint8_t* Data() const noexcept { return buffer_; }
        std::size_t         Size() const noexcept { return size_; }
        bool                Failed() const noexcept { return error_ != 0; }
        std::uint32_t       Error() const noexcept { return error_; }

        /// <summary>Frees the buffer after waiting for the end of the read.</summary>
        void Release() noexcept;

        // IWriteSequentialMemory (AsyncFile::ReadFile): the buffer in file order
        void       Initialize(std::size_t initialSize) noexcept override;
        std::byte* RequestWriteMemory(std::size_t size) noexcept override;
        void       ReleaseWriteMemory(std::size_t size) noexcept override;
        void       SetEndOfStream() noexcept override;

    private:
        const wchar_t*                                       path_   = nullptr;
        std::uint8_t*                                        buffer_ = nullptr; // VirtualAlloc, by Initialize
        std::size_t                                          size_   = 0;
        std::size_t                                          step_   = 0;
        std::size_t                                          steps_  = 0;
        std::unique_ptr<Synchronization::ManualResetEvent[]> arrived_;          // per step, set in file order; many waiters: the Pixel tiles
        std::size_t                                          requested_    = 0; // handed to ReadFile
        std::size_t                                          arrivedBytes_ = 0;
        std::size_t                                          signalled_    = 0; // steps whose event is set
        std::uint32_t                                        error_        = 0;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
