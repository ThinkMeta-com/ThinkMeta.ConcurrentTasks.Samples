// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "EncodeGraph.h"

#include <cstdint>
#include <memory>
#include <new>

#include "AsyncInput.h"
#include "AsyncOutput.h"
#include "Markers.h"
#include "StageWorkers.h"

import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    namespace {

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
        SequentialEncodeTask       encode(image_, settings_.options, settings_.simd, output_);
        Synchronization::TaskGroup group;
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
        if (!Pixel(scheduler))
            return EncodeStatus::OutOfMemory;
        const double t2 = Now();
        if (!Tokenize(scheduler) || !Stitch(scheduler))
            return EncodeStatus::OutOfMemory;
        const double t3 = Now();
        Huffman();
        const double t4 = Now();
        if (!Count(scheduler))
            return EncodeStatus::OutOfMemory;
        chunks_.PlaceCursors(setup_, scanTokens_.get()); // serial, between Count and Write
        if (!Write(scheduler) || !Splice(scheduler))
            return EncodeStatus::OutOfMemory;
        const double t5 = Now();
        if (!Assemble())
            return EncodeStatus::OutOfMemory;
        const double               t6 = Now();
        Synchronization::TaskGroup writing;
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
        releaseInput_ = settings_.split.releaseInput == &image_ ? settings_.split.releaseInput : nullptr;
        return EncodeStatus::Ok;
    }

    // --- Pixel: tiles of bands (iMCU rows) and strips (iMCU columns).
    bool EncodeGraph::Pixel(ITaskScheduler& scheduler) noexcept
    {
        return RunStage(scheduler, settings_.workers, (setup_.iMcuRows + bandRows_ - 1) / bandRows_ * strips_, PixelJob, this);
    }

    bool EncodeGraph::PixelJob(void* graph, std::int32_t index, std::uint32_t worker) noexcept
    {
        auto&              g     = *static_cast<EncodeGraph*>(graph);
        const std::int32_t strip = index % g.strips_;
        const std::int32_t band  = index / g.strips_;
        const std::int32_t r0    = band * g.bandRows_;
        const std::int32_t r1    = r0 + g.bandRows_ < g.setup_.iMcuRows ? r0 + g.bandRows_ : g.setup_.iMcuRows;
        const std::int32_t c0    = strip * g.stripCols_;
        const std::int32_t c1    = c0 + g.stripCols_ < g.setup_.iMcuCols ? c0 + g.stripCols_ : g.setup_.iMcuCols;
        if (g.settings_.input != nullptr) {
            // the band reads its rows plus 5 rows of context below (ComputeBand)
            const std::int32_t rows = r1 * DctSize * g.setup_.maxV + 8 < g.image_.Height() ? r1 * DctSize * g.setup_.maxV + 8 : g.image_.Height();
            g.settings_.input->WaitFor(static_cast<std::size_t>(g.image_.Row(0) - g.settings_.input->Data())
                                       + static_cast<std::size_t>(rows) * g.image_.Stride());
            if (g.settings_.input->Failed())
                return false;
        }
        return ComputeCoefficients(g.setup_, g.image_, g.settings_.simd, r0, r1, c0, c1, g.coefficients_, g.scratch_[worker]);
    }

    // --- Tokenize: whole scans, AC bands through all scans of their component, DC bands; the first
    // job frees the input image.
    bool EncodeGraph::Tokenize(ITaskScheduler& scheduler) noexcept
    {
        return RunStage(scheduler, settings_.workers, bands_.jobCount + (releaseInput_ != nullptr || settings_.input != nullptr ? 1 : 0), TokenizeJob, this);
    }

    bool EncodeGraph::TokenizeJob(void* graph, std::int32_t index, std::uint32_t) noexcept
    {
        auto& g = *static_cast<EncodeGraph*>(graph);
        if (g.releaseInput_ != nullptr || g.settings_.input != nullptr) {
            if (index == 0) {
                if (g.releaseInput_ != nullptr)
                    g.releaseInput_->Reset();
                if (g.settings_.input != nullptr)
                    g.settings_.input->Release();
                return true;
            }
            --index;
        }
        const TokenBands::Job job = g.bands_.jobs[static_cast<std::size_t>(index)];
        if (job.scan >= 0 && job.band >= 0)
            return TokenizeDcBand(g.setup_, g.coefficients_, job.scan, job.rowBegin, job.rowEnd, g.scanTokens_[job.scan].parts[job.band],
                                  g.bands_.dcHistograms[static_cast<std::size_t>(job.scan)][job.band]);
        if (job.band < 0) {
            if (!TokenizeScan(g.setup_, g.coefficients_, job.scan, g.scanTokens_[job.scan]))
                return false;
            AddScanHistograms(g.setup_, job.scan, g.scanTokens_[job.scan], g.histograms_[job.scan]);
            return true;
        }
        const auto comp = static_cast<std::size_t>(job.component);
        for (std::int32_t i = 0; i < g.bands_.componentScanCount[comp]; ++i) {
            const std::int32_t scan = g.bands_.componentScans[comp][static_cast<std::size_t>(i)];
            if (!TokenizeScanBand(g.setup_, g.coefficients_, scan, job.rowBegin, job.rowEnd, g.scanTokens_[scan].parts[2 * job.band + 1],
                                  g.bands_.bandResults[static_cast<std::size_t>(scan)][job.band]))
                return false;
        }
        return true;
    }

    // --- Stitch: joins the bands of each banded scan (EOB runs across band borders, histograms).
    bool EncodeGraph::Stitch(ITaskScheduler& scheduler) noexcept
    {
        return RunStage(scheduler, settings_.workers, bands_.stitchCount, StitchJob, this);
    }

    bool EncodeGraph::StitchJob(void* graph, std::int32_t index, std::uint32_t) noexcept
    {
        auto&              g    = *static_cast<EncodeGraph*>(graph);
        const std::int32_t scan = g.bands_.stitchScans[static_cast<std::size_t>(index)];
        if (g.bands_.dcHistograms[static_cast<std::size_t>(scan)]) { // DC bands: only the histograms
            auto& sum = g.histograms_[scan].count;
            for (std::int32_t b = 0; b < g.bands_.scanBands[static_cast<std::size_t>(scan)]; ++b) {
                const auto& band = g.bands_.dcHistograms[static_cast<std::size_t>(scan)][b].count;
                for (std::size_t ctx = 0; ctx < MaxComponents; ++ctx)
                    for (std::size_t sym = 0; sym < HuffmanSymbols; ++sym)
                        sum[ctx][sym] += band[ctx][sym];
            }
            return true;
        }
        return StitchScanBands(g.setup_, scan, g.bands_.scanBands[static_cast<std::size_t>(scan)], g.scanTokens_[scan],
                               g.bands_.bandResults[static_cast<std::size_t>(scan)].get(), g.histograms_[scan]);
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

    // --- Count: the correction bits and run lengths each chunk of a refinement scan reads.
    bool EncodeGraph::Count(ITaskScheduler& scheduler) noexcept
    {
        return RunStage(scheduler, settings_.workers, chunks_.countChunkCount, CountJob, this);
    }

    bool EncodeGraph::CountJob(void* graph, std::int32_t index, std::uint32_t) noexcept
    {
        auto&      g  = *static_cast<EncodeGraph*>(graph);
        const auto ci = static_cast<std::size_t>(g.chunks_.countChunks[static_cast<std::size_t>(index)]);
        CountRefinementStreams(g.scanTokens_[g.chunks_.scanOf[ci]], g.chunks_.partBegin[ci], g.chunks_.partEnd[ci], g.chunks_.refBitCounts[ci],
                               g.chunks_.runCounts[ci]);
        return true;
    }

    // --- Write: the chunks of the banded scans as plain bit streams, the other scans whole; its
    // first jobs free the coefficient planes, which the writing does not read, so the kernel
    // returns their pages while the other workers write.
    bool EncodeGraph::Write(ITaskScheduler& scheduler) noexcept
    {
        return RunStage(scheduler, settings_.workers, setup_.frame.components + chunks_.jobCount, WriteJob, this);
    }

    bool EncodeGraph::WriteJob(void* graph, std::int32_t index, std::uint32_t) noexcept
    {
        auto&              g     = *static_cast<EncodeGraph*>(graph);
        const std::int32_t comps = g.setup_.frame.components;
        if (index < comps) {
            g.coefficients_.ReleaseComponent(index);
            return true;
        }
        const ScanChunks::Job job = g.chunks_.jobs[static_cast<std::size_t>(index - comps)];
        if (job.chunk < 0)
            return WriteScanData(g.setup_, job.scan, g.scanTokens_[job.scan], *g.coding_, g.scanData_[job.scan]);
        const auto ci = static_cast<std::size_t>(job.chunk);
        return WriteScanChunk(g.setup_, job.scan, g.scanTokens_[job.scan], job.partBegin, job.partEnd, g.chunks_.cursors[ci], *g.coding_, g.chunks_.bits[ci]);
    }

    // --- Splice: the stuffed bytes that start in each chunk, across the chunk borders.
    bool EncodeGraph::Splice(ITaskScheduler& scheduler) noexcept
    {
        return RunStage(scheduler, settings_.workers, chunks_.totalChunks, SpliceJob, this);
    }

    bool EncodeGraph::SpliceJob(void* graph, std::int32_t chunk, std::uint32_t) noexcept
    {
        auto&              g    = *static_cast<EncodeGraph*>(graph);
        const std::int32_t scan = g.chunks_.scanOf[static_cast<std::size_t>(chunk)];
        const std::int32_t base = g.chunks_.chunkBase[static_cast<std::size_t>(scan)];
        return SpliceScanChunk(g.chunks_.bits.get() + base, g.chunks_.chunkCount[static_cast<std::size_t>(scan)], chunk - base,
                               g.chunks_.segments[static_cast<std::size_t>(chunk)]);
    }

    // --- Assemble (serial): SOI, DQT, SOF, per scan DHT/SOS and its data, EOI (as EncodeFromCoefficients).
    bool EncodeGraph::Assemble() noexcept
    {
        std::size_t total = 4096;
        for (std::int32_t i = 0; i < setup_.scanCount; ++i)
            total += scanData_[static_cast<std::size_t>(i)].Size();
        for (std::int32_t k = 0; k < chunks_.totalChunks; ++k)
            total += chunks_.segments[static_cast<std::size_t>(k)].Size();
        bool baseline = false;
        bool ok       = output_.Reserve(total) && WriteSoi(output_) && WriteDqt(output_, setup_.frame, setup_.quant.data(), baseline)
            && WriteSof(output_, setup_.frame, false);
        std::int32_t nextDhtIndex = 0;
        for (std::int32_t i = 0; ok && i < setup_.scanCount; ++i) {
            const auto si = static_cast<std::size_t>(i);
            ok            = WriteScanHeader(setup_, i, *coding_, nextDhtIndex, output_) && output_.Append(scanData_[si].Data(), scanData_[si].Size());
            for (std::int32_t k = 0; ok && k < chunks_.chunkCount[si]; ++k) {
                const ByteBuffer& segment = chunks_.segments[static_cast<std::size_t>(chunks_.chunkBase[si] + k)];
                ok                        = output_.Append(segment.Data(), segment.Size());
            }
        }
        return ok && WriteEoi(output_);
    }

    // --- Release: frees the tokens and written data of each scan, so the kernel's cost of
    // returning a few hundred MB of touched pages is spread over the workers instead of paid
    // serially at the end of the encode or of the process.
    void EncodeGraph::Release(ITaskScheduler& scheduler) noexcept
    {
        RunStage(scheduler, settings_.workers, setup_.scanCount, ReleaseJob, this);
    }

    bool EncodeGraph::ReleaseJob(void* graph, std::int32_t scan, std::uint32_t) noexcept
    {
        auto&      g  = *static_cast<EncodeGraph*>(graph);
        const auto si = static_cast<std::size_t>(scan);
        g.scanTokens_[scan].parts.reset();
        g.scanTokens_[scan].partCount = 0;
        g.scanData_[scan].Reset();
        for (std::int32_t k = 0; k < g.chunks_.chunkCount[si]; ++k) {
            const auto ci = static_cast<std::size_t>(g.chunks_.chunkBase[si] + k);
            g.chunks_.bits[ci].bytes.Reset();
            g.chunks_.segments[ci].Reset();
        }
        return true;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
