// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#if defined(_WIN32)
#    include <Windows.h>
#endif

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <jxl/parallel_runner.h>

#include <atomic>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Jxl {

    /// <summary>
    /// libjxl's parallel runner (JxlParallelRunner) on ConcurrentTasks: every data-parallel region of libjxl
    /// (RunOnPool) becomes a fork-join of worker tasks on the scheduler.
    /// <para>Fork: K = min(workers, items) workers (fewer when the task budget below is used up), the caller as worker 0
    /// and K - 1 worker tasks; init(K) first, then
    /// each worker claims the items in order from a shared counter and calls func(item, k) with its own number k.
    /// libjxl keeps per-thread scratch memory by that number, and a worker runs its items one after another, so two
    /// items of one k never overlap, also when a task moves to another thread. Join: the caller waits in a
    /// TaskGroup_s, so it must be a task (or a thread converted with ConvertThreadToTask); calls from inside a worker
    /// (nested regions) are allowed. The worker tasks live on the caller's stack up to 64 workers.</para>
    /// <para>Without workers (0, --num_threads 0) or with one item the region runs on the caller, as libjxl's own pool
    /// does without threads.</para>
    /// <para>Items are claimed in ascending order, also in the test orders (StartOrder). libjxl's runner contract does not
    /// promise that; ctJxl's own dependencies rely on it (the GroupSync turns: an item never waits for a turn of an item
    /// that nobody has claimed yet).</para>
    /// <para>Task budget: nested regions multiply their worker tasks (frames x subtrees x properties), and every worker
    /// task can be runnable at once, while the scheduler's queue has a fixed capacity (filling it is invalid use). So at
    /// most taskBudget worker tasks live at a time; a region that finds the budget used up forks fewer tasks, down to
    /// none, and the caller runs the rest of the items itself. The bytes do not change: libjxl's output does not depend
    /// on which worker runs an item, and the items are still claimed in order. libjxl's init is told the workers that
    /// really run items. The test orders ignore the budget (their caller runs no items); with three nested levels their
    /// workers stay within the queue up to about 25 per region.</para>
    /// <para>Preliminary.</para>
    /// </summary>
    class ParallelRunner
    {
    public:
        /// <summary>
        /// For tests of the task graph (ctJxl.Tests): how a region runs its items. Variant 0 is the normal run (the other
        /// field is ignored); otherwise the caller runs no items itself, it only waits, all K workers are tasks and start
        /// in another order (1 reversed, else
        /// shuffled with the variant and the region as the seed), and a worker yields 0 to 3 times after it claimed an
        /// item. The items are still claimed in order, as libjxl's pool hands them out. Item priorities: variant 1 runs
        /// every second item (offset 1, 3, ...) with High priority, so a later item overtakes the one before;
        /// randomPriorities gives each item and each worker task High or Normal priority at random. On a scheduler with
        /// one thread this varies the order in which the items run, through every region, nested ones included. GroupSync
        /// and InputBands yield at their hand-overs too (see there). An empty initializer ({}) is the normal run (no
        /// default member initializers: clang does not take them in a default argument of the enclosing class).
        /// </summary>
        struct StartOrder {
            std::uint32_t variant;
            bool          randomPriorities;
        };

        /// <summary>
        /// A runner that forks at most workers tasks per region on scheduler (which may be null when workers is 0) and at
        /// most taskBudget worker tasks in all regions at once.
        /// </summary>
        ParallelRunner(ITaskScheduler* scheduler, std::uint32_t workers, std::uint32_t taskBudget, StartOrder order = {}) noexcept
            : scheduler_(scheduler), workers_(scheduler ? workers : 0), order_(order), freeTasks_(taskBudget)
        {}

        /// <summary>The JxlParallelRunner function; runner is the ParallelRunner.</summary>
        static JxlParallelRetCode Run(void* runner, void* jpegxlOpaque, JxlParallelRunInit init, JxlParallelRunFunction func,
                                      std::uint32_t startRange, std::uint32_t endRange) noexcept;

        /// <summary>The most worker tasks a region forks (0: regions run on the caller).</summary>
        [[nodiscard]] std::uint32_t Workers() const noexcept { return workers_; }

        /// <summary>The test order of the regions (variant 0: the normal run).</summary>
        [[nodiscard]] const StartOrder& Order() const noexcept { return order_; }

        /// <summary>The scheduler of the worker tasks (null without workers).</summary>
        [[nodiscard]] ITaskScheduler* Scheduler() const noexcept { return workers_ ? scheduler_ : nullptr; }

    private:
        /// <summary>Takes up to wanted worker tasks from the budget; returns how many (0 when it is used up).</summary>
        std::uint32_t TakeTasks(std::uint32_t wanted) const noexcept;

        /// <summary>Gives tasks taken with TakeTasks back once they have exited.</summary>
        void GiveBackTasks(std::uint32_t tasks) const noexcept { freeTasks_.fetch_add(tasks, std::memory_order_relaxed); }

        ITaskScheduler*                    scheduler_;
        std::uint32_t                      workers_;
        StartOrder                         order_;
        mutable std::atomic<std::uint32_t> regions_{0}; // seeds the shuffled start orders
        mutable std::atomic<std::uint32_t> freeTasks_;  // the task budget not taken
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jxl
