// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <cstdint>
#include <memory>
#include <optional>

#include "CeltEncoder.h"
#include "HeapArray.h"
#include "OggOpusEncoder.h"
#include "SchedulerHandle.h"
#include "WavInput.h"

// headers with imports last
#include "EncodeHelpers.h"
#include "EncodeTasks.h"
#include "GraphEdges.h"
#include "ToneChain.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    /// <summary>
    /// The task graph of one encoding: its nodes (the tasks) and its edges (the PCM feed, the block
    /// ring, the rings of the analysis chain and the work queues of the helpers), built and run in one
    /// place. docs/task-graph.md tells how the graph grew to this shape.
    /// <code>
    ///  I/O scheduler (1 thread): FileRead and FileWrite (FileIo.h); everything else on the compute
    ///  scheduler, one thread per logical processor
    ///
    ///  FileRead ──read pipe──► Read                      Encode ──write pipe──► FileWrite
    ///
    ///  Read ──pcm──► Resample ──blocks──► Encode ──► Ogg pages (the encoder's write callback)
    ///  (High, WAV,   (×threads,  │        (High, frames in order: libopusenc's buffer, the
    ///   drain         any block) │         Opus/CELT encoder, Ogg)
    ///   extension)               │          ▲  ▲  ▲   ▲
    ///                            │          │  │  │   └── theta trial ⇄ Theta (High)
    ///                            │          │  │  └────── speculation ⇄ Speculate ×3 levels (High), each ⇄ Theta
    ///                            │          │  └───────── front parts ◄── Front (High) ◄─┐
    ///                            │          └──────────── tonality, signal frames         │ resync from Encode
    ///                            └──blocks──► Analyse ──calls──► Signal ──calls──────────┘
    ///                                         (High, tonality)   (High, input and CELT signal stage)
    /// </code>
    /// <list type="bullet">
    /// <item>FileRead and FileWrite (FileIo, I/O scheduler, owned by the frontend): the file behind the
    /// WAV header with overlapped reads into the read pipe, from which the InputFile under WavInput
    /// takes its bytes; the Ogg pages of the write callback through the write pipe to overlapped
    /// writes. Both graphs use them.</item>
    /// <item>Read reads the WAV samples into the PCM feed and dispatches the blocks of the ring; at the
    /// end it appends libopusenc's drain extension. With the input through FileRead it only waits for
    /// tasks and runs on the compute scheduler (High: the resamplers and all after them wait for its
    /// blocks); with stdin it reads synchronously and stays on the I/O scheduler.</item>
    /// <item>Resample: one task per compute thread, any block in any order (ParallelResampler,
    /// bit-identical to the streaming resampler; input at 48 kHz is copied).</item>
    /// <item>Encode, the serial backbone: the blocks in order into libopusenc's buffer logic, which
    /// encodes the frames and writes the pages; it reads the results of the analysis chain and hands
    /// work to the helper nodes. A slot of the ring is free once Encode and Analyse have it.</item>
    /// <item>The analysis chain runs ahead of Encode: Analyse (tonality analysis), Signal (input and CELT
    /// signal stage), Front (speculative CELT front part, resynchronised by Encode).</item>
    /// <item>Speculate: up to three levels quantize the next frames on cloned encoder states (from 7
    /// threads, three levels from 8); Theta: the second theta RDO trial (from 3 threads and --comp 8;
    /// the speculation levels get their own from 12 threads, or with a single level).</item>
    /// <item>The sequential graph (SequentialGraph, --sequential) has one node: SequentialEncode,
    /// opusenc's frame loop.</item>
    /// </list>
    /// The graph owns its edges and their events; Run waits for its TaskGroup, so call it from a task
    /// or a thread converted with ConvertThreadToTask.
    /// </summary>
    class EncodeGraph
    {
    public:
        struct Settings {
            std::uint32_t  threads    = 0;     // compute threads, 0 = all logical processors
            std::int32_t   lookahead  = 0;     // OPUS_GET_LOOKAHEAD after all settings
            std::int32_t   complexity = 10;    // --comp: the theta helpers only where CELT tries a second theta (8 and above)
            EncodeProgress progress;           // after every block
            bool           asyncInput = false; // the input comes through FileRead: Read on the compute scheduler
        };

        EncodeGraph(WavInput& wav, OggOpusEncoder& encoder, const Settings& settings) noexcept;

        EncodeGraph(const EncodeGraph&)            = delete;
        EncodeGraph& operator=(const EncodeGraph&) = delete;

        /// <summary>Allocates the edges; false if the memory is not available.</summary>
        bool Initialize() noexcept;

        /// <summary>Creates the nodes, starts them on the two running schedulers and waits until all have ended.</summary>
        /// <returns>An OPE error code: 0, the encoder's error, or OPE_ALLOC_FAIL.</returns>
        std::int32_t Run(ITaskScheduler& io, ITaskScheduler& compute) noexcept;

        std::uint32_t Threads() const noexcept { return threads_; }

    private:
        static constexpr std::int32_t SpecLevels = EncodeTask::SpecLevels;

        WavInput&       wav_;
        OggOpusEncoder& encoder_;
        Settings        settings_;
        std::uint32_t   threads_ = 0;

        // --- edges, in the order the data flows
        PcmFeed          pcm_;
        BlockRing        ring_;
        ToneChain        chain_; // the rings of the analysis chain (calls, signal frames, front parts)
        bool             chained_ = false;
        ThetaHelper      theta_;                    // Encode ⇄ Theta
        SpeculationQueue speculations_[SpecLevels]; // Encode → Speculate
        ThetaHelper      specThetas_[SpecLevels];   // Speculate ⇄ Theta

        // --- nodes, in the same order; created by Run, destroyed before the edges
        std::optional<ReadTask>                        read_;
        std::unique_ptr<std::optional<ResampleTask>[]> resamplers_;
        std::uint32_t                                  resamplerCount_ = 0;
        std::optional<AnalyseTask>                     analyse_;
        std::optional<SignalTask>                      signal_;
        std::optional<FrontTask>                       front_;
        std::optional<SpeculateTask>                   speculate_[SpecLevels];
        std::optional<ThetaTask>                       specTheta_[SpecLevels];
        std::optional<ThetaTask>                       thetaTask_;
        std::optional<EncodeTask>                      encode_;
    };

    /// <summary>
    /// The sequential graph: one node, SequentialEncode (opusenc's frame loop), on one compute thread.
    /// The reference path (--sequential).
    /// <code>
    ///  SequentialEncode: read a frame ──► ope_encoder_write_float ──► Ogg pages, then ope_encoder_drain
    /// </code>
    /// </summary>
    class SequentialGraph
    {
    public:
        /// <summary>buffer: frameSize frames of the input's channels; progress: after every frame.</summary>
        SequentialGraph(WavInput& wav, OggOpusEncoder& encoder, float* buffer, std::int32_t frameSize, const EncodeProgress& progress) noexcept
            : encode_(wav, encoder, buffer, frameSize, progress)
        {}

        SequentialGraph(const SequentialGraph&)            = delete;
        SequentialGraph& operator=(const SequentialGraph&) = delete;

        /// <summary>Starts the node on the running scheduler and waits for it.</summary>
        std::int32_t Run(ITaskScheduler& compute) noexcept;

    private:
        SequentialEncodeTask encode_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
