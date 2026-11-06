// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "AsyncInput.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <new>

#include "File.h"

import ThinkMeta.ConcurrentTasks.Platform.Error;

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    using Platform::Error;

    // the codes of the reader's own errors
#if defined(_WIN32)
    constexpr std::uint32_t OutOfMemory = ERROR_OUTOFMEMORY;
    constexpr std::uint32_t FileChanged = ERROR_FILE_INVALID;
#else
    constexpr std::uint32_t OutOfMemory = ENOMEM;
    constexpr std::uint32_t FileChanged = EIO;
#endif

    AsyncInput::~AsyncInput()
    {
        Shared::AddressSpace::Release(buffer_, size_);
    }

    bool AsyncInput::Open(const Shared::Char* path, std::size_t granularity) noexcept
    {
        std::uint64_t size = 0;
        if (!Shared::File::Size(path, size)) {
            error_ = Error::GetLastError();
            return false;
        }
        size_ = static_cast<std::size_t>(size);
        if (size_ == 0) {
            error_ = Error::EndOfFile;
            return false;
        }
        path_  = path;
        step_  = granularity < 65536 ? 65536 : granularity;
        steps_ = (size_ + step_ - 1) / step_;
        arrived_.reset(new (std::nothrow) Synchronization::ManualResetEvent[steps_]);
        if (!arrived_) {
            error_ = OutOfMemory;
            return false;
        }
        return true;
    }

    void AsyncInput::Run() noexcept
    {
        // the library's default shape (1 MB x 5 in flight)
#if defined(_WIN32)
        const std::uint32_t result = AsyncIo::AsyncFile::ReadFile(path_, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, this);
#else
        const std::uint32_t result = AsyncIo::AsyncFile::ReadFile(path_, Platform::FileOptions{}, this);
#endif
        // The error of the file (open, size, a read) wins over the end-of-stream and size codes set
        // above; Error::NotEnoughMemory is the library's answer to a refused block, whose reason
        // error_ already holds.
        if (Error::Failed(result) && (result != static_cast<std::uint32_t>(Error::NotEnoughMemory) || error_ == 0))
            error_ = result;
        SetEndOfStream(); // ReadFile has called it already, unless it failed before Initialize
    }

    void AsyncInput::Initialize(std::size_t initialSize) noexcept
    {
        if (initialSize == 0) // the file could not be opened or sized: Run takes the error of ReadFile
            return;
        if (initialSize != size_) // the file changed since Open: the reads beyond it fail
            error_ = FileChanged;
        buffer_ = static_cast<std::uint8_t*>(Shared::AddressSpace::Allocate(size_));
        if (buffer_ == nullptr)
            error_ = OutOfMemory;
    }

    std::byte* AsyncInput::RequestWriteMemory(std::size_t size) noexcept
    {
        // no memory, or a file that grew since Open (error_ is set either way): ReadFile ends the
        // read with Error::NotEnoughMemory after the requests in flight
        if (buffer_ == nullptr || requested_ + size > size_)
            return nullptr;
        std::byte* at = reinterpret_cast<std::byte*>(buffer_) + requested_;
        requested_ += size;
        return at;
    }

    void AsyncInput::ReleaseWriteMemory(std::size_t size) noexcept
    {
        arrivedBytes_ += size;
        const std::size_t whole = arrivedBytes_ >= size_ ? steps_ : arrivedBytes_ / step_;
        while (signalled_ < whole)
            arrived_[signalled_++].Set();
    }

    void AsyncInput::SetEndOfStream() noexcept
    {
        if (arrivedBytes_ < size_ && error_ == 0)
            error_ = Error::EndOfFile;
        while (signalled_ < steps_)
            arrived_[signalled_++].Set();
    }

    void AsyncInput::WaitFor(std::size_t bytes) noexcept
    {
        if (bytes == 0 || steps_ == 0)
            return;
        if (bytes > size_)
            bytes = size_;
        arrived_[(bytes - 1) / step_].Wait();
    }

    void AsyncInput::Release() noexcept
    {
        WaitFor(size_);
        Shared::AddressSpace::Release(buffer_, size_);
        buffer_ = nullptr;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
