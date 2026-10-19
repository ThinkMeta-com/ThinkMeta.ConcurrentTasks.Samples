//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#pragma once

#include <cstddef>
#include <cstdint>

// The gzip framing of libdeflate_gzip_compress() (lib/gzip_compress.c): a fixed 10-byte header, the DEFLATE stream,
// then CRC-32 and the input size modulo 2^32.
namespace ThinkMeta::ConcurrentTasks::Samples::Deflate {

    constexpr std::size_t GzipHeaderSize  = 10;
    constexpr std::size_t GzipTrailerSize = 8;

    /// <summary>libdeflate: the header part of libdeflate_gzip_compress(). MTIME 0, XFL by level, OS 255; returns 10.</summary>
    inline std::size_t WriteGzipHeader(std::int32_t level, std::uint8_t* out) noexcept
    {
        out[0] = 0x1F;
        out[1] = 0x8B;
        out[2] = 8;                            // CM deflate
        out[3] = 0;                            // FLG
        out[4] = out[5] = out[6] = out[7] = 0; // MTIME unavailable
        out[8]                            = level < 2 ? 0x04 : level >= 8 ? 0x02 : 0x00;
        out[9]                            = 255; // OS unknown
        return GzipHeaderSize;
    }

    /// <summary>libdeflate: the trailer part of libdeflate_gzip_compress(): CRC-32 and ISIZE, little-endian; returns 8.</summary>
    inline std::size_t WriteGzipTrailer(std::uint32_t crc, std::size_t inSize, std::uint8_t* out) noexcept
    {
        const auto size = static_cast<std::uint32_t>(inSize);
        for (std::int32_t i = 0; i < 4; ++i) {
            out[i]     = static_cast<std::uint8_t>(crc >> (8 * i));
            out[4 + i] = static_cast<std::uint8_t>(size >> (8 * i));
        }
        return GzipTrailerSize;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Deflate
