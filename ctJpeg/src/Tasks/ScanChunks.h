// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "ByteBuffer.h"
#include "EncoderSetup.h"
#include "Entropy.h"
#include "TokenBands.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    /// <summary>
    /// The edges between the nodes Count, Write, Splice and Assemble (EncodeGraph.h): a banded
    /// scan is written in chunks, one per band (AC: its prefix and band part, the last chunk also
    /// the final part; DC: its band part), as plain bit streams that Splice joins into stuffed
    /// bytes; every other scan is written whole into its scan data. For the chunks of a refinement
    /// scan, Count counts the correction bits and run lengths each chunk reads, and PlaceCursors
    /// turns the counts into the place where each chunk starts reading.
    /// </summary>
    class ScanChunks
    {
    public:
        /// <summary>A write job: a whole scan (chunk -1) or one chunk of a banded scan.</summary>
        struct Job {
            std::int32_t scan, chunk, partBegin, partEnd;
            std::int64_t work;
        };

        /// <summary>Plans the write jobs, largest first, from the bands of the tokenization; false if out of memory.</summary>
        bool Plan(const EncoderSetup& setup, const TokenBands& bands) noexcept;

        /// <summary>The cursors of the chunks of every refinement scan, from the counts of Count (serial, cheap).</summary>
        void PlaceCursors(const EncoderSetup& setup, const ScanTokens* scanTokens) noexcept;

        std::unique_ptr<Job[]> jobs;
        std::int32_t           jobCount = 0;

        std::array<std::int32_t, MaxScans> chunkBase{};  // per scan: its first chunk
        std::array<std::int32_t, MaxScans> chunkCount{}; // per scan: 0 = written whole
        std::int32_t                       totalChunks = 0;

        // per chunk
        std::unique_ptr<ChunkBits[]>    bits;     // the plain bit stream (Write)
        std::unique_ptr<ByteBuffer[]>   segments; // the stuffed bytes that start in the chunk (Splice)
        std::unique_ptr<ChunkCursor[]>  cursors;  // where a refinement chunk starts reading
        std::unique_ptr<std::int64_t[]> refBitCounts, runCounts;
        std::unique_ptr<std::int32_t[]> scanOf, partBegin, partEnd;

        std::unique_ptr<std::int32_t[]> countChunks; // the chunks of banded refinement scans, one Count job each
        std::int32_t                    countChunkCount = 0;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
