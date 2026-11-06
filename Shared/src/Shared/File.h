// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstddef>
#include <cstdint>

#include "Text.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    /// <summary>
    /// Whole files at once (synchronous file I/O, in pieces of 1 GB), for what the samples do outside their task graphs:
    /// an in-memory mode, a check tool, the comparison of input and output path. Paths in the samples' character type
    /// (Char).
    /// <para>Preliminary.</para>
    /// </summary>
    class File
    {
    public:
        /// <summary>The size of the file in bytes; false if it cannot be opened.</summary>
        static bool Size(const Char* path, std::uint64_t& size) noexcept;

        /// <summary>Reads the first size bytes of the file into data; false on an error or if the file is shorter.</summary>
        static bool Read(const Char* path, void* data, std::size_t size) noexcept;

        /// <summary>Creates or replaces the file with size bytes of data; false on an error.</summary>
        static bool Write(const Char* path, const void* data, std::size_t size) noexcept;

        /// <summary>True if both paths name the same existing file (same volume or device and file ID, so also through links or other spellings).</summary>
        static bool Same(const Char* a, const Char* b) noexcept;

        /// <summary>Deletes the file; false if it could not be deleted.</summary>
        static bool Delete(const Char* path) noexcept;

        /// <summary>The longest path the samples build themselves, with the terminating zero (MAX_PATH, PATH_MAX).</summary>
#if defined(_WIN32)
        static constexpr std::int32_t MaxPath = 260;
#else
        static constexpr std::int32_t MaxPath = 4096;
#endif

        /// <summary>
        /// The text of an error code of the operating system (Windows: FormatMessage, Linux: strerror_r of an errno
        /// value), without the line end; empty if there is none.
        /// </summary>
        static void ErrorText(std::uint32_t error, Char* text, std::int32_t size) noexcept;

    private:
        static constexpr std::size_t Piece = std::size_t{1} << 30;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
