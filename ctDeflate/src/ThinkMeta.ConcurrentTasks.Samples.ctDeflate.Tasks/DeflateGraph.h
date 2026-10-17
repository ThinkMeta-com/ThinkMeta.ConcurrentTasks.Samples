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
#include <memory>
#include <optional>

#include "ChunkedDeflate.h"
#include "ParallelDeflate.h"
#include "Parser.h"

// headers with imports last
#include "BlockRing.h"
#include "ChunkLogs.h"
#include "DeflateNodes.h"
#include "InputWindow.h"
#include "OutputWindow.h"

import ThinkMeta.ConcurrentTasks.Synchronization.Semaphore;

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    /// <summary>
    /// The task graph of one compression: its nodes (the tasks) and its edges (input, logs, ring, output), built and run
    /// in one place. docs/task-graph.md of the ctDeflate sample tells how the graph grew to this shape.
    /// <code>
    ///  I/O scheduler   compute scheduler, one thread per logical processor                                   I/O scheduler
    ///
    ///  Read ──input──► Parse ──logs──► Resolve ──ring──► Encode ──ring──► Stitch ──output──► Write
    ///  (async          (parallel,      (serial,          (parallel,       (serial, in order,  (async
    ///   ReadFile)       up to ahead     in order, on     any order)       run by the encoder  WriteFile)
    ///                   chunks past     the calling                        that completes)
    ///                   Resolve)        task)
    ///    ▲               ▲                 │
    ///    └── allowance ──┴─── window, ─────┘
    ///                         feedback
    /// </code>
    /// <list type="bullet">
    /// <item>Read and Write run on the I/O scheduler with overlapped file I/O. Read streams the file into the input window,
    /// only as far as the parse window needs; Stitch hands the final output to Write and frees the input and logs behind
    /// the bitstream, Write frees what it has written, so the memory stays bounded by the chunks in flight.</item>
    /// <item>Parse and Encode are the same worker tasks, one per compute thread: a published block is encoded first, else
    /// the next chunk of the parse window is parsed. Resolve, the serial node, runs on the task that calls Run, a thread
    /// of its own beside the workers' threads, and helps Encode at the end. Stitch has no task: the encoder that
    /// completes the next block in order runs it.</item>
    /// <item>The sequential graph (--sequential, level 0, inputs too small to split) has one compute node instead, which
    /// streams as well: Read ──input──► SequentialCompress ──output──► Write.</item>
    /// </list>
    /// </summary>
    class DeflateGraph
    {
    public:
        struct Settings {
            std::int32_t  level   = 6;
            std::uint32_t workers = 1; // Parse/Encode worker tasks
            ChunkOptions  options;
            bool          gzip       = true; // gzip framing, else a raw DEFLATE stream
            bool          sequential = false;
            bool          libraryIo  = false; // Read and Write with AsyncFile::ReadFile/WriteFile (for comparison)
            IoShape       readShape;          // block size and requests in flight of Read
            IoShape       writeShape;         // and of Write
        };

        struct Result {
            std::uint64_t inSize  = 0;
            std::uint64_t outSize = 0;
            std::uint32_t crc     = 0; // CRC-32 of the input
            std::uint32_t error   = 0; // Windows error code of the file I/O
        };

        explicit DeflateGraph(const Settings& settings) noexcept;

        DeflateGraph(const DeflateGraph&)            = delete;
        DeflateGraph& operator=(const DeflateGraph&) = delete;

        // The edges input and output; the input first, its size bounds the output. Call them from the task that runs
        // the graph: the events of the edges belong to the task that creates them.
        std::uint32_t OpenInput(const wchar_t* path) noexcept { return input_.Open(path); }
        void          AttachInput(const std::uint8_t* data, std::size_t size) noexcept { input_.Attach(data, size); }
        std::uint32_t OpenOutput(const wchar_t* path) noexcept
        {
            const std::uint32_t error = output_.Open(path, OutputBound());
            if (error == 0 && settings_.libraryIo)
                output_.HandOverFile(path);
            return error;
        }
        std::uint32_t OpenOutput(IDeflateSink& sink) noexcept { return output_.Open(sink, OutputBound()); }
        void          AttachOutput(std::uint8_t* out) noexcept { output_.Attach(out); }
        std::size_t   OutputBound() const noexcept;

        /// <summary>
        /// Creates the nodes, starts them on the running scheduler and waits until all have ended; writes the rest of the
        /// output. The caller must run as a task (ConvertThreadToTask).
        /// </summary>
        Result Run(ITaskScheduler& io, ITaskScheduler& compute, ParallelStats* stats = nullptr) noexcept;

        /// <summary>The same with Read and Write on the compute scheduler.</summary>
        Result Run(ITaskScheduler& scheduler, ParallelStats* stats = nullptr) noexcept { return Run(scheduler, scheduler, stats); }

    private:
        void RunParallel(ITaskScheduler& io, ITaskScheduler& compute, Result& result, ParallelStats* stats) noexcept;
        void RunSequential(ITaskScheduler& io, ITaskScheduler& compute, Result& result) noexcept;
        /// <summary>Creates the node Read (null: the input is in memory).</summary>
        Tasks::Task* StartRead() noexcept;

        Settings    settings_;
        LevelParams params_;

        // --- edges, in the order the data flows
        InputWindow                input_;
        std::optional<ChunkLogs>   logs_; // not in the sequential graph
        std::optional<BlockRing>   ring_; // not in the sequential graph
        OutputWindow               output_;
        Synchronization::Semaphore work_; // wakes the workers: a block was published, the parse window moved, Resolve ended

        // --- nodes, in the same order; created by Run, destroyed before the edges
        std::optional<ReadTask>                      read_;    // only when the input streams from a file
        std::unique_ptr<std::optional<WorkerTask>[]> workers_; // Parse and Encode
        std::optional<ResolveTask>                   resolve_; // run on the calling task
        std::optional<BitWriter>                     bits_;    // the bitstream Stitch appends to
        std::optional<Stitch>                        stitch_;
        std::optional<SequentialCompressTask>        sequential_;
        std::optional<WriteTask>                     write_;        // only when the output goes to a file or a sink
        std::optional<LibraryReadTask>               libraryRead_;  // instead of read_ with libraryIo
        std::optional<LibraryWriteTask>              libraryWrite_; // instead of write_ with libraryIo and a file
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
