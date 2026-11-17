// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#if defined(_WIN32)
#    include <Windows.h>
#endif

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

#include "Encoder.h"
#include "Input.h"
#include "Profiling.h"

// headers with imports last
#include "BlockPipe.h"
#include "DispatchTask.h"
#include "EncodeTask.h"
#include "FrameEncoder.h"
#include "FrameRing.h"
#include "GainTask.h"
#include "Mp3WriterTask.h"
#include "PrepareTask.h"
#include "ResolveTask.h"
#include "SearchTask.h"
#include "SequentialEncodeTask.h"
#include "SharedSampleStream.h"
#include "Text.h"
#include "WavReaderTask.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    struct SessionConfig;

    /// <summary>
    /// The task graph of one encoding: its nodes (the tasks) and its edges (the shared objects in which the tasks
    /// meet: the PCM pipe, the sample stream, the frame ring and the MP3 file), built and run in one place. The
    /// ctLame documentation (task-graph.md) tells how the graph grew to this shape.
    /// <code>
    ///  I/O scheduler, 1 thread       compute scheduler, Settings::threads threads                                  I/O scheduler
    ///
    ///  Read ──pcm──► Dispatch ──ring──► Prepare ──ring──► Resolve ──ring──► Search ──ring──► Encode ──mp3──► Write
    ///  (async        (serial,           (parallel,        (serial,          (parallel,       (serial,        (async
    ///   ReadFile)     in order)          any order)        in order, High)   any order, High) in order, High)  WriteFile)
    ///                                    N tasks                             N tasks
    ///                    └──stream──► Gain L, Gain R ──────────────────────────────────────────────►┘ (ReplayGain, before the tag)
    /// </code>
    /// <list type="bullet">
    /// <item>Read and Write run on the I/O scheduler with asynchronous file I/O; the edges between them and the compute
    /// scheduler are bounded. Each does one request at a time, block after block, with AsyncFile and a position of its
    /// own (the library's StreamedFileReader and OrderedFileWriter are not used); Write writes the MP3 file front to
    /// back (Mp3File.h) and runs with High priority, Read with Normal. A read or write suspends only its task. The two
    /// have a scheduler of their own, with one thread, so that a completed request goes on at once and does not queue
    /// behind the compute tasks.</item>
    /// <item>The ring carries each frame through the five frame stages (FrameRing.h). Every stage has tasks of its
    /// own, each a loop over the frames of its stage: one task for a stage that carries state from frame to frame,
    /// one per compute thread for Prepare and for Search. The stages behind Prepare run with High priority, so the
    /// pipeline drains before the input runs further ahead.</item>
    /// <item>With ReplayGain, one Gain task per channel analyzes the sample stream beside the frame stages, in LAME's
    /// pieces. The three kinds of task meet in the stream (SharedSampleStream.h): it wakes the Gain tasks as Dispatch
    /// writes, and Encode waits there for the end of the stream, which carries the padding, the ReplayGain and
    /// whether the stream lost samples (out of memory).</item>
    /// <item>The sequential graph (--sequential) has one compute node instead of the five frame stages:
    /// Read ──pcm──► SequentialEncode ──mp3──► Write.</item>
    /// <item>Where a node runs and with which priority is a matter of speed only and is decided in one place
    /// (CreateNodes); the output does not depend on it.</item>
    /// </list>
    /// Use: construct, Initialize, Run. The encoder and the configuration must outlive the graph.
    /// </summary>
    class EncodeGraph
    {
    public:
        struct Settings {
            const Shared::Char* inPath          = nullptr;
            const Shared::Char* outPath         = nullptr;
            std::uint64_t       dataOffset      = 0;     // first byte of the WAV sample data
            std::uint64_t       byteLimit       = ~0ull; // bytes of sample data to read at most
            std::uint64_t       maxInputSamples = 0;     // samples per channel the input can have at most (0: unknown)
            PcmReader::Format   format;
            std::uint32_t       threads    = 1; // compute threads: one Prepare and one Search task each
            bool                sequential = false;
            Id3Bytes            id3; // the ID3 tags around the MP3 stream (none by default)
            // reading the WAV file: one read at a time; what counts is the data in flight, and beyond about 1 MB
            // nothing was gained (cold, NVMe; the numbers are in docs/task-graph.md, step 1)
            std::uint32_t readSize = 1024 * 1024; // bytes per read (the PCM pipe's block size)
        };

        /// <summary>
        /// CTLAME_PROFILE: busy and idle time of each node, and the wall time of the run. A node counts its life time
        /// (ProfiledRun), the edges count the time it waits in them (MeasureWaits).
        /// </summary>
        struct Profile {
            TaskProfile  read;
            TaskProfile  dispatch;
            TaskProfile  prepare;
            TaskProfile  resolve;
            TaskProfile  search;
            TaskProfile  encode; // Encode, or SequentialEncode
            TaskProfile  gain;   // Gain (both channels)
            TaskProfile  write;
            std::int64_t start = 0;
            std::int64_t end   = 0;
        };

        /// <summary>
        /// The errors of the nodes. A read error does not stop the encoding: like LAME, the encoder takes it for the
        /// end of the input, so the file is complete for what was read.
        /// </summary>
        struct Result {
            std::uint32_t readError   = 0;
            std::uint32_t writeError  = 0;
            std::int32_t  encodeError = 0; // LAME's error code or FrameEncoder::OutOfMemory, see FrameEncoder::Error
        };

        /// <summary>The scheduler a node runs on.</summary>
        enum class RunsOn { Io, Compute };

        /// <summary>A node as it is started: the task, where it runs and with which priority.</summary>
        struct Node {
            ITask*       task     = nullptr;
            RunsOn       runsOn   = RunsOn::Compute;
            TaskPriority priority = TaskPriority::Normal;
        };

        /// <summary>The nodes of the graph, in the order Run starts them.</summary>
        struct Nodes {
            const Node*   first = nullptr;
            std::uint32_t count = 0;

            const Node* begin() const noexcept { return first; }
            const Node* end() const noexcept { return first + count; }
        };

        /// <summary>
        /// The most nodes a graph can have: Read, Write, Dispatch, Resolve, Encode, two Gain tasks, and stageTasks
        /// tasks for each of Prepare and Search.
        /// </summary>
        static constexpr std::uint32_t MaxNodes(std::uint32_t stageTasks) noexcept { return 7 + 2 * stageTasks; }

        EncodeGraph(Encoder& encoder, const SessionConfig& config, const Settings& settings, Profile* profile) noexcept;

        EncodeGraph(const EncodeGraph&)            = delete;
        EncodeGraph& operator=(const EncodeGraph&) = delete;

        /// <summary>Allocates the edges and the arrays of the nodes; false if the memory is not available.</summary>
        bool Initialize() noexcept;

        /// <summary>
        /// Creates the nodes on the edges and returns them in the order of the diagram, which is the order Run starts
        /// them in. Where and when a node runs is a matter of speed only; the output does not depend on it (the unit
        /// test EncodeGraphOrderTests starts the nodes in other orders and with other priorities). Call it once, after
        /// Initialize; Run does. Only a caller that starts the nodes itself calls it instead of Run.
        /// </summary>
        Nodes CreateNodes() noexcept;

        /// <summary>
        /// Creates the nodes (CreateNodes), starts them on the two running schedulers and waits until all have ended.
        /// The graph owns its edges and their events; Run waits for a TaskGroup, so call it from a task or a thread
        /// converted with ConvertThreadToTask, after Initialize.
        /// </summary>
        Result Run(ITaskScheduler& io, ITaskScheduler& compute) noexcept;

        /// <summary>The errors of the nodes, once all nodes have ended.</summary>
        Result Errors() const noexcept;

    private:
        void Add(ITask* task, RunsOn runsOn, TaskPriority priority) noexcept;

        Encoder&             encoder_;
        const SessionConfig& config_;
        Settings             settings_;
        Profile*             profile_;

        // --- edges, in the order the data flows
        BlockPipe                pcm_;
        SharedSampleStream       stream_;
        std::optional<FrameRing> ring_; // not in the sequential graph
        Mp3File                  mp3_;  // the MP3 file in its sections; its stream is a bounded pipe

        // --- nodes, in the same order. std::optional: which nodes there are is only known after Initialize (sequential
        // or not, the Gain readers, the tasks per stage), and CreateNodes builds each in place. They stand behind the
        // edges, so they are destroyed before them.
        std::uint32_t                                 stageTasks_ = 0; // tasks of each parallel stage: one per compute thread
        std::optional<WavReaderTask>                  read_;
        std::optional<DispatchTask>                   dispatch_;
        std::unique_ptr<std::optional<PrepareTask>[]> prepares_; // Prepare, stageTasks_ of them
        std::optional<ResolveTask>                    resolve_;
        std::unique_ptr<std::optional<SearchTask>[]>  searches_;     // Search, stageTasks_ of them
        std::optional<FrameEncoder>                   frameEncoder_; // the work of Encode: appends to mp3 and ends it
        std::optional<EncodeTask>                     encode_;
        std::optional<SequentialEncodeTask>           sequential_; // instead of Dispatch to Encode in the sequential graph
        std::array<std::optional<GainTask>, 2>        gains_;      // Gain, one per channel; with ReplayGain in the parallel graph
        std::optional<Mp3WriterTask>                  write_;

        // --- the nodes as they are started
        std::unique_ptr<Node[]> nodes_;
        std::uint32_t           nodeCount_ = 0;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
