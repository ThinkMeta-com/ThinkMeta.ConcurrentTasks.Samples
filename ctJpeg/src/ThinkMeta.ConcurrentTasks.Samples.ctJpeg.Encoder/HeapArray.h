// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#pragma once

#include <cstddef>
#include <cstdint>

#include <Windows.h>

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    /// <summary>
    /// A fixed-size array of trivial elements on the process heap (no std::vector). Reserve sets
    /// up a large array of which only a part will be used: the address space is reserved and
    /// committed in steps by Ensure, so the unused rest costs neither commit charge nor the time
    /// to commit and free it.
    /// </summary>
    template <typename T>
    class HeapArray
    {
    public:
        HeapArray() noexcept = default;
        HeapArray(HeapArray&& other) noexcept : data_(other.data_), size_(other.size_), committed_(other.committed_), reserved_(other.reserved_)
        {
            other.data_      = nullptr;
            other.size_      = 0;
            other.committed_ = 0;
            other.reserved_  = false;
        }
        HeapArray& operator=(HeapArray&& other) noexcept
        {
            if (this != &other) {
                Reset();
                data_            = other.data_;
                size_            = other.size_;
                committed_       = other.committed_;
                reserved_        = other.reserved_;
                other.data_      = nullptr;
                other.size_      = 0;
                other.committed_ = 0;
                other.reserved_  = false;
            }
            return *this;
        }
        HeapArray(const HeapArray&)            = delete;
        HeapArray& operator=(const HeapArray&) = delete;
        ~HeapArray() { Reset(); }

        /// <summary>Allocates count elements (zeroed if zero is set); false if out of memory.</summary>
        bool Allocate(std::size_t count, bool zero = false) noexcept
        {
            Reset();
            if (count == 0)
                count = 1;
            data_      = static_cast<T*>(HeapAlloc(GetProcessHeap(), zero ? HEAP_ZERO_MEMORY : 0, count * sizeof(T)));
            size_      = data_ != nullptr ? count : 0;
            committed_ = size_;
            return data_ != nullptr;
        }

        /// <summary>
        /// Like Allocate (not zeroed), but a large array is only reserved: elements below
        /// Ensure(n) may be used. False if out of memory.
        /// </summary>
        bool Reserve(std::size_t count) noexcept
        {
            if (count * sizeof(T) < LargeBytes)
                return Allocate(count);
            Reset();
            data_ = static_cast<T*>(VirtualAlloc(nullptr, count * sizeof(T), MEM_RESERVE, PAGE_READWRITE));
            if (data_ == nullptr)
                return false;
            size_     = count;
            reserved_ = true;
            return true;
        }

        /// <summary>Makes the elements [0, count) usable (at most Size() of them); false if out of memory.</summary>
        bool Ensure(std::size_t count) noexcept
        {
            if (count > size_)
                count = size_; // callers pass a generous bound
            if (count <= committed_)
                return true;
            // commit in steps of CommitStep bytes
            std::size_t bytes = (count * sizeof(T) + CommitStep - 1) / CommitStep * CommitStep;
            if (bytes > size_ * sizeof(T))
                bytes = size_ * sizeof(T);
            std::byte* const  base = reinterpret_cast<std::byte*>(data_);
            const std::size_t from = committed_ * sizeof(T) / CommitStep * CommitStep;
            if (VirtualAlloc(base + from, bytes - from, MEM_COMMIT, PAGE_READWRITE) == nullptr)
                return false;
            committed_ = bytes / sizeof(T);
            return committed_ >= count;
        }

        void Reset() noexcept
        {
            if (data_ != nullptr) {
                if (reserved_)
                    VirtualFree(data_, 0, MEM_RELEASE);
                else
                    HeapFree(GetProcessHeap(), 0, data_);
            }
            data_      = nullptr;
            size_      = 0;
            committed_ = 0;
            reserved_  = false;
        }

        T*          Data() noexcept { return data_; }
        const T*    Data() const noexcept { return data_; }
        std::size_t Size() const noexcept { return size_; }
        T&          operator[](std::size_t i) noexcept { return data_[i]; }
        const T&    operator[](std::size_t i) const noexcept { return data_[i]; }

    private:
        static constexpr std::size_t LargeBytes = 1u << 18; // smaller arrays are allocated whole
        static constexpr std::size_t CommitStep = 1u << 16;

        T*          data_      = nullptr;
        std::size_t size_      = 0;
        std::size_t committed_ = 0; // usable elements
        bool        reserved_  = false;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
