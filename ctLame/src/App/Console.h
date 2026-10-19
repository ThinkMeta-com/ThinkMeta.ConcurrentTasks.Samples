// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <cstdarg>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    // Port of frontend/console.c for Windows. Console messages and errors go to stderr, help texts
    // to stdout, reports nowhere. Text is formatted as UTF-16: a console gets it through
    // WriteConsoleW, a file or pipe as UTF-8 with "\r\n" line ends, like the CRT's text mode.

    enum class Stream {
        Output, // stdout
        Error   // stderr (Console_IO.Console_fp and Console_IO.Error_fp)
    };

    void OpenConsole() noexcept;
    void CloseConsole() noexcept;

    std::int32_t Print(Stream stream, const wchar_t* format, ...);
    std::int32_t PrintV(Stream stream, const wchar_t* format, std::va_list args);

    std::int32_t ConsolePrintf(const wchar_t* format, ...);
    std::int32_t ErrorPrintf(const wchar_t* format, ...);

    /// <summary>Report functions for the encoder (frontend_msgf, frontend_debugf, frontend_errorf).</summary>
    void FrontendMessage(const char* format, std::va_list args);
    void FrontendDebug(const char* format, std::va_list args);
    void FrontendError(const char* format, std::va_list args);

    /// <summary>Error report function for messages with UTF-16 text (paths).</summary>
    void FrontendErrorW(const wchar_t* format, std::va_list args);

    void Flush(Stream stream) noexcept;

    std::int32_t ConsoleGetWidth() noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
