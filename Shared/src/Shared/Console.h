// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstdarg>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    /// <summary>The standard stream a text goes to.</summary>
    enum class Stream {
        Output, // stdout: help and version texts
        Error   // stderr: everything else
    };

    /// <summary>
    /// Text output of the samples, without the C runtime: printf-style (Formatter) to stdout or stderr. A console gets
    /// the text as UTF-16 (WriteConsoleW), a file or pipe as UTF-8 with "\r\n" line ends, like the text mode of the C
    /// runtime. The text is UTF-16 throughout; narrow arguments (%hs) are read as UTF-8. Every call writes at once, there is no buffer to flush.
    /// <para>Preliminary.</para>
    /// </summary>
    class Console
    {
    public:
        /// <summary>Formats like printf and writes the text to the stream.</summary>
        static void Print(Stream stream, const wchar_t* format, ...) noexcept;

        /// <summary>Print with the arguments as a va_list.</summary>
        static void PrintV(Stream stream, const wchar_t* format, std::va_list args) noexcept;

        /// <summary>Writes length characters of text to the stream, unformatted.</summary>
        static void Write(Stream stream, const wchar_t* text, std::int32_t length) noexcept;

        /// <summary>
        /// Prints the message to stderr and ends the process with exit code 1 (ExitProcess: the C runtime does not shut
        /// down, threads of a scheduler end with the process).
        /// </summary>
        [[noreturn]] static void Fatal(const wchar_t* format, ...) noexcept;

    private:
        static void* HandleOf(Stream stream) noexcept; // HANDLE
        static bool  IsConsole(void* handle) noexcept;
        static void  WriteConsoleText(void* handle, const wchar_t* text, std::int32_t length) noexcept;
        static void  WriteFileText(void* handle, const char* text, std::int32_t length) noexcept; // UTF-8
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
