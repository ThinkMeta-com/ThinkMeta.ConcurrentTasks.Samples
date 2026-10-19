// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <cstdint>
#include <type_traits>

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    /// <summary>A job of a stage, type-erased: job index on the given worker; false if it failed.</summary>
    using StageJob = bool (*)(void* context, std::int32_t index, std::uint32_t worker) noexcept;

    /// <summary>The fork-join behind RunStage: job(context, i, worker) for i = 0..count-1.</summary>
    bool RunStageJobs(ITaskScheduler& scheduler, std::uint32_t workers, std::int32_t count, StageJob job, void* context) noexcept;

    /// <summary>
    /// Runs one stage node of the graph (EncodeGraph.h) as a fork-join. Fork: one worker task per worker (at most one per
    /// job) on the scheduler; each claims the jobs 0..count-1 in order until none is left and calls job(index, worker).
    /// Join: returns when every job is done; false if a job returned false. The worker index (0..workers-1) selects
    /// per-worker scratch memory: a worker task runs its jobs one after another (even when a job waits), so two jobs
    /// of one worker never overlap.
    /// The caller waits for a TaskGroup_s, so it must be a task (or a thread converted with ConvertThreadToTask).
    /// </summary>
    template <typename Job>
    bool RunStage(ITaskScheduler& scheduler, std::uint32_t workers, std::int32_t count, Job&& job) noexcept
    {
        using J = std::remove_reference_t<Job>;
        return RunStageJobs(
            scheduler, workers, count,
            [](void* context, std::int32_t index, std::uint32_t worker) noexcept -> bool { return (*static_cast<J*>(context))(index, worker); }, &job);
    }

    /// <summary>
    /// RunStage with side jobs claimed first: sideJob(i) for i = 0..sideCount-1, then job(index, worker) for index =
    /// 0..count-1. The graph uses them to free a large buffer of the previous stage while the workers do the stage's
    /// real work, so the kernel returns its pages in parallel instead of at the end.
    /// </summary>
    template <typename SideJob, typename Job>
    bool RunStageWithSideJobs(ITaskScheduler& scheduler, std::uint32_t workers, std::int32_t sideCount, SideJob&& sideJob, std::int32_t count,
                              Job&& job) noexcept
    {
        return RunStage(scheduler, workers, sideCount + count, [&](std::int32_t index, std::uint32_t worker) noexcept {
            if (index < sideCount) {
                sideJob(index);
                return true;
            }
            return job(index - sideCount, worker);
        });
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
