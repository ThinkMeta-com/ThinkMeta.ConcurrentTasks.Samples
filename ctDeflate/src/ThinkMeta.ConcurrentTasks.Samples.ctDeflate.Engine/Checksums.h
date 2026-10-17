//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#pragma once

#include <cstddef>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    /// <summary>CRC-32 (gzip polynomial) continued from crc (0 for a new checksum), as libdeflate_crc32().</summary>
    std::uint32_t Crc32(std::uint32_t crc, const std::uint8_t* data, std::size_t size) noexcept;

    /// <summary>CRC-32 of the concatenation A || B from crcA, crcB and the length of B.</summary>
    std::uint32_t Crc32Combine(std::uint32_t crcA, std::uint32_t crcB, std::uint64_t sizeB) noexcept;

    /// <summary>Adler-32 continued from adler (1 for a new checksum), as libdeflate_adler32().</summary>
    std::uint32_t Adler32(std::uint32_t adler, const std::uint8_t* data, std::size_t size) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
