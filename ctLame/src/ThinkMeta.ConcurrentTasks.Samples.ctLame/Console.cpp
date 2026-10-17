// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Console.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    namespace {

        // Like LAME's console stream (setvbuf with 2048 bytes): output is collected and written
        // when the buffer is full, on Flush and when the console is closed.
        struct Channel {
            HANDLE                    handle    = nullptr;
            bool                      isConsole = false;
            std::int32_t              used      = 0;
            std::array<wchar_t, 2048> buffer;
        };

        struct ConsoleIo {
            bool                   initialized = false;
            std::int32_t           dispWidth   = 80;
            std::array<Channel, 2> channels;
        };

        ConsoleIo console;

        Channel& ChannelOf(Stream stream) noexcept
        {
            return console.channels[stream == Stream::Output ? 0 : 1];
        }

        void WriteRedirected(HANDLE handle, const wchar_t* text, std::int32_t length) noexcept
        {
            // text mode: "\n" becomes "\r\n"; then UTF-8
            std::array<wchar_t, 2 * 2048>  translated;
            std::array<char, 3 * 2 * 2048> bytes;
            while (length > 0) {
                std::int32_t used = 0;
                std::int32_t n    = 0;
                while (n < length && used < 2 * 2048 - 1) {
                    if (text[n] == L'\n')
                        translated[used++] = L'\r';
                    translated[used++] = text[n++];
                }
                const std::int32_t size
                    = WideCharToMultiByte(CP_UTF8, 0, translated.data(), used, bytes.data(), static_cast<int>(bytes.size()), nullptr, nullptr);
                unsigned long written; // DWORD of WriteFile
                if (size > 0)
                    WriteFile(handle, bytes.data(), static_cast<std::uint32_t>(size), &written, nullptr);
                text += n;
                length -= n;
            }
        }

        void Write(Channel& channel, const wchar_t* text, std::int32_t length) noexcept
        {
            if (length <= 0 || channel.handle == nullptr || channel.handle == INVALID_HANDLE_VALUE)
                return;
            if (channel.isConsole) {
                unsigned long written; // DWORD of WriteConsoleW
                WriteConsoleW(channel.handle, text, static_cast<std::uint32_t>(length), &written, nullptr);
            } else {
                WriteRedirected(channel.handle, text, length);
            }
        }

        void FlushChannel(Channel& channel) noexcept
        {
            Write(channel, channel.buffer.data(), channel.used);
            channel.used = 0;
        }

        void Append(Channel& channel, const wchar_t* text, std::int32_t length) noexcept
        {
            if (channel.used + length > static_cast<std::int32_t>(channel.buffer.size()))
                FlushChannel(channel);
            if (length > static_cast<std::int32_t>(channel.buffer.size())) {
                Write(channel, text, length);
                return;
            }
            for (std::int32_t i = 0; i < length; ++i)
                channel.buffer[channel.used++] = text[i];
        }

        void OpenChannel(Channel& channel, std::uint32_t stdHandle) noexcept
        {
            unsigned long mode = 0; // DWORD of GetConsoleMode
            channel.handle     = GetStdHandle(stdHandle);
            channel.isConsole  = channel.handle != nullptr && channel.handle != INVALID_HANDLE_VALUE && GetConsoleMode(channel.handle, &mode);
            channel.used       = 0;
        }

        // A narrow report message: LAME's library only prints ASCII, so widening is enough.
        void PrintNarrow(Stream stream, const char* format, std::va_list args)
        {
            std::array<char, 4096>    narrow;
            std::array<wchar_t, 4096> wide;
            const std::int32_t        length = std::vsnprintf(narrow.data(), narrow.size(), format, args);
            const std::int32_t        used
                = length < 0 ? 0 : (length < static_cast<std::int32_t>(sizeof(narrow)) ? length : static_cast<std::int32_t>(sizeof(narrow)) - 1);
            const std::int32_t size = MultiByteToWideChar(CP_ACP, 0, narrow.data(), used, wide.data(), static_cast<int>(wide.size()));
            if (console.initialized)
                Append(ChannelOf(stream), wide.data(), size);
        }

    } // namespace

    void OpenConsole() noexcept
    {
        if (console.initialized)
            return;
        console.dispWidth = 80;
        OpenChannel(console.channels[0], STD_OUTPUT_HANDLE);
        OpenChannel(console.channels[1], STD_ERROR_HANDLE);
        console.initialized = true;
    }

    void CloseConsole() noexcept
    {
        if (!console.initialized)
            return;
        FlushChannel(console.channels[0]);
        FlushChannel(console.channels[1]);
        console.initialized = false;
    }

    std::int32_t PrintV(Stream stream, const wchar_t* format, std::va_list args)
    {
        if (!console.initialized)
            return 0;

        std::array<wchar_t, 4096> local;
        wchar_t*                  text = local.data();
        std::va_list              copy;
        va_copy(copy, args);
        const std::int32_t length = _vscwprintf(format, copy);
        va_end(copy);
        if (length < 0)
            return 0;
        if (length >= static_cast<std::int32_t>(local.size())) {
            text = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), 0, (static_cast<std::size_t>(length) + 1) * sizeof(wchar_t)));
            if (text == nullptr)
                return 0;
        }
        _vsnwprintf_s(text, static_cast<std::size_t>(length) + 1, _TRUNCATE, format, args);
        Append(ChannelOf(stream), text, length);
        if (text != local.data())
            HeapFree(GetProcessHeap(), 0, text);
        return length;
    }

    std::int32_t Print(Stream stream, const wchar_t* format, ...)
    {
        std::va_list args;
        va_start(args, format);
        const std::int32_t ret = PrintV(stream, format, args);
        va_end(args);
        return ret;
    }

    std::int32_t ConsolePrintf(const wchar_t* format, ...)
    {
        std::va_list args;
        va_start(args, format);
        const std::int32_t ret = PrintV(Stream::Error, format, args);
        va_end(args);
        return ret;
    }

    std::int32_t ErrorPrintf(const wchar_t* format, ...)
    {
        std::va_list args;
        va_start(args, format);
        const std::int32_t ret = PrintV(Stream::Error, format, args);
        va_end(args);
        return ret;
    }

    void FrontendMessage(const char* format, std::va_list args)
    {
        PrintNarrow(Stream::Error, format, args);
    }

    void FrontendDebug(const char*, std::va_list)
    {} // the report file is never opened

    void FrontendError(const char* format, std::va_list args)
    {
        PrintNarrow(Stream::Error, format, args);
    }

    void FrontendErrorW(const wchar_t* format, std::va_list args)
    {
        (void)PrintV(Stream::Error, format, args);
    }

    void Flush(Stream stream) noexcept
    {
        if (console.initialized)
            FlushChannel(ChannelOf(stream));
    }

    std::int32_t ConsoleGetWidth() noexcept
    {
        return console.initialized ? console.dispWidth : 80;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
