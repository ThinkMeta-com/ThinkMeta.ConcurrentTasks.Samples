// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "Console.h"

#if defined(_WIN32)
#    include <Windows.h>
#else
#    include <cerrno>
#    include <cstdlib>
#    include <unistd.h>
#endif

#include <cstdarg>
#include <cstddef>
#include <cstdint>

#include "Formatter.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    void Console::Print(Stream stream, const Char* format, ...) noexcept
    {
        std::va_list args;
        va_start(args, format);
        PrintV(stream, format, args);
        va_end(args);
    }

    void Console::PrintV(Stream stream, const Char* format, std::va_list args) noexcept
    {
        // most texts fit on the stack; a longer one (a help text) is formatted again into a heap buffer
        constexpr std::int32_t LocalSize = 512;
        Char                   local[LocalSize];
        std::va_list           copy;
        va_copy(copy, args);
        const std::int32_t length = Formatter::FormatV(local, LocalSize, format, args);
        if (length < LocalSize) {
            Write(stream, local, length);
        } else {
            const std::size_t bytes = (static_cast<std::size_t>(length) + 1) * sizeof(Char);
#if defined(_WIN32)
            auto* text = static_cast<Char*>(HeapAlloc(GetProcessHeap(), 0, bytes));
#else
            auto* text = static_cast<Char*>(std::malloc(bytes));
#endif
            if (text != nullptr) {
                Formatter::FormatV(text, length + 1, format, copy);
                Write(stream, text, length);
#if defined(_WIN32)
                HeapFree(GetProcessHeap(), 0, text);
#else
                std::free(text);
#endif
            }
        }
        va_end(copy);
    }

    void Console::Fatal(const Char* format, ...) noexcept
    {
        std::va_list args;
        va_start(args, format);
        PrintV(Stream::Error, format, args);
        va_end(args);
#if defined(_WIN32)
        ExitProcess(1);
#else
        _exit(1);
#endif
    }

#if defined(_WIN32)

    void Console::Write(Stream stream, const Char* text, std::int32_t length) noexcept
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

#else

    // UTF-8 as it is, whether the stream is a terminal, a file or a pipe
    void Console::Write(Stream stream, const Char* text, std::int32_t length) noexcept
    {
        const int descriptor = stream == Stream::Output ? STDOUT_FILENO : STDERR_FILENO;
        while (length > 0) {
            const ssize_t written = ::write(descriptor, text, static_cast<std::size_t>(length));
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                return;
            text += written;
            length -= static_cast<std::int32_t>(written);
        }
    }

#endif

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
