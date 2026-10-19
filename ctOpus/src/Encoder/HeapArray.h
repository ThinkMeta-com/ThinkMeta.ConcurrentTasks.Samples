// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    /// <summary>
    /// Array of trivially copyable elements on the process heap (HeapAlloc), zero-initialised: no
    /// constructors or destructors run, and Grow moves the bytes.
    /// </summary>
    template <typename T>
    class HeapArray
    {
    public:
        HeapArray() noexcept                   = default;
        HeapArray(const HeapArray&)            = delete;
        HeapArray& operator=(const HeapArray&) = delete;

        HeapArray(HeapArray&& other) noexcept : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

        HeapArray& operator=(HeapArray&& other) noexcept
        {
            if (this != &other) {
                Free();
                data_ = std::exchange(other.data_, nullptr);
                size_ = std::exchange(other.size_, 0);
            }
            return *this;
        }

        ~HeapArray() { Free(); }

        /// <summary>Replaces the contents by count zeroed elements; false if out of memory.</summary>
        bool Allocate(std::size_t count) noexcept
        {
            static_assert(std::is_trivially_copyable_v<T>, "HeapArray runs no constructors or destructors");
            Free();
            if (count == 0)
                return true;
            data_ = static_cast<T*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, count * sizeof(T)));
            if (data_ == nullptr)
                return false;
            size_ = count;
            return true;
        }

        /// <summary>Grows to count elements, keeping the contents; new elements are zeroed.</summary>
        bool Grow(std::size_t count) noexcept
        {
            static_assert(std::is_trivially_copyable_v<T>, "HeapArray moves the elements as bytes");
            if (count <= size_)
                return true;
            if (data_ == nullptr)
                return Allocate(count);
            void* p = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, data_, count * sizeof(T));
            if (p == nullptr)
                return false;
            data_ = static_cast<T*>(p);
            size_ = count;
            return true;
        }

        void Free() noexcept
        {
            if (data_ != nullptr)
                HeapFree(GetProcessHeap(), 0, data_);
            data_ = nullptr;
            size_ = 0;
        }

        T*          Data() noexcept { return data_; }
        const T*    Data() const noexcept { return data_; }
        std::size_t Size() const noexcept { return size_; }
        T&          operator[](std::size_t i) noexcept { return data_[i]; }
        const T&    operator[](std::size_t i) const noexcept { return data_[i]; }

    private:
        T*          data_ = nullptr;
        std::size_t size_ = 0;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
