//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <cstddef>
#include <cstdint>

#include "ChunkedDeflate.h"
#include "Resolver.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    /// <summary>Timings (seconds) and resolver counters of a parallel run.</summary>
    struct ParallelStats {
        ResolveStats resolve;
        std::size_t  chunks       = 0;
        std::size_t  blocks       = 0;
        double       parseResolve = 0; // until the last block is resolved
        double       resolveWait  = 0; // resolver waiting for chunk parses
        double       encode       = 0;
        double       stitch       = 0;
    };

    /// <summary>
    /// gzip compression of one input on the scheduler, bit-identical to libdeflate_gzip_compress(): the chunks are
    /// parsed speculatively by worker tasks while the calling task resolves them in order; then the blocks are encoded
    /// in parallel and appended to the bitstream. The caller must run as a task (ConvertThreadToTask) and the scheduler
    /// must be started. out: Compressor::GzipBound(inSize).
    /// </summary>
    std::size_t CompressGzipParallel(ITaskScheduler& scheduler, std::uint32_t workers, std::int32_t level, const std::uint8_t* in, std::size_t inSize,
                                     std::uint8_t* out, const ChunkOptions& options, ParallelStats* stats = nullptr);

    /// <summary>Outcome of a file compression: sizes, and a Windows error code (0: success).</summary>
    struct FileResult {
        std::uint64_t inSize  = 0;
        std::uint64_t outSize = 0;
        std::uint32_t error   = 0;
    };

    /// <summary>Receives a raw DEFLATE stream piece by piece, in order, from one task at a time.</summary>
    class IDeflateSink
    {
    public:
        virtual void Write(const std::uint8_t* data, std::size_t size) = 0;

    protected:
        ~IDeflateSink() = default;
    };

    /// <summary>Outcome of DeflateFileParallel: sizes, CRC-32 of the input, a Windows error code (0: success).</summary>
    struct DeflateResult {
        std::uint64_t inSize  = 0;
        std::uint64_t outSize = 0;
        std::uint32_t crc     = 0;
        std::uint32_t error   = 0;
    };

    /// <summary>
    /// The raw DEFLATE stream of a file (no gzip framing), streamed like CompressGzipFileParallel and handed to sink as
    /// it is produced; bit-identical to libdeflate_deflate_compress(). For containers such as ZIP.
    /// </summary>
    DeflateResult DeflateFileParallel(ITaskScheduler& scheduler, std::uint32_t workers, std::int32_t level, const wchar_t* inPath, IDeflateSink& sink,
                                      const ChunkOptions& options, ParallelStats* stats = nullptr);

    /// <summary>
    /// The same compression streamed from file to file: the input is read with AsyncFile into a reserved address range
    /// whose pages are committed as they are read and decommitted once no stage needs them; the chunk parses run at most
    /// a few chunks ahead of the resolver; the output is written as it is appended. Memory stays bounded by the chunks in
    /// flight, not by the file size. Same calling conditions as CompressGzipParallel.
    /// </summary>
    FileResult CompressGzipFileParallel(ITaskScheduler& scheduler, std::uint32_t workers, std::int32_t level, const wchar_t* inPath, const wchar_t* outPath,
                                        const ChunkOptions& options, ParallelStats* stats = nullptr);

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
