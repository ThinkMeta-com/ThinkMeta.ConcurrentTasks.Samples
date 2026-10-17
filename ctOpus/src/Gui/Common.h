// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "HeapArray.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::Gui {

    using ctOpus::HeapArray;

    /// <summary>The sample rate everything in the GUI runs at (the Opus API rate).</summary>
    constexpr std::int32_t SampleRate = 48000;
    /// <summary>The GUI encodes stereo.</summary>
    constexpr std::int32_t Channels = 2;
    /// <summary>The largest packet of one Opus frame of at most 20 ms (1275 bytes plus the TOC).</summary>
    constexpr std::int32_t MaxPacketBytes = 1500;

    /// <summary>The time in 100 ns units on the QPC time base, the base of the WASAPI packet timestamps.</summary>
    inline std::int64_t Now100ns() noexcept
    {
        static const std::int64_t frequency = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return f.QuadPart;
        }();
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        const std::int64_t c = counter.QuadPart;
        return c / frequency * 10'000'000 + c % frequency * 10'000'000 / frequency;
    }

    constexpr double ToMs(std::int64_t t100ns) noexcept
    {
        return static_cast<double>(t100ns) / 10'000.0;
    }

    constexpr std::int64_t FramesTo100ns(std::int64_t frames) noexcept
    {
        return frames * 10'000'000 / SampleRate;
    }

    constexpr std::int64_t FramesTo100ns(std::int64_t frames, std::int32_t rate) noexcept
    {
        return rate > 0 ? frames * 10'000'000 / rate : 0;
    }

    /// <summary>Copies a wide string into a fixed buffer, always terminated.</summary>
    inline void CopyText(wchar_t* target, std::size_t capacity, const wchar_t* source) noexcept
    {
        if (capacity == 0)
            return;
        std::size_t i = 0;
        for (; source != nullptr && source[i] != 0 && i + 1 < capacity; ++i)
            target[i] = source[i];
        target[i] = 0;
    }

    /// <summary>
    /// Single-producer single-consumer ring of interleaved stereo float frames. The indices count
    /// all frames ever written and read, so a writer can tag its packets with absolute positions.
    /// </summary>
    class FrameRing
    {
    public:
        bool Allocate(std::int64_t minimumFrames) noexcept
        {
            std::int64_t capacity = 1;
            while (capacity < minimumFrames)
                capacity <<= 1;
            if (!data_.Allocate(static_cast<std::size_t>(capacity * Channels)))
                return false;
            mask_ = capacity - 1;
            write_.store(0, std::memory_order_relaxed);
            read_.store(0, std::memory_order_relaxed);
            return true;
        }

        std::int64_t Capacity() const noexcept { return mask_ + 1; }

        /// <summary>Consumer side: the frames that can be read.</summary>
        std::int64_t Available() const noexcept { return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_relaxed); }

        /// <summary>Either side: the frames buffered right now (approximate for the producer).</summary>
        std::int64_t Fill() const noexcept { return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire); }

        std::int64_t WriteIndex() const noexcept { return write_.load(std::memory_order_acquire); }
        std::int64_t ReadIndex() const noexcept { return read_.load(std::memory_order_acquire); }

        /// <summary>Producer side: writes up to frames frames (nullptr writes silence); returns the frames written.</summary>
        std::int64_t Write(const float* data, std::int64_t frames) noexcept
        {
            const std::int64_t w    = write_.load(std::memory_order_relaxed);
            const std::int64_t free = Capacity() - (w - read_.load(std::memory_order_acquire));
            if (frames > free)
                frames = free;
            for (std::int64_t done = 0; done < frames;) {
                const std::int64_t at    = (w + done) & mask_;
                const std::int64_t chunk = frames - done < Capacity() - at ? frames - done : Capacity() - at;
                float*             dst   = data_.Data() + at * Channels;
                if (data != nullptr)
                    std::memcpy(dst, data + done * Channels, static_cast<std::size_t>(chunk * Channels) * sizeof(float));
                else
                    std::memset(dst, 0, static_cast<std::size_t>(chunk * Channels) * sizeof(float));
                done += chunk;
            }
            write_.store(w + frames, std::memory_order_release);
            return frames;
        }

        /// <summary>Consumer side: copies frames frames (they must be available) without consuming them.</summary>
        void Peek(float* data, std::int64_t frames) const noexcept
        {
            const std::int64_t r = read_.load(std::memory_order_relaxed);
            for (std::int64_t done = 0; done < frames;) {
                const std::int64_t at    = (r + done) & mask_;
                const std::int64_t chunk = frames - done < Capacity() - at ? frames - done : Capacity() - at;
                std::memcpy(data + done * Channels, data_.Data() + at * Channels, static_cast<std::size_t>(chunk * Channels) * sizeof(float));
                done += chunk;
            }
        }

        /// <summary>Consumer side: reads exactly frames frames (they must be available); nullptr skips them.</summary>
        void Read(float* data, std::int64_t frames) noexcept
        {
            const std::int64_t r = read_.load(std::memory_order_relaxed);
            for (std::int64_t done = 0; data != nullptr && done < frames;) {
                const std::int64_t at    = (r + done) & mask_;
                const std::int64_t chunk = frames - done < Capacity() - at ? frames - done : Capacity() - at;
                std::memcpy(data + done * Channels, data_.Data() + at * Channels, static_cast<std::size_t>(chunk * Channels) * sizeof(float));
                done += chunk;
            }
            read_.store(r + frames, std::memory_order_release);
        }

    private:
        HeapArray<float> data_;
        std::int64_t     mask_ = 0;
        alignas(64) std::atomic<std::int64_t> write_{0};
        alignas(64) std::atomic<std::int64_t> read_{0};
    };

    /// <summary>Single-producer single-consumer queue of trivially copyable items.</summary>
    template <typename T, std::int64_t Capacity>
    class SpscQueue
    {
        static_assert((Capacity & (Capacity - 1)) == 0, "the capacity must be a power of two");

    public:
        bool Push(const T& item) noexcept
        {
            const std::int64_t w = write_.load(std::memory_order_relaxed);
            if (w - read_.load(std::memory_order_acquire) >= Capacity)
                return false;
            items_[w & (Capacity - 1)] = item;
            write_.store(w + 1, std::memory_order_release);
            return true;
        }

        /// <summary>The oldest item, or nullptr; valid until Pop.</summary>
        const T* Front() const noexcept
        {
            const std::int64_t r = read_.load(std::memory_order_relaxed);
            if (r == write_.load(std::memory_order_acquire))
                return nullptr;
            return &items_[r & (Capacity - 1)];
        }

        void Pop() noexcept { read_.store(read_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

        void Clear() noexcept
        {
            write_.store(0, std::memory_order_relaxed);
            read_.store(0, std::memory_order_relaxed);
        }

    private:
        T items_[Capacity] = {};
        alignas(64) std::atomic<std::int64_t> write_{0};
        alignas(64) std::atomic<std::int64_t> read_{0};
    };

    /// <summary>A Win32 handle closed at the end of its lifetime.</summary>
    class Handle
    {
    public:
        Handle() noexcept = default;
        explicit Handle(HANDLE h) noexcept : h_(h) {}
        Handle(const Handle&)            = delete;
        Handle& operator=(const Handle&) = delete;
        ~Handle() { Reset(); }

        void Reset(HANDLE h = nullptr) noexcept
        {
            if (h_ != nullptr && h_ != INVALID_HANDLE_VALUE)
                CloseHandle(h_);
            h_ = h;
        }
        HANDLE   Get() const noexcept { return h_; }
        explicit operator bool() const noexcept { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }

    private:
        HANDLE h_ = nullptr;
    };

    /// <summary>Releases a COM interface at the end of its lifetime.</summary>
    template <typename T>
    class ComPtr
    {
    public:
        ComPtr() noexcept                = default;
        ComPtr(const ComPtr&)            = delete;
        ComPtr& operator=(const ComPtr&) = delete;
        ~ComPtr() { Reset(); }

        void Reset() noexcept
        {
            if (p_ != nullptr)
                p_->Release();
            p_ = nullptr;
        }
        T**      Put() noexcept { return Reset(), &p_; }
        void**   PutVoid() noexcept { return reinterpret_cast<void**>(Put()); }
        T*       Get() const noexcept { return p_; }
        T*       operator->() const noexcept { return p_; }
        explicit operator bool() const noexcept { return p_ != nullptr; }

    private:
        T* p_ = nullptr;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::Gui
