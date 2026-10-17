// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    /// <summary>The work of one job of a stage node: job index on the given worker; false if it failed (out of memory).</summary>
    using StageJob = bool (*)(void* context, std::int32_t index, std::uint32_t worker) noexcept;

    /// <summary>
    /// Runs one stage node of the graph (EncodeGraph.h) as a fork-join: forks one worker task per
    /// worker (at most one per job) on the scheduler, each claims the jobs 0..count-1 in order until
    /// none is left, and joins them: returns when every job is done; false if a job failed. The
    /// caller must run as a task; its waiting suspends it instead of blocking its thread.
    /// </summary>
    bool RunStage(ITaskScheduler& scheduler, std::uint32_t workers, std::int32_t count, StageJob job, void* context) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
