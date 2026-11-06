// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "File.h"

#if defined(_WIN32)
#    include <Windows.h>
#else
#    include <cerrno>
#    include <cstring>
#    include <fcntl.h>
#    include <sys/stat.h>
#    include <unistd.h>
#endif

#include <cstddef>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

#if defined(_WIN32)

    bool File::Size(const Char* path, std::uint64_t& size) noexcept
    {
        WIN32_FILE_ATTRIBUTE_DATA attributes;
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attributes))
            return false;
        size = static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32 | attributes.nFileSizeLow;
        return true;
    }

    bool File::Read(const Char* path, void* data, std::size_t size) noexcept
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

    bool File::Write(const Char* path, const void* data, std::size_t size) noexcept
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

    bool File::Same(const Char* a, const Char* b) noexcept
    {
        BY_HANDLE_FILE_INFORMATION info[2];
        const Char*                paths[2] = {a, b};
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

    bool File::Delete(const Char* path) noexcept
    {
        return DeleteFileW(path) != 0;
    }

    void File::ErrorText(std::uint32_t error, Char* text, std::int32_t size) noexcept
    {
        if (size <= 0)
            return;
        unsigned long length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, text,
                                              static_cast<unsigned long>(size), nullptr); // DWORD of FormatMessageW
        while (length > 0 && (text[length - 1] == L'\n' || text[length - 1] == L'\r' || text[length - 1] == L' '))
            --length;
        text[length] = 0;
    }

#else

    bool File::Size(const Char* path, std::uint64_t& size) noexcept
    {
        struct stat status;
        if (::stat(path, &status) != 0)
            return false;
        size = static_cast<std::uint64_t>(status.st_size);
        return true;
    }

    bool File::Read(const Char* path, void* data, std::size_t size) noexcept
    {
        const int file = ::open(path, O_RDONLY | O_CLOEXEC);
        if (file < 0)
            return false;
        ::posix_fadvise(file, 0, 0, POSIX_FADV_SEQUENTIAL);
        auto*       bytes = static_cast<std::uint8_t*>(data);
        std::size_t done  = 0;
        while (done < size) {
            const ssize_t n = ::read(file, bytes + done, size - done < Piece ? size - done : Piece);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break; // an error, or the file is shorter
            done += static_cast<std::size_t>(n);
        }
        ::close(file);
        return done == size;
    }

    bool File::Write(const Char* path, const void* data, std::size_t size) noexcept
    {
        const int file = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (file < 0)
            return false;
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        std::size_t done  = 0;
        while (done < size) {
            const ssize_t n = ::write(file, bytes + done, size - done < Piece ? size - done : Piece);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            done += static_cast<std::size_t>(n);
        }
        return ::close(file) == 0 && done == size;
    }

    bool File::Same(const Char* a, const Char* b) noexcept
    {
        struct stat first, second;
        return ::stat(a, &first) == 0 && ::stat(b, &second) == 0 && first.st_dev == second.st_dev && first.st_ino == second.st_ino;
    }

    bool File::Delete(const Char* path) noexcept
    {
        return ::unlink(path) == 0;
    }

    void File::ErrorText(std::uint32_t error, Char* text, std::int32_t size) noexcept
    {
        if (size <= 0)
            return;
        // GNU strerror_r: returns the text, which may or may not be in the buffer
        const char* message = ::strerror_r(static_cast<int>(error), text, static_cast<std::size_t>(size));
        Text::Copy(text, static_cast<std::size_t>(size), message != nullptr ? message : "");
    }

#endif

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
