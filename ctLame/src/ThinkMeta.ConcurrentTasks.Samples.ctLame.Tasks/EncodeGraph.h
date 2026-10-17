// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <cstdint>
#include <memory>
#include <optional>

#include "Encoder.h"
#include "Input.h"
#include "Profiling.h"
#include "SampleStream.h"
#include "SchedulerHandle.h"

// headers with imports last
#include "BlockPipe.h"
#include "DispatchTask.h"
#include "EncodeTask.h"
#include "FrameEncoder.h"
#include "FrameRing.h"
#include "Mp3WriterTask.h"
#include "ResolveTask.h"
#include "SequentialEncodeTask.h"
#include "WavReaderTask.h"
#include "WorkerTask.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    struct SessionConfig;

    /// <summary>
    /// The task graph of one encoding: its nodes (the tasks) and its edges (the pipes, the sample
    /// stream and the frame ring), built and run in one place. The ctLame documentation
    /// (task-graph.md) tells how the graph grew to this shape.
    /// <code>
    ///  I/O scheduler, 1 thread       compute scheduler, one thread per logical processor                          I/O scheduler
    ///
    ///  Read ──pcm──► Dispatch ──ring──► Prepare ──ring──► Resolve ──ring──► Search ──ring──► Encode ──mp3──► Write
    ///  (async        (serial,           (parallel,        (serial,          (parallel,       (serial,        (async
    ///   ReadFile)     in order)          any order)        in order, High)   any order)       in order, High)  WriteFile)
    /// </code>
    /// <list type="bullet">
    /// <item>Read and Write run on the I/O scheduler with overlapped file I/O; the pipes between them and the compute
    /// scheduler are bounded, and their fill levels set the I/O priorities.</item>
    /// <item>The ring carries each frame through the five frame stages (FrameRing.h). Prepare and Search are the same
    /// worker tasks, one per compute thread; the serial nodes Resolve and Encode run with High priority, so the
    /// pipeline drains before the input runs further ahead.</item>
    /// <item>The sequential graph (--sequential) has one compute node instead of the five frame stages:
    /// Read ──pcm──► SequentialEncode ──mp3──► Write.</item>
    /// </list>
    /// </summary>
    class EncodeGraph
    {
    public:
        struct Settings {
            const wchar_t*    inPath          = nullptr;
            const wchar_t*    outPath         = nullptr;
            std::uint64_t     dataOffset      = 0;     // first byte of the WAV sample data
            std::uint64_t     byteLimit       = ~0ull; // bytes of sample data to read at most
            std::uint64_t     maxInputSamples = 0;     // samples per channel the input can have at most (0: unknown)
            PcmReader::Format format;
            std::uint32_t     threads    = 1; // compute threads: one worker each
            bool              sequential = false;
            // reading the WAV file: measured cold (fresh unbuffered copy) on NVMe, 16 threads, 4 and
            // 49 minutes of audio, what counts is the data in flight: 64 KB x 2 1.06, 64 KB x 4 1.00,
            // 256 KB x 2..16 and 1 MB x 2..8 0.96-0.98, 4 MB 1.00-1.02, sequential 0.95-0.96 from 256 KB;
            // reads that grow from 64 KB gain nothing over fixed ones
            std::uint32_t readSize      = 256 * 1024; // bytes per read (the PCM pipe's block size)
            std::uint32_t readsInFlight = 5;          // reads at once
            std::uint32_t firstReadSize = 0;          // 0, or a smaller first read that doubles per read up to readSize
        };

        /// <summary>CTLAME_PROFILE: busy and idle time of each node, and the wall time of the run.</summary>
        struct Profile {
            TaskProfile  read;
            TaskProfile  dispatch;
            TaskProfile  worker; // Prepare and Search
            TaskProfile  resolve;
            TaskProfile  encode; // Encode, or SequentialEncode
            TaskProfile  write;
            std::int64_t start = 0;
            std::int64_t end   = 0;
        };

        struct Result {
            std::uint32_t readError   = ERROR_SUCCESS;
            std::uint32_t writeError  = ERROR_SUCCESS;
            std::int32_t  encodeError = 0; // LAME's error code, see FrameEncoder::Error
        };

        EncodeGraph(Encoder& encoder, const SessionConfig& config, const Settings& settings, Profile* profile) noexcept;

        EncodeGraph(const EncodeGraph&)            = delete;
        EncodeGraph& operator=(const EncodeGraph&) = delete;

        /// <summary>
        /// Allocates the edges; false if the memory is not available. Call it from a task
        /// (ConvertThreadToTask): the events of the edges belong to the task that creates them.
        /// </summary>
        bool Initialize() noexcept;

        /// <summary>Creates the nodes, starts them on the two running schedulers and waits until all have ended.</summary>
        Result Run(SchedulerHandle& io, SchedulerHandle& compute) noexcept;

    private:
        Encoder&             encoder_;
        const SessionConfig& config_;
        Settings             settings_;
        Profile*             profile_;

        // --- edges, in the order the data flows
        BlockPipe                pcm_;
        SampleStream             stream_;
        std::optional<FrameRing> ring_; // not in the sequential graph
        BlockPipe                mp3_;

        // --- nodes, in the same order; created by Run, destroyed before the edges
        std::optional<WavReaderTask>                 read_;
        std::optional<DispatchTask>                  dispatch_;
        std::unique_ptr<std::optional<WorkerTask>[]> workers_; // Prepare and Search
        std::uint32_t                                workerCount_ = 0;
        std::optional<ResolveTask>                   resolve_;
        std::optional<Mp3WriterTask>                 write_;
        std::optional<FrameEncoder>                  frameEncoder_; // the work of Encode, writes into mp3 and hands the tag to Write
        std::optional<EncodeTask>                    encode_;
        std::optional<SequentialEncodeTask>          sequential_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
