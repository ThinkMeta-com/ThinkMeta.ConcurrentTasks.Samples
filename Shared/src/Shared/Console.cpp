// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "Console.h"

#include <Windows.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>

#include "Formatter.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    void Console::Print(Stream stream, const wchar_t* format, ...) noexcept
    {
        std::va_list args;
        va_start(args, format);
        PrintV(stream, format, args);
        va_end(args);
    }

    void Console::PrintV(Stream stream, const wchar_t* format, std::va_list args) noexcept
    {
        // most texts fit on the stack; a longer one (a help text) is formatted again into a heap buffer
        constexpr std::int32_t LocalSize = 512;
        wchar_t                local[LocalSize];
        std::va_list           copy;
        va_copy(copy, args);
        const std::int32_t length = Formatter::FormatV(local, LocalSize, format, args);
        if (length < LocalSize) {
            Write(stream, local, length);
        } else if (auto* text = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), 0, (static_cast<std::size_t>(length) + 1) * sizeof(wchar_t)))) {
            Formatter::FormatV(text, length + 1, format, copy);
            Write(stream, text, length);
            HeapFree(GetProcessHeap(), 0, text);
        }
        va_end(copy);
    }

    void Console::Write(Stream stream, const wchar_t* text, std::int32_t length) noexcept
    {
        void* const handle = HandleOf(stream);
        if (handle == nullptr || length <= 0)
            return;
        if (IsConsole(handle)) {
            WriteConsoleText(handle, text, length);
            return;
        }
        // to UTF-8 in pieces that do not split a surrogate pair
        constexpr std::int32_t Piece = 256;
        char                   bytes[3 * Piece];
        while (length > 0) {
            std::int32_t n = length < Piece ? length : Piece;
            if (n < length && text[n - 1] >= 0xD800 && text[n - 1] < 0xDC00)
                --n;
            const int size = WideCharToMultiByte(CP_UTF8, 0, text, n, bytes, static_cast<int>(sizeof(bytes)), nullptr, nullptr);
            WriteFileText(handle, bytes, size);
            text += n;
            length -= n;
        }
    }

    void Console::Fatal(const wchar_t* format, ...) noexcept
    {
        std::va_list args;
        va_start(args, format);
        PrintV(Stream::Error, format, args);
        va_end(args);
        ExitProcess(1);
    }

    void* Console::HandleOf(Stream stream) noexcept
    {
        HANDLE const handle = GetStdHandle(stream == Stream::Output ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
        return handle == INVALID_HANDLE_VALUE ? nullptr : handle;
    }

    bool Console::IsConsole(void* handle) noexcept
    {
        unsigned long mode = 0; // DWORD of GetConsoleMode
        return GetConsoleMode(handle, &mode) != 0;
    }

    void Console::WriteConsoleText(void* handle, const wchar_t* text, std::int32_t length) noexcept
    {
        while (length > 0) {
            unsigned long written = 0; // DWORD of WriteConsoleW
            if (!WriteConsoleW(handle, text, static_cast<unsigned long>(length), &written, nullptr) || written == 0)
                return;
            text += written;
            length -= static_cast<std::int32_t>(written);
        }
    }

    // "\n" becomes "\r\n" (the text mode of the C runtime)
    void Console::WriteFileText(void* handle, const char* text, std::int32_t length) noexcept
    {
        char         out[1024];
        std::int32_t used = 0;
        for (std::int32_t i = 0; i < length; ++i) {
            if (used >= static_cast<std::int32_t>(sizeof(out)) - 2) {
                unsigned long written = 0; // DWORD of WriteFile
                WriteFile(handle, out, static_cast<unsigned long>(used), &written, nullptr);
                used = 0;
            }
            if (text[i] == '\n')
                out[used++] = '\r';
            out[used++] = text[i];
        }
        if (used > 0) {
            unsigned long written = 0; // DWORD of WriteFile
            WriteFile(handle, out, static_cast<unsigned long>(used), &written, nullptr);
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
