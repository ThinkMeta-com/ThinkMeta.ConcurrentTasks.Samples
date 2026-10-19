// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <cstdint>
#include <memory>

#include "ByteBuffer.h"
#include "Coefficients.h"
#include "Encoder.h"
#include "EncoderOptions.h"
#include "EncoderSetup.h"
#include "Entropy.h"
#include "Image.h"
#include "PixelPhase.h"
#include "ScanChunks.h"
#include "SimdFeatures.h"
#include "TokenBands.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    class AsyncInput;
    class AsyncOutput;

    /// <summary>Wall-clock seconds of the stages of one encode (--times).</summary>
    struct PhaseTimes {
        double setup    = 0; // Setup
        double pixels   = 0; // Pixel
        double tokenize = 0; // Tokenize and Stitch
        double huffman  = 0; // Huffman
        double write    = 0; // Count, PlaceCursors, Write and Splice
        double assemble = 0; // Assemble
        double release  = 0; // Release
    };

    /// <summary>Work split of the stage graph; 0 = automatic. Neither changes the output.</summary>
    struct ParallelOptions {
        std::int32_t bandRows   = 0; // iMCU rows per band of the Pixel node
        std::int32_t scanBands  = 0; // bands per AC scan of the Tokenize node; 1 = whole scans only
        std::int32_t stripWidth = 0; // pixel columns per strip of the Pixel node (rounded up to iMCU columns)
    };

    /// <summary>
    /// The task graph of one encode: its edges (the image, the coefficients, the tokens and bands,
    /// the codes, the chunks, the JPEG) and its nodes (the stages), built and run in one place.
    /// docs/task-graph.md in the ctJpeg repository tells how the graph grew to this shape.
    /// <code>
    ///  I/O scheduler (1 thread)   compute scheduler: each parallel node forks one worker task per
    ///                             thread and joins them before the next node starts (StageWorkers.h)
    ///
    ///  Read ──image──► Setup ──► Pixel ──coefficients──► Tokenize ──bands──► Stitch ──histograms──► Huffman
    ///  (AsyncFile::    (serial)  (tiles: bands × strips;  (AC bands, DC bands,   (one job per        (serial)
    ///   ReadFile;                 each waits for its      whole scans; frees     banded scan)            │
    ///   AsyncInput)               rows to arrive)         the image)                                 codes
    ///                                                                                                    ▼
    ///  Write file ◄──JPEG── Assemble ◄──segments── Splice ◄──bits── Write ◄──cursors── Count ◄────────────┘
    ///  (AsyncFile::         (serial: headers,       (one job       (chunks and whole    (one job per
    ///   WriteFile;           DHT/SOS, segments)      per chunk)     scans; frees the     refinement chunk)
    ///   AsyncOutput)              │                                  coefficients)
    ///   meanwhile:                ▼
    ///                       Release (one job per scan)
    /// </code>
    /// <list type="bullet">
    /// <item>The parallel nodes are fork-join stages (RunStage, StageWorkers.h): no job waits for
    /// another; only Pixel tiles wait for their rows of the input. The caller of Run - the main
    /// thread, converted to a task - runs the serial nodes and waits once per parallel node for its
    /// task group; it is not one of the compute scheduler's threads.</item>
    /// <item>The serial nodes (Setup, Huffman, PlaceCursors between Count and Write, Assemble) run
    /// on the caller.</item>
    /// <item>The sequential graph (--sequential) has one node instead of the stages:
    /// Read ──► SequentialEncode ──► Write file, the reference path EncodeJpeg in one task once
    /// the whole file has arrived.</item>
    /// <item>Read and Write file belong to the caller (main.cpp): Read is started before the graph
    /// and reached through Settings::input, Write file through Settings::outputFile.</item>
    /// </list>
    /// </summary>
    class EncodeGraph
    {
    public:
        struct Settings {
            EncoderOptions  options;
            SimdLevel       simd    = SimdLevel::Sse2;
            std::uint32_t   workers = 1; // threads of the scheduler: one worker task each per parallel node
            ParallelOptions split;
            bool            sequential = false;
            // The node Read (async input): Pixel jobs wait for their rows, Tokenize frees the
            // buffer; nullptr when the whole image is in memory. The image must point into its buffer.
            AsyncInput* input = nullptr;
            // The node Write file: started on io after Assemble, so it writes while Release frees
            // the buffers; nullptr leaves the file to the caller.
            AsyncOutput*    outputFile = nullptr;
            ITaskScheduler* io         = nullptr;
        };

        EncodeGraph(Image& image, const Settings& settings, ByteBuffer& output, PhaseTimes* times) noexcept;
        ~EncodeGraph();

        EncodeGraph(const EncodeGraph&)            = delete;
        EncodeGraph& operator=(const EncodeGraph&) = delete;

        /// <summary>
        /// Runs the graph on the started scheduler and waits until it has ended; the JPEG goes to
        /// output. Run waits for task groups, so call it from a task (ConvertThreadToTask).
        /// </summary>
        EncodeStatus Run(ITaskScheduler& scheduler) noexcept;

    private:
        EncodeStatus RunStages(ITaskScheduler& scheduler) noexcept;
        EncodeStatus RunSequential(ITaskScheduler& scheduler) noexcept;

        // --- nodes, in the order the data flows
        EncodeStatus Setup() noexcept;
        bool         Pixel(ITaskScheduler& scheduler) noexcept;
        bool         Tokenize(ITaskScheduler& scheduler) noexcept;
        bool         Stitch(ITaskScheduler& scheduler) noexcept;
        void         Huffman() noexcept;
        bool         Count(ITaskScheduler& scheduler) noexcept;
        bool         Write(ITaskScheduler& scheduler) noexcept;
        bool         Splice(ITaskScheduler& scheduler) noexcept;
        bool         Assemble() noexcept;
        void         Release(ITaskScheduler& scheduler) noexcept;

        Image&      image_;
        Settings    settings_;
        ByteBuffer& output_;
        PhaseTimes* times_;

        // the split of the Pixel node
        std::int32_t bandRows_  = DefaultBandRows;
        std::int32_t stripCols_ = 0; // iMCU columns per strip
        std::int32_t strips_    = 1; // strips per band

        // --- edges, in the order the data flows
        EncoderSetup                    setup_;        // Setup → all: tables, scan script, sizes
        std::unique_ptr<PixelScratch[]> scratch_;      // Pixel: the window buffers, one per worker
        Coefficients                    coefficients_; // Pixel → Tokenize: quantized blocks and the dense DC plane
        std::unique_ptr<ScanTokens[]>   scanTokens_;   // Tokenize → Stitch → Count, Write: the tokens per scan
        TokenBands                      bands_;        // Tokenize → Stitch: the jobs and what each band reports
        std::unique_ptr<Histograms[]>   histograms_;   // Tokenize, Stitch → Huffman: symbol counts per scan
        std::unique_ptr<HuffmanCoding>  coding_;       // Huffman → Write, Assemble: the code tables
        ScanChunks                      chunks_;       // Count → Write → Splice → Assemble: chunks, cursors, segments
        std::unique_ptr<ByteBuffer[]>   scanData_;     // Write → Assemble: the bytes of the scans written whole
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
