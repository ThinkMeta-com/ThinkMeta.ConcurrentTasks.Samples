//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#include "ParallelDeflate.h"

#include "DeflateGraph.h"

// Each entry point builds the edges of a DeflateGraph and runs it; the graph and its nodes are in DeflateGraph.h.
namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    std::size_t CompressGzipParallel(ITaskScheduler& scheduler, std::uint32_t workers, std::int32_t level, const std::uint8_t* in, std::size_t inSize,
                                     std::uint8_t* out, const ChunkOptions& options, ParallelStats* stats)
    {
        DeflateGraph graph({.level = level, .workers = workers, .options = options});
        graph.AttachInput(in, inSize);
        graph.AttachOutput(out);
        return static_cast<std::size_t>(graph.Run(scheduler, stats).outSize);
    }

    FileResult CompressGzipFileParallel(ITaskScheduler& scheduler, std::uint32_t workers, std::int32_t level, const wchar_t* inPath, const wchar_t* outPath,
                                        const ChunkOptions& options, ParallelStats* stats)
    {
        FileResult   result;
        DeflateGraph graph({.level = level, .workers = workers, .options = options});
        result.error = graph.OpenInput(inPath);
        if (result.error != 0)
            return result;
        result.error = graph.OpenOutput(outPath);
        if (result.error != 0)
            return result;
        const DeflateGraph::Result run = graph.Run(scheduler, stats);
        result.inSize                  = run.inSize;
        result.outSize                 = run.outSize;
        result.error                   = run.error;
        return result;
    }

    DeflateResult DeflateFileParallel(ITaskScheduler& scheduler, std::uint32_t workers, std::int32_t level, const wchar_t* inPath, IDeflateSink& sink,
                                      const ChunkOptions& options, ParallelStats* stats)
    {
        DeflateResult result;
        DeflateGraph  graph({.level = level, .workers = workers, .options = options, .gzip = false});
        result.error = graph.OpenInput(inPath);
        if (result.error != 0)
            return result;
        result.error = graph.OpenOutput(sink);
        if (result.error != 0)
            return result;
        const DeflateGraph::Result run = graph.Run(scheduler, stats);
        result.inSize                  = run.inSize;
        result.outSize                 = run.outSize;
        result.crc                     = run.crc;
        result.error                   = run.error;
        return result;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
