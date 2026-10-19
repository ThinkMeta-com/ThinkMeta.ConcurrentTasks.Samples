// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "File.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    bool File::Size(const wchar_t* path, std::uint64_t& size) noexcept
    {
        WIN32_FILE_ATTRIBUTE_DATA attributes;
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attributes))
            return false;
        size = static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32 | attributes.nFileSizeLow;
        return true;
    }

    bool File::Read(const wchar_t* path, void* data, std::size_t size) noexcept
    {
        HANDLE const file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        auto*       bytes = static_cast<std::uint8_t*>(data);
        bool        ok    = true;
        std::size_t done  = 0;
        while (ok && done < size) {
            const auto    chunk = static_cast<unsigned long>(size - done < Piece ? size - done : Piece);
            unsigned long read  = 0; // DWORD of ReadFile
            ok                  = ReadFile(file, bytes + done, chunk, &read, nullptr) != 0 && read == chunk;
            done += read;
        }
        CloseHandle(file);
        return ok;
    }

    bool File::Write(const wchar_t* path, const void* data, std::size_t size) noexcept
    {
        HANDLE const file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        bool        ok    = true;
        std::size_t done  = 0;
        while (ok && done < size) {
            const auto    chunk   = static_cast<unsigned long>(size - done < Piece ? size - done : Piece);
            unsigned long written = 0; // DWORD of WriteFile
            ok                    = WriteFile(file, bytes + done, chunk, &written, nullptr) != 0 && written == chunk;
            done += written;
        }
        return CloseHandle(file) != 0 && ok;
    }

    bool File::Same(const wchar_t* a, const wchar_t* b) noexcept
    {
        BY_HANDLE_FILE_INFORMATION info[2];
        const wchar_t*             paths[2] = {a, b};
        for (std::int32_t i = 0; i < 2; ++i) {
            HANDLE const file
                = CreateFileW(paths[i], 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;
            const bool ok = GetFileInformationByHandle(file, &info[i]) != 0;
            CloseHandle(file);
            if (!ok)
                return false;
        }
        return info[0].dwVolumeSerialNumber == info[1].dwVolumeSerialNumber && info[0].nFileIndexHigh == info[1].nFileIndexHigh
            && info[0].nFileIndexLow == info[1].nFileIndexLow;
    }

    void File::ErrorText(std::uint32_t error, wchar_t* text, std::int32_t size) noexcept
    {
        if (size <= 0)
            return;
        unsigned long length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, text,
                                              static_cast<unsigned long>(size), nullptr); // DWORD of FormatMessageW
        while (length > 0 && (text[length - 1] == L'\n' || text[length - 1] == L'\r' || text[length - 1] == L' '))
            --length;
        text[length] = 0;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
