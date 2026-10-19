// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "EncodeGraph.h"

#include <cstdint>
#include <memory>
#include <new>

#include "JpegFile.h"

// headers with imports last
#include "AsyncInput.h"
#include "AsyncOutput.h"
#include "StageWorkers.h"

import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    namespace {

        /// <summary>
        /// Rows below a band a Pixel tile waits for: ComputeBand reads up to 5 rows of context below its band (the AQ
        /// field); 8 is a safe bound and costs nothing, since the input arrives in 1 MB steps.
        /// </summary>
        constexpr std::int32_t ContextRowsBelow = 8;

        /// <summary>Bytes reserved in the JPEG for the markers and tables around the scan data.</summary>
        constexpr std::size_t HeaderReserve = 4096;

        double Now() noexcept
        {
            LARGE_INTEGER counter, frequency;
            QueryPerformanceCounter(&counter);
            QueryPerformanceFrequency(&frequency);
            return static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
        }

        /// <summary>The node SequentialEncode of the sequential graph: the reference path EncodeJpeg in one task.</summary>
        class SequentialEncodeTask : public Tasks::Task
        {
        public:
            SequentialEncodeTask(const Image& image, const EncoderOptions& options, SimdLevel simd, ByteBuffer& output) noexcept
                : image_(image),
                  options_(options),
                  simd_(simd),
                  output_(output)
            {}

            void Run() noexcept override { status_ = EncodeJpeg(image_, options_, simd_, output_); }

            EncodeStatus Status() const noexcept { return status_; }

        private:
            const Image&          image_;
            const EncoderOptions& options_;
            SimdLevel             simd_;
            ByteBuffer&           output_;
            EncodeStatus          status_ = EncodeStatus::Ok;
        };

    } // namespace

    EncodeGraph::EncodeGraph(Image& image, const Settings& settings, ByteBuffer& output, PhaseTimes* times) noexcept
        : image_(image),
          settings_(settings),
          output_(output),
          times_(times)
    {
        if (settings_.workers == 0)
            settings_.workers = 1;
    }

    EncodeGraph::~EncodeGraph() = default;

    EncodeStatus EncodeGraph::Run(ITaskScheduler& scheduler) noexcept
    {
        return settings_.sequential ? RunSequential(scheduler) : RunStages(scheduler);
    }

    EncodeStatus EncodeGraph::RunSequential(ITaskScheduler& scheduler) noexcept
    {
        SequentialEncodeTask         encode(image_, settings_.options, settings_.simd, output_);
        Synchronization::TaskGroup_s group; // one waiter: this task
        group.AddTask(&encode);
        scheduler.AddTask(&encode, TaskPriority::Normal);
        group.Wait();
        return encode.Status();
    }

    EncodeStatus EncodeGraph::RunStages(ITaskScheduler& scheduler) noexcept
    {
        const double t0     = Now();
        EncodeStatus status = Setup();
        if (status != EncodeStatus::Ok)
            return status;
        const double t1 = Now();
        if (!Pixel(scheduler)) // a tile fails when its rows do not arrive, or without memory
            return settings_.input != nullptr && settings_.input->Failed() ? EncodeStatus::ReadFailed : EncodeStatus::OutOfMemory;
        const double t2 = Now();
        if (!Tokenize(scheduler) || !Stitch(scheduler))
            return EncodeStatus::OutOfMemory;
        const double t3 = Now();
        Huffman();
        const double t4 = Now();
        if (!Count(scheduler))
            return EncodeStatus::OutOfMemory;
        chunks_.PlaceCursors(setup_, scanTokens_.get()); // the serial node PlaceCursors, between Count and Write
        if (!Write(scheduler) || !Splice(scheduler))
            return EncodeStatus::OutOfMemory;
        const double t5 = Now();
        if (!Assemble())
            return EncodeStatus::OutOfMemory;
        const double                 t6 = Now();
        Synchronization::TaskGroup_s writing; // one waiter: this task
        if (settings_.outputFile != nullptr && settings_.io != nullptr) {
            writing.AddTask(settings_.outputFile);
            settings_.io->AddTask(settings_.outputFile, TaskPriority::Normal);
        }
        Release(scheduler);
        writing.Wait();
        const double t7 = Now();
        if (times_ != nullptr) {
            times_->setup    = t1 - t0;
            times_->pixels   = t2 - t1;
            times_->tokenize = t3 - t2;
            times_->huffman  = t4 - t3;
            times_->write    = t5 - t4;
            times_->assemble = t6 - t5;
            times_->release  = t7 - t6;
        }
        return EncodeStatus::Ok;
    }

    // --- Setup (serial): tables, scan script and coefficient planes; the split of Pixel; the plans
    // of Tokenize and Write.
    EncodeStatus EncodeGraph::Setup() noexcept
    {
        const EncodeStatus status = PrepareEncode(image_, settings_.options, setup_, coefficients_);
        if (status != EncodeStatus::Ok)
            return status;
        const auto workers = static_cast<std::int32_t>(settings_.workers);
        bandRows_          = settings_.split.bandRows;
        if (bandRows_ <= 0) {
            // about four bands per worker for balance, but not so thin that the context rows of
            // every band (5 above, 5 below) dominate
            const std::int32_t perBand = (setup_.iMcuRows + 4 * workers - 1) / (4 * workers);
            bandRows_                  = perBand < 4 ? 4 : perBand;
        }
        // strips of about stripWidth pixel columns, so that the color planes of a worker (about
        // 300 bytes per column) stay in its share of the cache
        const std::int32_t mcuW  = DctSize * setup_.maxH;
        const std::int32_t width = settings_.split.stripWidth > 0 ? settings_.split.stripWidth : DefaultStripWidth;
        stripCols_               = (width + mcuW - 1) / mcuW;
        strips_                  = (setup_.iMcuCols + stripCols_ - 1) / stripCols_;

        scanTokens_.reset(new (std::nothrow) ScanTokens[MaxScans]);
        histograms_.reset(new (std::nothrow) Histograms[MaxScans]());
        coding_.reset(new (std::nothrow) HuffmanCoding());
        scanData_.reset(new (std::nothrow) ByteBuffer[MaxScans]);
        scratch_.reset(new (std::nothrow) PixelScratch[settings_.workers]);
        if (!scanTokens_ || !histograms_ || !coding_ || !scanData_ || !scratch_)
            return EncodeStatus::OutOfMemory;
        if (!bands_.Plan(setup_, scanTokens_.get(), settings_.workers, settings_.split.scanBands) || !chunks_.Plan(setup_, bands_))
            return EncodeStatus::OutOfMemory;
        return EncodeStatus::Ok;
    }

    // --- Pixel: tiles of bands (iMCU rows) and strips (iMCU columns). With the async input each tile first waits for its
    // rows to arrive; this is the only wait inside a job of the graph.
    bool EncodeGraph::Pixel(ITaskScheduler& scheduler) noexcept
    {
        const std::int32_t tiles = (setup_.iMcuRows + bandRows_ - 1) / bandRows_ * strips_;
        return RunStage(scheduler, settings_.workers, tiles, [this](std::int32_t index, std::uint32_t worker) noexcept {
            const std::int32_t strip = index % strips_;
            const std::int32_t band  = index / strips_;
            const std::int32_t r0    = band * bandRows_;
            const std::int32_t r1    = r0 + bandRows_ < setup_.iMcuRows ? r0 + bandRows_ : setup_.iMcuRows;
            const std::int32_t c0    = strip * stripCols_;
            const std::int32_t c1    = c0 + stripCols_ < setup_.iMcuCols ? c0 + stripCols_ : setup_.iMcuCols;
            if (settings_.input != nullptr) {
                const std::int32_t end  = r1 * DctSize * setup_.maxV + ContextRowsBelow;
                const std::int32_t rows = end < image_.Height() ? end : image_.Height();
                settings_.input->WaitFor(static_cast<std::size_t>(image_.Row(0) - settings_.input->Data()) + static_cast<std::size_t>(rows) * image_.Stride());
                if (settings_.input->Failed())
                    return false; // RunStages reports ReadFailed
            }
            return ComputeCoefficients(setup_, image_, settings_.simd, r0, r1, c0, c1, coefficients_, scratch_[worker]);
        });
    }

    // --- Tokenize: whole scans, AC bands through all scans of their component, DC bands. Side job: frees the input
    // buffer, which no stage reads any more.
    bool EncodeGraph::Tokenize(ITaskScheduler& scheduler) noexcept
    {
        const std::int32_t freeInput = settings_.input != nullptr ? 1 : 0;
        return RunStageWithSideJobs(
            scheduler, settings_.workers, freeInput, [this](std::int32_t) noexcept { settings_.input->Release(); }, bands_.jobCount,
            [this](std::int32_t index, std::uint32_t) noexcept {
                const TokenBands::Job job = bands_.jobs[static_cast<std::size_t>(index)];
                if (job.scan >= 0 && job.band >= 0)
                    return TokenizeDcBand(setup_, coefficients_, job.scan, job.rowBegin, job.rowEnd, scanTokens_[job.scan].parts[job.band],
                                          bands_.dcHistograms[static_cast<std::size_t>(job.scan)][job.band]);
                if (job.band < 0) {
                    if (!TokenizeScan(setup_, coefficients_, job.scan, scanTokens_[job.scan]))
                        return false;
                    AddScanHistograms(setup_, job.scan, scanTokens_[job.scan], histograms_[job.scan]);
                    return true;
                }
                const auto comp = static_cast<std::size_t>(job.component);
                for (std::int32_t i = 0; i < bands_.componentScanCount[comp]; ++i) {
                    const std::int32_t scan = bands_.componentScans[comp][static_cast<std::size_t>(i)];
                    if (!TokenizeScanBand(setup_, coefficients_, scan, job.rowBegin, job.rowEnd, scanTokens_[scan].parts[2 * job.band + 1],
                                          bands_.bandResults[static_cast<std::size_t>(scan)][job.band]))
                        return false;
                }
                return true;
            });
    }

    // --- Stitch: joins the bands of each banded scan (EOB runs across band borders, histograms).
    bool EncodeGraph::Stitch(ITaskScheduler& scheduler) noexcept
    {
        return RunStage(scheduler, settings_.workers, bands_.stitchCount, [this](std::int32_t index, std::uint32_t) noexcept {
            const std::int32_t scan = bands_.stitchScans[static_cast<std::size_t>(index)];
            if (bands_.dcHistograms[static_cast<std::size_t>(scan)]) { // DC bands: only the histograms
                auto& sum = histograms_[scan].count;
                for (std::int32_t b = 0; b < bands_.scanBands[static_cast<std::size_t>(scan)]; ++b) {
                    const auto& band = bands_.dcHistograms[static_cast<std::size_t>(scan)][b].count;
                    for (std::size_t ctx = 0; ctx < MaxComponents; ++ctx)
                        for (std::size_t sym = 0; sym < HuffmanSymbols; ++sym)
                            sum[ctx][sym] += band[ctx][sym];
                }
                return true;
            }
            return StitchScanBands(setup_, scan, bands_.scanBands[static_cast<std::size_t>(scan)], scanTokens_[scan],
                                   bands_.bandResults[static_cast<std::size_t>(scan)].get(), histograms_[scan]);
        });
    }

    // --- Huffman (serial, small): the code tables from the summed histograms.
    void EncodeGraph::Huffman() noexcept
    {
        for (std::int32_t i = 1; i < setup_.scanCount; ++i)
            for (std::size_t ctx = 0; ctx < MaxContexts; ++ctx)
                for (std::size_t sym = 0; sym < HuffmanSymbols; ++sym)
                    histograms_[0].count[ctx][sym] += histograms_[static_cast<std::size_t>(i)].count[ctx][sym];
        OptimizeHuffmanCodes(setup_, histograms_[0], *coding_);
    }

    // --- Count: the correction bits and run lengths each chunk of a refinement scan reads (from the stitched tokens).
    bool EncodeGraph::Count(ITaskScheduler& scheduler) noexcept
    {
        return RunStage(scheduler, settings_.workers, chunks_.countChunkCount, [this](std::int32_t index, std::uint32_t) noexcept {
            const auto ci = static_cast<std::size_t>(chunks_.countChunks[static_cast<std::size_t>(index)]);
            CountRefinementStreams(scanTokens_[chunks_.scanOf[ci]], chunks_.partBegin[ci], chunks_.partEnd[ci], chunks_.refBitCounts[ci], chunks_.runCounts[ci]);
            return true;
        });
    }

    // --- Write: the chunks of the banded scans as plain bit streams, the other scans whole. Side jobs: free the
    // coefficient planes, one per component, which the writing does not read, so the kernel returns their pages while
    // the other workers write.
    bool EncodeGraph::Write(ITaskScheduler& scheduler) noexcept
    {
        return RunStageWithSideJobs(
            scheduler, settings_.workers, setup_.frame.components, [this](std::int32_t component) noexcept { coefficients_.ReleaseComponent(component); },
            chunks_.jobCount, [this](std::int32_t index, std::uint32_t) noexcept {
                const ScanChunks::Job job = chunks_.jobs[static_cast<std::size_t>(index)];
                if (job.chunk < 0)
                    return WriteScanData(setup_, job.scan, scanTokens_[job.scan], *coding_, scanData_[job.scan]);
                const auto ci = static_cast<std::size_t>(job.chunk);
                return WriteScanChunk(setup_, job.scan, scanTokens_[job.scan], job.partBegin, job.partEnd, chunks_.cursors[ci], *coding_, chunks_.bits[ci]);
            });
    }

    // --- Splice: the stuffed bytes that start in each chunk, across the chunk borders.
    bool EncodeGraph::Splice(ITaskScheduler& scheduler) noexcept
    {
        return RunStage(scheduler, settings_.workers, chunks_.totalChunks, [this](std::int32_t chunk, std::uint32_t) noexcept {
            const std::int32_t scan = chunks_.scanOf[static_cast<std::size_t>(chunk)];
            const std::int32_t base = chunks_.chunkBase[static_cast<std::size_t>(scan)];
            return SpliceScanChunk(chunks_.bits.get() + base, chunks_.chunkCount[static_cast<std::size_t>(scan)], chunk - base,
                                   chunks_.segments[static_cast<std::size_t>(chunk)]);
        });
    }

    // --- Assemble (serial): SOI, DQT, SOF, per scan DHT/SOS and its data, EOI (AssembleJpeg, as the sequential encoder).
    bool EncodeGraph::Assemble() noexcept
    {
        std::size_t total = HeaderReserve;
        for (std::int32_t i = 0; i < setup_.scanCount; ++i)
            total += scanData_[static_cast<std::size_t>(i)].Size();
        for (std::int32_t k = 0; k < chunks_.totalChunks; ++k)
            total += chunks_.segments[static_cast<std::size_t>(k)].Size();
        // the scans whole (Write) or as their spliced chunk segments (Write, Splice)
        return output_.Reserve(total) && AssembleJpeg(setup_, *coding_, output_, [this](std::int32_t i, ByteBuffer& out) noexcept {
                   const auto si = static_cast<std::size_t>(i);
                   bool       ok = out.Append(scanData_[si].Data(), scanData_[si].Size());
                   for (std::int32_t k = 0; ok && k < chunks_.chunkCount[si]; ++k) {
                       const ByteBuffer& segment = chunks_.segments[static_cast<std::size_t>(chunks_.chunkBase[si] + k)];
                       ok                        = out.Append(segment.Data(), segment.Size());
                   }
                   return ok;
               });
    }

    // --- Release: frees the tokens and written data of each scan, so the kernel's cost of returning a few hundred MB of
    // touched pages is spread over the workers instead of paid serially at the end of the encode or of the process.
    void EncodeGraph::Release(ITaskScheduler& scheduler) noexcept
    {
        RunStage(scheduler, settings_.workers, setup_.scanCount, [this](std::int32_t scan, std::uint32_t) noexcept {
            const auto si = static_cast<std::size_t>(scan);
            scanTokens_[scan].parts.reset();
            scanTokens_[scan].partCount = 0;
            scanData_[scan].Reset();
            for (std::int32_t k = 0; k < chunks_.chunkCount[si]; ++k) {
                const auto ci = static_cast<std::size_t>(chunks_.chunkBase[si] + k);
                chunks_.bits[ci].bytes.Reset();
                chunks_.segments[ci].Reset();
            }
            return true;
        });
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
