//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#include "DeflateGraph.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include "Gzip.h"

import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate {

    namespace {

        /// <summary>Output past the end of the bitstream: the byte alignment, the gzip trailer and the bit writer's 8-byte stores.</summary>
        constexpr std::size_t TailSlack = 16;

        double Seconds(DeflateClock::time_point from, DeflateClock::time_point to)
        {
            return std::chrono::duration<double>(to - from).count();
        }

    } // namespace

    DeflateGraph::DeflateGraph(const Settings& settings) noexcept : settings_(settings), params_(LevelParams::For(settings.level))
    {
        if (settings_.workers == 0)
            settings_.workers = 1;
    }

    std::size_t DeflateGraph::OutputBound() const noexcept
    {
        return Compressor::GzipBound(input_.Size()) + TailSlack;
    }

    DeflateGraph::Result DeflateGraph::Run(ITaskScheduler& io, ITaskScheduler& compute, ParallelStats* stats) noexcept
    {
        Result result;
        result.inSize = input_.Size();

        // Write runs as long as the whole graph: it writes the output as the compute nodes finish it.
        Synchronization::TaskGroup_s writing; // one waiter: this task
        if (!output_.Attached()) {
            write_.emplace(output_);
            writing.AddTask(&*write_);
            io.AddTask(&*write_, TaskPriority::Normal);
        }
        if (settings_.sequential || params_.mode == ParseMode::None || input_.Size() <= params_.maxPassthroughSize)
            RunSequential(io, compute, result); // stored, or too small to split
        else
            RunParallel(io, compute, result, stats);
        output_.CompleteAll(static_cast<std::size_t>(result.outSize));
        writing.Wait();

        result.error = input_.ReadError();
        if (result.error == 0)
            result.error = output_.Error();
        return result;
    }

    Tasks::Task* DeflateGraph::StartRead() noexcept
    {
        if (!input_.Streaming())
            return nullptr;
        return &read_.emplace(input_);
    }

    void DeflateGraph::RunSequential(ITaskScheduler& io, ITaskScheduler& compute, Result& result) noexcept
    {
        if (input_.Streaming())
            input_.Allow((std::min)(input_.Size(), SequentialCompressTask::ReadAhead));
        Tasks::Task* read = StartRead();
        sequential_.emplace(settings_.level, input_, output_);

        Synchronization::TaskGroup_s group; // one waiter: this task
        if (read != nullptr)
            group.AddTask(read);
        group.AddTask(&*sequential_);
        if (read != nullptr)
            io.AddTask(read, TaskPriority::Normal);
        compute.AddTask(&*sequential_, TaskPriority::Normal);
        group.Wait();

        result.outSize = sequential_->OutSize();
        result.crc     = sequential_->Crc();
    }

    void DeflateGraph::RunParallel(ITaskScheduler& io, ITaskScheduler& compute, Result& result, ParallelStats* stats) noexcept
    {
        const auto        t0     = DeflateClock::now();
        const std::size_t inSize = input_.Size();

        // --- edges
        const ChunkOptions& options = settings_.chunks;
        logs_.emplace();
        logs_->Plan(PlanChunks(inSize, options.chunkSize), inSize, options,
                    options.ahead != 0 ? options.ahead : static_cast<std::size_t>(settings_.workers) + 4);
        if (input_.Streaming()) {
            // chunk k's parse reads to the next chunk's start, plus the overlap, plus a match that crosses its limit
            const auto&              starts = logs_->Starts();
            std::vector<std::size_t> needs(starts.size());
            for (std::size_t k = 0; k < needs.size(); ++k)
                needs[k] = k + 1 < needs.size() ? (std::min)(inSize, starts[k + 1] + options.overlap + InputReach::Parse) : inSize;
            input_.PlanChunks(std::move(needs));
            input_.Allow(ResolveNode::WindowEnd(input_, *logs_, 0));
        }
        ring_.emplace();
        output_.Commit(SegmentSize);
        std::uint8_t*     out = output_.Data();
        const std::size_t pos = WriteGzipHeader(settings_.level, out);
        bits_.emplace(out + pos, out + Compressor::GzipBound(inSize));

        // --- nodes
        Tasks::Task* read = StartRead();
        stitch_.emplace(input_, *logs_, *ring_, output_, *bits_);
        workers_ = std::make_unique<std::optional<WorkerTask>[]>(settings_.workers);
        for (std::uint32_t i = 0; i < settings_.workers; ++i)
            workers_[i].emplace(params_, input_, *logs_, *ring_, *stitch_, work_);
        resolve_.emplace(params_, input_, *logs_, *ring_, *stitch_, work_, settings_.workers);

        // --- start: Read on the I/O scheduler, the workers on the compute scheduler, Resolve right here
        Synchronization::TaskGroup_s group; // one waiter: this task
        if (read != nullptr)
            group.AddTask(read);
        for (std::uint32_t i = 0; i < settings_.workers; ++i)
            group.AddTask(&*workers_[i]);
        if (read != nullptr)
            io.AddTask(read, TaskPriority::Normal);
        for (std::uint32_t i = 0; i < settings_.workers; ++i)
            compute.AddTask(&*workers_[i], TaskPriority::Normal);
        // The serial node runs on the calling task, a thread of its own beside the workers' threads: as a task on the
        // scheduler it would share their threads, which measured up to 9 % slower with 2 threads (equal with 16).
        resolve_->Run();
        group.Wait();
        const auto t2 = DeflateClock::now();

        // --- the end of the stream
        BitWriter& os = *bits_;
        output_.Commit(static_cast<std::size_t>(os.Next() - out) + TailSlack);
        os.AlignToByte();
        std::size_t end = static_cast<std::size_t>(os.Next() - out);
        result.crc      = logs_->Crc();
        end += WriteGzipTrailer(result.crc, inSize, out + end);
        result.outSize = end;
        const auto t3  = DeflateClock::now();

        if (stats != nullptr) {
            stats->resolve      = resolve_->Stats();
            stats->chunks       = logs_->Chunks();
            stats->blocks       = ring_->Published();
            stats->parseResolve = Seconds(t0, resolve_->Resolved());
            stats->resolveWait  = resolve_->Waited();
            stats->encode       = Seconds(resolve_->Resolved(), t2); // the encoding and stitching left after Resolve
            stats->stitch       = Seconds(t2, t3);
        }
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate
