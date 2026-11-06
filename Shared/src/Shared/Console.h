// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstdarg>
#include <cstdint>

#include "Text.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    /// <summary>The standard stream a text goes to.</summary>
    enum class Stream {
        Output, // stdout: help and version texts
        Error   // stderr: everything else
    };

    /// <summary>
    /// Text output of the samples, without the C runtime: printf-style (Formatter) to stdout or stderr, in the samples'
    /// character type (Char). Windows: a console gets the text as UTF-16 (WriteConsoleW), a file or pipe as UTF-8 with
    /// "\r\n" line ends, like the text mode of the C runtime; narrow arguments (%hs) are read as UTF-8. Linux: the text
    /// is UTF-8 already and goes out as it is (write). Every call writes at once, there is no buffer to flush.
    /// <para>Preliminary.</para>
    /// </summary>
    class Console
    {
    public:
        /// <summary>Formats like printf and writes the text to the stream.</summary>
        static void Print(Stream stream, const Char* format, ...) noexcept;

        /// <summary>Print with the arguments as a va_list.</summary>
        static void PrintV(Stream stream, const Char* format, std::va_list args) noexcept;

        /// <summary>Writes length characters of text to the stream, unformatted.</summary>
        static void Write(Stream stream, const Char* text, std::int32_t length) noexcept;

        /// <summary>
        /// Prints the message to stderr and ends the process with exit code 1 (ExitProcess or _exit: the C runtime does
        /// not shut down, threads of a scheduler end with the process).
        /// </summary>
        [[noreturn]] static void Fatal(const Char* format, ...) noexcept;

    private:
#if defined(_WIN32)
        static void* HandleOf(Stream stream) noexcept; // HANDLE
        static bool  IsConsole(void* handle) noexcept;
        static void  WriteConsoleText(void* handle, const wchar_t* text, std::int32_t length) noexcept;
        static void  WriteFileText(void* handle, const char* text, std::int32_t length) noexcept; // UTF-8
#endif
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
