//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
// Contains code ported from libdeflate 1.26 (MIT License, Copyright 2016 Eric Biggers and 2024 Google LLC;
// see COPYING).
//

#pragma once

#include "BlockWriter.h"
#include "MultiParser.h"
#include "Parser.h"
#include "Resolver.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    /// <summary>How the input is split for speculative parsing.</summary>
    struct ChunkOptions {
        std::size_t   chunkSize = std::size_t{1} << 20;
        std::size_t   overlap   = 16384; // parsed past the chunk end, to meet the next chunk's parse
        std::uint32_t minLens   = 0;     // minimum match lengths parsed (bit m); 0: chosen per chunk
        std::size_t   ahead     = 0;     // chunks parsed ahead of the resolver (parallel); 0: workers + 4
    };

    /// <summary>Chunk start positions: 0, chunkSize, ... ; the last chunk is at least 1 KiB long.</summary>
    std::vector<std::size_t> PlanChunks(std::size_t inSize, std::size_t chunkSize);

    /// <summary>
    /// The paths a chunk parse follows: the schedule path and the constant minimum match lengths of the last resolved
    /// blocks (recent, bit m), limited to the values the level can produce. All for the fastest parser.
    /// </summary>
    /// With scheduleOnly (the schedule lane held in the last resolved chunks) just the schedule path.
    std::uint32_t ChunkMinLens(const LevelParams& params, const std::uint8_t* in, std::size_t inSize, std::size_t start, std::uint32_t recent,
                               bool scheduleOnly = false) noexcept;

    /// <summary>Speculative parse of a chunk: from its start to the next chunk's start plus the overlap.</summary>
    class ChunkParser
    {
    public:
        void Init(const LevelParams& params, const std::uint8_t* in, std::size_t inSize);
        void Parse(const std::vector<std::size_t>& starts, std::size_t k, std::size_t overlap, std::uint32_t minLens, PathLog& out);

    private:
        LevelParams         params_{};
        const std::uint8_t* in_     = nullptr;
        std::size_t         inSize_ = 0;
        Parser              parser_; // fastest
        MultiParser         multi_;  // greedy, lazy, lazy2
    };

    /// <summary>Writes the gzip header of libdeflate_gzip_compress(); returns its size (10).</summary>
    std::size_t WriteGzipHeader(std::int32_t level, std::uint8_t* out) noexcept;
    /// <summary>Writes CRC-32 and ISIZE; returns 8.</summary>
    std::size_t WriteGzipTrailer(std::uint32_t crc, std::size_t inSize, std::uint8_t* out) noexcept;

    /// <summary>
    /// The chunked pipeline on the calling thread: speculative chunk parses, resolver, block encoding, bitstream
    /// assembly. Produces the same gzip stream as the sequential compressor; used to test the pipeline without a
    /// scheduler. out: Compressor::GzipBound(inSize).
    /// </summary>
    std::size_t CompressGzipChunked(std::int32_t level, const std::uint8_t* in, std::size_t inSize, std::uint8_t* out, const ChunkOptions& options,
                                    ResolveStats* stats = nullptr);

    /// <summary>Appends the encoded blocks to the bitstream, choosing uncompressed blocks by the actual bit offset.</summary>
    void StitchBlock(BitWriter& os, const std::uint8_t* in, const ResolvedBlock& block, const EncodedBlock& encoded) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
