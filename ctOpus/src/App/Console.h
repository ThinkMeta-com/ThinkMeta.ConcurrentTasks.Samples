// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstdarg>

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    // opusenc's console output on Windows: printf-style UTF-8 text. A console gets it as UTF-16
    // through WriteConsoleW, a file or pipe as UTF-8 with "\r\n" line ends (the CRT's text mode).

    enum class Stream {
        Output, // stdout: usage and version
        Error   // stderr: everything else
    };

    void Print(Stream stream, const char* format, ...);
    void PrintV(Stream stream, const char* format, std::va_list args);

    /// <summary>Prints to stderr (fprintf(stderr, ...)).</summary>
    void ErrorPrintf(const char* format, ...);

    /// <summary>ReportFunction for the library: to stderr.</summary>
    void ReportToStderr(const char* format, std::va_list args);

    /// <summary>fatal(): prints to stderr and exits with code 1.</summary>
    [[noreturn]] void Fatal(const char* format, ...);

    void Flush(Stream stream) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
