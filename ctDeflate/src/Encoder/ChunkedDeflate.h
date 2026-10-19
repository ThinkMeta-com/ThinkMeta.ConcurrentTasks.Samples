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

// New in ctDeflate, not in libdeflate: splits the input into chunks for speculative parsing and runs the whole parallel
// algorithm sequentially as a reference (chunk parses, resolver, block encoding, bitstream assembly).

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    /// <summary>How the input is split for speculative parsing.</summary>
    struct ChunkOptions {
        std::size_t chunkSize = std::size_t{1} << 20;
        std::size_t overlap   = 16384; // parsed past the chunk end, to meet the next chunk's parse
        std::size_t ahead     = 0;     // chunks parsed ahead of the resolver (parallel); 0: workers + 4
    };

    /// <summary>Chunk start positions: 0, chunkSize, ... ; the last chunk is at least 1 KiB long.</summary>
    std::vector<std::size_t> PlanChunks(std::size_t inSize, std::size_t chunkSize);

    /// <summary>
    /// The paths a chunk parse follows: the schedule path and the constant minimum match lengths of the last resolved
    /// blocks (recent, bit m), limited to the values the level can produce. All for the fastest parser. With
    /// scheduleOnly (the schedule lane held in the last resolved chunks) just the schedule path.
    /// </summary>
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

    /// <summary>
    /// The reference pipeline without a scheduler: speculative chunk parses, resolver, block encoding and bitstream
    /// assembly, one after the other on the calling thread. Produces the same gzip stream as the sequential compressor
    /// and as DeflateGraph; the unit tests check the parallel algorithm with it. out: Compressor::GzipBound(inSize).
    /// </summary>
    std::size_t CompressGzipChunked(std::int32_t level, const std::uint8_t* in, std::size_t inSize, std::uint8_t* out, const ChunkOptions& options,
                                    ResolveStats* stats = nullptr);

    /// <summary>Appends the encoded blocks to the bitstream, choosing uncompressed blocks by the actual bit offset.</summary>
    void StitchBlock(BitWriter& os, const std::uint8_t* in, const ResolvedBlock& block, const EncodedBlock& encoded) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
