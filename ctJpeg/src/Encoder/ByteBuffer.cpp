// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "ByteBuffer.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Memory.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

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
        void* block = data_ == nullptr ? Shared::Heap::Allocate(capacity) : Shared::Heap::Reallocate(data_, capacity_, capacity);
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
            Shared::Heap::Free(data_);
        data_     = nullptr;
        size_     = 0;
        capacity_ = 0;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
