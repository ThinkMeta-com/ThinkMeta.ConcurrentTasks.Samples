// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#if defined(_WIN32)
#    include <Windows.h>
#endif

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <cstdint>
#include <memory>

#include "ParallelRunner.h"
#include "TaskRuntime.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Jxl {

    /// <summary>
    /// The compute scheduler of ctJxl and the ParallelRunner on it, as long as libjxl uses the runner. While it lives,
    /// the thread that created it is a task (ConvertThreadToTask): libjxl calls the runner from there, and the runner
    /// waits for its workers. Create, use and destroy it on the same thread; destruction stops the scheduler first,
    /// then turns the task back into a thread.
    /// <para>Preliminary.</para>
    /// </summary>
    class TaskPool
    {
    public:
        /// <summary>A pool with workers threads (0: the runner works on the caller); null if the scheduler cannot be created.</summary>
        static std::shared_ptr<TaskPool> Create(std::uint32_t workers);

        /// <summary>
        /// For tests of the task graph: a runner with workers worker tasks per region on a scheduler with threads threads,
        /// starting them in the given order (ParallelRunner::StartOrder), with taskBudget worker tasks at once (at most
        /// TaskQueueCapacity / 2; a smaller one lets the regions fork fewer tasks than workers).
        /// </summary>
        static std::shared_ptr<TaskPool> Create(std::uint32_t workers, std::uint32_t threads, ParallelRunner::StartOrder order,
                                                std::uint32_t taskBudget = TaskQueueCapacity / 2);

        TaskPool(std::uint32_t workers, std::uint32_t threads, ParallelRunner::StartOrder order, std::uint32_t taskBudget) noexcept;
        ~TaskPool();

        TaskPool(const TaskPool&)            = delete;
        TaskPool& operator=(const TaskPool&) = delete;

        /// <summary>The runner for libjxl (JxlParallelRunner: ParallelRunner::Run with this as its opaque pointer).</summary>
        ParallelRunner& Runner() noexcept { return runner_; }

        /// <summary>
        /// The scheduler's queue capacity: room for every runnable task. The runner's task budget is half of it; the rest
        /// covers the tasks outside the runner (the PNG reader) and the test orders, which fork past the budget.
        /// </summary>
        static constexpr std::uint32_t TaskQueueCapacity = 16384;

    private:
        bool                     converted_; // first member: converted before the scheduler starts, back after it stopped
        Shared::RunningScheduler scheduler_;
        ParallelRunner           runner_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jxl
