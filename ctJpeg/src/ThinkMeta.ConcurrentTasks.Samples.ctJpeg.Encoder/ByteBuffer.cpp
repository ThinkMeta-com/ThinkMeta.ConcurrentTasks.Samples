// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#include "ByteBuffer.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <Windows.h>

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    ByteBuffer::ByteBuffer(ByteBuffer&& other) noexcept : data_(other.data_), size_(other.size_), capacity_(other.capacity_)
    {
        other.data_     = nullptr;
        other.size_     = 0;
        other.capacity_ = 0;
    }

    ByteBuffer& ByteBuffer::operator=(ByteBuffer&& other) noexcept
    {
        if (this != &other) {
            Reset();
            data_           = other.data_;
            size_           = other.size_;
            capacity_       = other.capacity_;
            other.data_     = nullptr;
            other.size_     = 0;
            other.capacity_ = 0;
        }
        return *this;
    }

    bool ByteBuffer::Reserve(std::size_t extra) noexcept
    {
        if (capacity_ - size_ >= extra)
            return true;
        std::size_t capacity = capacity_ < 4096 ? 4096 : capacity_;
        while (capacity - size_ < extra)
            capacity *= 2;
        void* block = data_ == nullptr ? HeapAlloc(GetProcessHeap(), 0, capacity) : HeapReAlloc(GetProcessHeap(), 0, data_, capacity);
        if (block == nullptr)
            return false;
        data_     = static_cast<std::uint8_t*>(block);
        capacity_ = capacity;
        return true;
    }

    bool ByteBuffer::Append(const std::uint8_t* bytes, std::size_t count) noexcept
    {
        if (!Reserve(count))
            return false;
        std::memcpy(data_ + size_, bytes, count);
        size_ += count;
        return true;
    }

    void ByteBuffer::Reset() noexcept
    {
        if (data_ != nullptr)
            HeapFree(GetProcessHeap(), 0, data_);
        data_     = nullptr;
        size_     = 0;
        capacity_ = 0;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
