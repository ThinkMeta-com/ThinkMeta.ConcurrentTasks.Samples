// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "Console.h"

#include <Windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    namespace {

        constexpr int TextSize = 8192;

        HANDLE HandleOf(Stream stream) noexcept
        {
            return GetStdHandle(stream == Stream::Output ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
        }

        void Write(Stream stream, const char* text, int length) noexcept
        {
            HANDLE handle = HandleOf(stream);
            if (handle == nullptr || handle == INVALID_HANDLE_VALUE || length <= 0)
                return;
            unsigned long mode = 0; // DWORD of GetConsoleMode
            if (GetConsoleMode(handle, &mode)) {
                wchar_t       wide[TextSize];
                const int     count   = MultiByteToWideChar(CP_UTF8, 0, text, length, wide, TextSize);
                unsigned long written = 0; // DWORD of WriteConsoleW
                if (count > 0)
                    WriteConsoleW(handle, wide, static_cast<unsigned long>(count), &written, nullptr);
                return;
            }
            // file or pipe: UTF-8 with CRLF line ends like the CRT text mode
            char out[2 * TextSize];
            int  n = 0;
            for (int i = 0; i < length; ++i) {
                if (text[i] == '\n')
                    out[n++] = '\r';
                out[n++] = text[i];
            }
            unsigned long written = 0; // DWORD of WriteFile
            WriteFile(handle, out, static_cast<unsigned long>(n), &written, nullptr);
        }

    } // namespace

    void PrintV(Stream stream, const char* format, std::va_list args)
    {
        char      text[TextSize];
        const int length = std::vsnprintf(text, sizeof(text), format, args);
        if (length > 0)
            Write(stream, text, length < TextSize ? length : TextSize - 1);
    }

    void Print(Stream stream, const char* format, ...)
    {
        std::va_list args;
        va_start(args, format);
        PrintV(stream, format, args);
        va_end(args);
    }

    void ErrorPrintf(const char* format, ...)
    {
        std::va_list args;
        va_start(args, format);
        PrintV(Stream::Error, format, args);
        va_end(args);
    }

    void ReportToStderr(const char* format, std::va_list args)
    {
        PrintV(Stream::Error, format, args);
    }

    void Fatal(const char* format, ...)
    {
        std::va_list args;
        va_start(args, format);
        PrintV(Stream::Error, format, args);
        va_end(args);
        std::exit(1);
    }

    void Flush(Stream stream) noexcept
    {
        FlushFileBuffers(HandleOf(stream));
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
