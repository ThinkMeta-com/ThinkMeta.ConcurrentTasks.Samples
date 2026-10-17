// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#include "Image.h"

#include <cstddef>
#include <cstdint>
#include <cwchar>

#include <Windows.h>

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

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
            HeapFree(GetProcessHeap(), 0, data_);
        if (view_ != nullptr)
            UnmapViewOfFile(view_);
        data_       = nullptr;
        view_       = nullptr;
        pixels_     = nullptr;
        width_      = 0;
        height_     = 0;
        components_ = 0;
    }

    bool Image::ReadPnm(const wchar_t* path, wchar_t* error, std::int32_t errorSize) noexcept
    {
        Reset();
        const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            swprintf_s(error, static_cast<std::size_t>(errorSize), L"cannot open '%ls' (error %lu)", path, GetLastError());
            return false;
        }
        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart <= 0) {
            CloseHandle(file);
            swprintf_s(error, static_cast<std::size_t>(errorSize), L"'%ls' is empty or unreadable", path);
            return false;
        }
        const auto   size    = static_cast<std::size_t>(fileSize.QuadPart);
        const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        CloseHandle(file);
        const void* view = mapping != nullptr ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
        if (mapping != nullptr)
            CloseHandle(mapping); // the view keeps the mapping alive
        if (view == nullptr) {
            swprintf_s(error, static_cast<std::size_t>(errorSize), L"cannot map '%ls' (error %lu)", path, GetLastError());
            return false;
        }
        view_ = view;
        return ParseHeader(static_cast<const std::uint8_t*>(view), size, error, errorSize);
    }

    bool Image::ParsePnm(std::uint8_t* data, std::size_t size, wchar_t* error, std::int32_t errorSize) noexcept
    {
        Reset();
        data_ = data;
        return ParseHeader(data, size, error, errorSize);
    }

    bool Image::ParsePnmView(const std::uint8_t* data, std::size_t size, wchar_t* error, std::int32_t errorSize) noexcept
    {
        Reset();
        return ParseHeader(data, size, error, errorSize);
    }

    bool Image::ParseHeader(const std::uint8_t* data, std::size_t size, wchar_t* error, std::int32_t errorSize) noexcept
    {
        if (size < 2 || data[0] != 'P' || (data[1] != '5' && data[1] != '6')) {
            Reset();
            swprintf_s(error, static_cast<std::size_t>(errorSize), L"not a binary PGM/PPM file (P5/P6)");
            return false;
        }
        const std::int32_t components = data[1] == '6' ? 3 : 1;
        std::size_t        pos        = 2;
        std::int32_t       width = 0, height = 0, maxval = 0;
        // JPEG_MAX_DIMENSION of jpegli is 65500
        if (!ReadValue(data, size, pos, 65500, width) || !ReadValue(data, size, pos, 65500, height) || !ReadValue(data, size, pos, 65535, maxval)) {
            Reset();
            swprintf_s(error, static_cast<std::size_t>(errorSize), L"invalid PNM header");
            return false;
        }
        if (width < 1 || height < 1) {
            Reset();
            swprintf_s(error, static_cast<std::size_t>(errorSize), L"empty image");
            return false;
        }
        if (maxval != 255) {
            Reset();
            swprintf_s(error, static_cast<std::size_t>(errorSize), L"only 8-bit PNM (maxval 255) is supported, not maxval %d", maxval);
            return false;
        }
        // exactly one whitespace character ends the header
        if (pos >= size || !IsPnmSpace(data[pos])) {
            Reset();
            swprintf_s(error, static_cast<std::size_t>(errorSize), L"invalid PNM header");
            return false;
        }
        ++pos;
        const std::size_t bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * static_cast<std::size_t>(components);
        if (size - pos < bytes) {
            Reset();
            swprintf_s(error, static_cast<std::size_t>(errorSize), L"PNM file is truncated");
            return false;
        }
        pixels_     = data + pos;
        width_      = width;
        height_     = height;
        components_ = components;
        return true;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
