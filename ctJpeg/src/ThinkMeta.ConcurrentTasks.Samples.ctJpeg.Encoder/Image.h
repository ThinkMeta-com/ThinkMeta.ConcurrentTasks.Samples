// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
//
// Contains code ported from jpegli (google/jpegli @ 031a007), Copyright (c) the JPEG XL Project
// Authors, BSD 3-Clause License (see COPYING). jpegli's patent grant (jpeg/jpegli/PATENTS) covers
// jpegli itself; whether it extends to this port has not been assessed.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    /// <summary>
    /// An 8-bit input image, interleaved (RGB) or gray, as cjpegli reads it from PPM (P6) or PGM
    /// (P5) with maxval 255. Owns the file contents (a mapped view of the file, or a heap block);
    /// the pixels point into them.
    /// </summary>
    class Image
    {
    public:
        Image() noexcept = default;
        Image(Image&& other) noexcept;
        Image& operator=(Image&& other) noexcept;
        Image(const Image&)            = delete;
        Image& operator=(const Image&) = delete;
        ~Image() { Reset(); }

        std::int32_t Width() const noexcept { return width_; }
        std::int32_t Height() const noexcept { return height_; }
        std::int32_t Components() const noexcept { return components_; } // 1 gray, 3 RGB
        std::size_t  Stride() const noexcept { return static_cast<std::size_t>(width_) * static_cast<std::size_t>(components_); }

        /// <summary>The first byte of row y.</summary>
        const std::uint8_t* Row(std::int32_t y) const noexcept { return pixels_ + static_cast<std::size_t>(y) * Stride(); }

        explicit operator bool() const noexcept { return pixels_ != nullptr; }

        /// <summary>
        /// Maps a binary PNM file (P5 or P6, maxval 255, '#' comments in the header) into memory, so
        /// the pixel phase reads it from the file cache without a serial copy. On an error
        /// the image stays empty and error holds a message (at most errorSize characters).
        /// </summary>
        bool ReadPnm(const wchar_t* path, wchar_t* error, std::int32_t errorSize) noexcept;

        /// <summary>Parses PNM file contents already in memory; takes ownership of a HeapAlloc block.</summary>
        bool ParsePnm(std::uint8_t* data, std::size_t size, wchar_t* error, std::int32_t errorSize) noexcept;

        /// <summary>Parses PNM file contents owned by the caller, which keeps them alive (the pixels may still be arriving).</summary>
        bool ParsePnmView(const std::uint8_t* data, std::size_t size, wchar_t* error, std::int32_t errorSize) noexcept;

        /// <summary>Frees the contents.</summary>
        void Reset() noexcept;

    private:
        bool ParseHeader(const std::uint8_t* data, std::size_t size, wchar_t* error, std::int32_t errorSize) noexcept;

        std::uint8_t*       data_       = nullptr; // HeapAlloc block with the whole file (ParsePnm)
        const void*         view_       = nullptr; // mapped view of the file (ReadPnm)
        const std::uint8_t* pixels_     = nullptr;
        std::int32_t        width_      = 0;
        std::int32_t        height_     = 0;
        std::int32_t        components_ = 0;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
