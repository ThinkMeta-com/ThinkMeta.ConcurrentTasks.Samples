// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "Image.h"

#include <cstddef>
#include <cstdint>
#include <cwchar>

#if !defined(_WIN32)
#    include <cerrno>
#    include <fcntl.h>
#    include <sys/mman.h>
#    include <sys/stat.h>
#    include <unistd.h>
#endif

#include "Memory.h"

#include "Formatter.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    using Shared::Formatter;

    namespace {

        bool IsPnmSpace(std::uint8_t c) noexcept
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
        }

        // Skips whitespace and '#' comments (to the end of the line) in the header.
        void SkipSpace(const std::uint8_t* data, std::size_t size, std::size_t& pos) noexcept
        {
            while (pos < size) {
                if (IsPnmSpace(data[pos]))
                    ++pos;
                else if (data[pos] == '#') {
                    while (pos < size && data[pos] != '\n' && data[pos] != '\r')
                        ++pos;
                } else
                    return;
            }
        }

        // A decimal header value; false if there is none or it exceeds the limit.
        bool ReadValue(const std::uint8_t* data, std::size_t size, std::size_t& pos, std::int32_t limit, std::int32_t& value) noexcept
        {
            SkipSpace(data, size, pos);
            if (pos >= size || data[pos] < '0' || data[pos] > '9')
                return false;
            std::int64_t v = 0;
            while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
                v = v * 10 + (data[pos++] - '0');
                if (v > limit)
                    return false;
            }
            value = static_cast<std::int32_t>(v);
            return true;
        }

    } // namespace

    Image::Image(Image&& other) noexcept
        : data_(other.data_),
          view_(other.view_),
          viewSize_(other.viewSize_),
          pixels_(other.pixels_),
          width_(other.width_),
          height_(other.height_),
          components_(other.components_)
    {
        other.data_   = nullptr;
        other.view_   = nullptr;
        other.pixels_ = nullptr;
    }

    Image& Image::operator=(Image&& other) noexcept
    {
        if (this != &other) {
            Reset();
            data_         = other.data_;
            view_         = other.view_;
            viewSize_     = other.viewSize_;
            pixels_       = other.pixels_;
            width_        = other.width_;
            height_       = other.height_;
            components_   = other.components_;
            other.data_   = nullptr;
            other.view_   = nullptr;
            other.pixels_ = nullptr;
        }
        return *this;
    }

    void Image::Reset() noexcept
    {
        if (data_ != nullptr)
            Shared::Heap::Free(data_);
        if (view_ != nullptr) {
#if defined(_WIN32)
            UnmapViewOfFile(view_);
#else
            munmap(const_cast<void*>(view_), viewSize_);
#endif
        }
        data_       = nullptr;
        view_       = nullptr;
        viewSize_   = 0;
        pixels_     = nullptr;
        width_      = 0;
        height_     = 0;
        components_ = 0;
    }

    bool Image::ReadPnm(const Shared::Char* path, Shared::Char* error, std::int32_t errorSize) noexcept
    {
        Reset();
#if defined(_WIN32)
        const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            Formatter::Format(error, errorSize, CT_TEXT("cannot open '%s' (error %lu)"), path, GetLastError());
            return false;
        }
        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart <= 0) {
            CloseHandle(file);
            Formatter::Format(error, errorSize, CT_TEXT("'%s' is empty or unreadable"), path);
            return false;
        }
        const auto   size    = static_cast<std::size_t>(fileSize.QuadPart);
        const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        CloseHandle(file);
        const void* view = mapping != nullptr ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
        if (mapping != nullptr)
            CloseHandle(mapping); // the view keeps the mapping alive
        if (view == nullptr) {
            Formatter::Format(error, errorSize, CT_TEXT("cannot map '%s' (error %lu)"), path, GetLastError());
            return false;
        }
#else
        const int file = ::open(path, O_RDONLY | O_CLOEXEC);
        if (file < 0) {
            Formatter::Format(error, errorSize, CT_TEXT("cannot open '%s' (error %d)"), path, errno);
            return false;
        }
        struct stat status{};
        if (fstat(file, &status) != 0 || status.st_size <= 0) {
            ::close(file);
            Formatter::Format(error, errorSize, CT_TEXT("'%s' is empty or unreadable"), path);
            return false;
        }
        const auto  size   = static_cast<std::size_t>(status.st_size);
        void* const mapped = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, file, 0);
        const int   failed = errno;
        ::close(file); // the mapping keeps the file open
        if (mapped == MAP_FAILED) {
            Formatter::Format(error, errorSize, CT_TEXT("cannot map '%s' (error %d)"), path, failed);
            return false;
        }
        const void* view = mapped;
        viewSize_        = size;
#endif
        view_ = view;
        return ParseHeader(static_cast<const std::uint8_t*>(view), size, size, error, errorSize);
    }

    bool Image::ParsePnm(std::uint8_t* data, std::size_t size, Shared::Char* error, std::int32_t errorSize) noexcept
    {
        Reset();
        data_ = data;
        return ParseHeader(data, size, size, error, errorSize);
    }

    bool Image::ParsePnmView(const std::uint8_t* data, std::size_t available, std::size_t size, Shared::Char* error, std::int32_t errorSize) noexcept
    {
        Reset();
        return ParseHeader(data, available, size, error, errorSize);
    }

    // The header from the first available bytes (available <= size), the pixels against the whole size.
    bool Image::ParseHeader(const std::uint8_t* data, std::size_t available, std::size_t size, Shared::Char* error, std::int32_t errorSize) noexcept
    {
        if (available < 2 || data[0] != 'P' || (data[1] != '5' && data[1] != '6')) {
            Reset();
            Formatter::Format(error, errorSize, CT_TEXT("not a binary PGM/PPM file (P5/P6)"));
            return false;
        }
        const std::int32_t components = data[1] == '6' ? 3 : 1;
        std::size_t        pos        = 2;
        std::int32_t       width = 0, height = 0, maxval = 0;
        // JPEG_MAX_DIMENSION of jpegli is 65500
        if (!ReadValue(data, available, pos, 65500, width) || !ReadValue(data, available, pos, 65500, height)
            || !ReadValue(data, available, pos, 65535, maxval)) {
            Reset();
            Formatter::Format(error, errorSize, CT_TEXT("invalid PNM header"));
            return false;
        }
        if (width < 1 || height < 1) {
            Reset();
            Formatter::Format(error, errorSize, CT_TEXT("empty image"));
            return false;
        }
        if (maxval != 255) {
            Reset();
            Formatter::Format(error, errorSize, CT_TEXT("only 8-bit PNM (maxval 255) is supported, not maxval %d"), maxval);
            return false;
        }
        // exactly one whitespace character ends the header
        if (pos >= available || !IsPnmSpace(data[pos])) {
            Reset();
            Formatter::Format(error, errorSize, CT_TEXT("invalid PNM header"));
            return false;
        }
        ++pos;
        const std::size_t bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * static_cast<std::size_t>(components);
        if (size - pos < bytes) {
            Reset();
            Formatter::Format(error, errorSize, CT_TEXT("PNM file is truncated"));
            return false;
        }
        pixels_     = data + pos;
        width_      = width;
        height_     = height;
        components_ = components;
        return true;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
