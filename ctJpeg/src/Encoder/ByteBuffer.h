// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstddef>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    /// <summary>A growing byte buffer on the process heap (HeapAlloc/HeapReAlloc).</summary>
    class ByteBuffer
    {
    public:
        ByteBuffer() noexcept = default;
        ByteBuffer(ByteBuffer&& other) noexcept;
        ByteBuffer& operator=(ByteBuffer&& other) noexcept;
        ByteBuffer(const ByteBuffer&)            = delete;
        ByteBuffer& operator=(const ByteBuffer&) = delete;
        ~ByteBuffer() { Reset(); }

        std::uint8_t*       Data() noexcept { return data_; }
        const std::uint8_t* Data() const noexcept { return data_; }
        std::size_t         Size() const noexcept { return size_; }
        std::size_t         Capacity() const noexcept { return capacity_; }

        /// <summary>Makes room for at least extra more bytes; false if out of memory.</summary>
        bool Reserve(std::size_t extra) noexcept;

        /// <summary>Appends bytes; false if out of memory.</summary>
        bool Append(const std::uint8_t* bytes, std::size_t count) noexcept;

        /// <summary>Appends one byte; false if out of memory.</summary>
        bool Append(std::uint8_t byte) noexcept { return Append(&byte, 1); }

        /// <summary>Sets the size after writing directly into Data() (at most Capacity()).</summary>
        void SetSize(std::size_t size) noexcept { size_ = size; }

        void Clear() noexcept { size_ = 0; }

        /// <summary>Frees the memory.</summary>
        void Reset() noexcept;

    private:
        std::uint8_t* data_     = nullptr;
        std::size_t   size_     = 0;
        std::size_t   capacity_ = 0;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
