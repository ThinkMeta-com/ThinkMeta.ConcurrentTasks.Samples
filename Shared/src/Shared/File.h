// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstddef>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    /// <summary>
    /// Whole files at once (synchronous Windows file I/O, in pieces of 1 GB), for what the samples do outside their
    /// task graphs: an in-memory mode, a check tool, the comparison of input and output path.
    /// <para>Preliminary.</para>
    /// </summary>
    class File
    {
    public:
        /// <summary>The size of the file in bytes; false if it cannot be opened.</summary>
        static bool Size(const wchar_t* path, std::uint64_t& size) noexcept;

        /// <summary>Reads the first size bytes of the file into data; false on an error or if the file is shorter.</summary>
        static bool Read(const wchar_t* path, void* data, std::size_t size) noexcept;

        /// <summary>Creates or replaces the file with size bytes of data; false on an error.</summary>
        static bool Write(const wchar_t* path, const void* data, std::size_t size) noexcept;

        /// <summary>True if both paths name the same existing file (same volume and file ID, so also through links or other spellings).</summary>
        static bool Same(const wchar_t* a, const wchar_t* b) noexcept;

        /// <summary>The text of a Windows error code (FormatMessage), without the line end; empty if there is none.</summary>
        static void ErrorText(std::uint32_t error, wchar_t* text, std::int32_t size) noexcept;

    private:
        static constexpr std::size_t Piece = std::size_t{1} << 30;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
