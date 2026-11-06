// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstddef>

#if defined(_WIN32)
#    include <Windows.h>
#else
#    include <cstdlib>
#    include <cstring>
#    include <sys/mman.h>
#endif

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    /// <summary>
    /// The process heap: HeapAlloc, HeapReAlloc and HeapFree on Windows, malloc, calloc, realloc and free on Linux. For
    /// the encoder libraries of the samples, which use neither the STL containers nor the modules of ThinkMeta.ConcurrentTasks.
    /// <para>Header only. Preliminary.</para>
    /// </summary>
    class Heap
    {
    public:
        /// <summary>A block of bytes (zeroed if zero is set); nullptr if out of memory.</summary>
        static void* Allocate(std::size_t bytes, bool zero = false) noexcept
        {
#if defined(_WIN32)
            return HeapAlloc(GetProcessHeap(), zero ? HEAP_ZERO_MEMORY : 0, bytes);
#else
            return zero ? std::calloc(1, bytes) : std::malloc(bytes);
#endif
        }

        /// <summary>
        /// The block of oldBytes resized to bytes, its contents kept, the new bytes zeroed if zero is set; nullptr if out
        /// of memory (the old block stays valid).
        /// </summary>
        static void* Reallocate(void* block, [[maybe_unused]] std::size_t oldBytes, std::size_t bytes, bool zero = false) noexcept
        {
#if defined(_WIN32)
            return HeapReAlloc(GetProcessHeap(), zero ? HEAP_ZERO_MEMORY : 0, block, bytes);
#else
            void* const resized = std::realloc(block, bytes);
            if (resized != nullptr && zero && bytes > oldBytes)
                std::memset(static_cast<char*>(resized) + oldBytes, 0, bytes - oldBytes);
            return resized;
#endif
        }

        /// <summary>Frees a block of Allocate or Reallocate (nullptr: nothing).</summary>
        static void Free(void* block) noexcept
        {
#if defined(_WIN32)
            if (block != nullptr)
                HeapFree(GetProcessHeap(), 0, block);
#else
            std::free(block);
#endif
        }
    };

    /// <summary>
    /// Address space reserved at once and committed in steps: VirtualAlloc and VirtualFree on Windows, mmap, mprotect and
    /// madvise on Linux. Committed pages read as zero.
    /// <para>Header only. Preliminary.</para>
    /// </summary>
    class AddressSpace
    {
    public:
        /// <summary>Reserves bytes of address space, nothing committed; nullptr if there is not enough.</summary>
        static void* Reserve(std::size_t bytes) noexcept
        {
#if defined(_WIN32)
            return VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_READWRITE);
#else
            void* const base = mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            return base == MAP_FAILED ? nullptr : base;
#endif
        }

        /// <summary>Reserves and commits bytes at once; nullptr if out of memory.</summary>
        static void* Allocate(std::size_t bytes) noexcept
        {
#if defined(_WIN32)
            return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
            void* const base = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            return base == MAP_FAILED ? nullptr : base;
#endif
        }

        /// <summary>Commits the pages of [address, address + bytes) for reading and writing; false if out of memory.</summary>
        static bool Commit(void* address, std::size_t bytes) noexcept
        {
#if defined(_WIN32)
            return VirtualAlloc(address, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
            return mprotect(address, bytes, PROT_READ | PROT_WRITE) == 0;
#endif
        }

        /// <summary>Returns the pages of [address, address + bytes) (whole pages) to the system; they stay reserved.</summary>
        static void Decommit(void* address, std::size_t bytes) noexcept
        {
#if defined(_WIN32)
            VirtualFree(address, bytes, MEM_DECOMMIT);
#else
            madvise(address, bytes, MADV_DONTNEED);
            mprotect(address, bytes, PROT_NONE);
#endif
        }

        /// <summary>Releases a range of Reserve or Allocate; bytes is its size (Linux needs it, Windows not).</summary>
        static void Release(void* base, [[maybe_unused]] std::size_t bytes) noexcept
        {
            if (base == nullptr)
                return;
#if defined(_WIN32)
            VirtualFree(base, 0, MEM_RELEASE);
#else
            munmap(base, bytes);
#endif
        }
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
