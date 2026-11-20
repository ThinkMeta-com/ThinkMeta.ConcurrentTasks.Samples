// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "ParallelRunner.h"
#include "RunnerInfo.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>

import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

namespace ThinkMeta::ConcurrentTasks::Samples::Jxl {

    namespace {

        /// <summary>
        /// The shared part of one region: the counter its workers claim the items from. 64 bit: the counter runs past the
        /// end by up to the number of workers, also when the range ends at the top of the 32-bit range.
        /// </summary>
        struct RegionItems {
            std::atomic<std::uint64_t> next{0};
            // tests only (ParallelRunner::StartOrder): not 0, a worker sets the priority of the item it claimed and yields 0 to
            // 3 times, so the items run in another order while they are still claimed in order (as libjxl's pool hands
            // them out). itemPriorities: 0 unchanged, 1 every second item High, 2 at random.
            std::uint32_t testSeed       = 0;
            std::uint32_t first          = 0;
            std::uint32_t itemPriorities = 0;
        };

        /// <summary>
        /// One worker of a region: runs the region's items until none is left. Function, opaque pointer and end are its own
        /// copies, so the only data the workers share is the counter.
        /// </summary>
        class RegionWorkerTask final : public Tasks::Task
        {
        public:
            RegionWorkerTask(RegionItems& items, JxlParallelRunFunction func, void* opaque, std::uint32_t end, std::uint32_t worker) noexcept
                : items_(items), func_(func), opaque_(opaque), end_(end), worker_(worker)
            {}

            void Run() noexcept override { RunItems(items_, func_, opaque_, end_, worker_); }

            /// <summary>The loop of a worker; the caller of the region runs it too, as worker 0.</summary>
            static void RunItems(RegionItems& items, JxlParallelRunFunction func, void* opaque, std::uint32_t end, std::uint32_t worker) noexcept
            {
                for (;;) {
                    const std::uint64_t item = items.next.fetch_add(1, std::memory_order_relaxed);
                    if (item >= end)
                        break;
                    if (items.testSeed != 0) {
                        const std::uint32_t hash = (static_cast<std::uint32_t>(item) ^ items.testSeed) * 2654435761u;
                        if (items.itemPriorities != 0) {
                            const bool high = items.itemPriorities == 1 ? ((static_cast<std::uint32_t>(item) - items.first) & 1) != 0
                                                                        : ((hash >> 16) & 1) != 0;
                            SetTaskPriority(high ? TaskPriority::High : TaskPriority::Normal);
                        }
                        for (std::uint32_t k = hash >> 30; k > 0; --k)
                            YieldTask();
                    }
                    func(opaque, static_cast<std::uint32_t>(item), worker);
                }
            }

        private:
            RegionItems&           items_;
            JxlParallelRunFunction func_;
            void*                  opaque_;
            std::uint32_t          end_;
            std::uint32_t          worker_;
        };

        /// <summary>Regions up to this many workers keep their worker tasks on the caller's stack.</summary>
        constexpr std::uint32_t StackWorkers = 64;

        std::uint32_t NextRandom(std::uint32_t& seed) noexcept
        {
            seed = seed * 1664525u + 1013904223u;
            return seed >> 8;
        }

    } // namespace

    JxlParallelRetCode ParallelRunner::Run(void* runner, void* jpegxlOpaque, JxlParallelRunInit init, JxlParallelRunFunction func,
                                           std::uint32_t startRange, std::uint32_t endRange) noexcept
    {
        // the contract of JxlParallelRunner (jxl/parallel_runner.h), as libjxl's own pool: an error of init is returned as
        // it is, a reversed range is a runner error
        const auto& self = *static_cast<const ParallelRunner*>(runner);
        if (startRange > endRange)
            return JXL_PARALLEL_RET_RUNNER_ERROR;
        const std::uint32_t count = endRange - startRange;
        if (count == 0)
            return JXL_PARALLEL_RET_SUCCESS;
        const std::uint32_t workers   = self.workers_ < count ? self.workers_ : count;
        const bool          testOrder = workers > 1 && self.order_.variant != 0;
        // the task budget (see ParallelRunner): fewer tasks, or none, once nested regions have taken it; init learns how
        // many workers really run items, so libjxl keeps scratch memory for those only. Given back on every way out,
        // after the tasks have exited (the group's wait).
        const std::uint32_t taskCount = workers <= 1 ? 0 : testOrder ? workers : self.TakeTasks(workers - 1);
        struct GiveBack {
            const ParallelRunner& runner;
            std::uint32_t         tasks;
            ~GiveBack() { runner.GiveBackTasks(tasks); }
        } giveBack{self, testOrder ? 0 : taskCount};
        if (taskCount == 0) {
            // on the caller, as libjxl's pool without threads
            const JxlParallelRetCode ret = init(jpegxlOpaque, 1);
            if (ret != JXL_PARALLEL_RET_SUCCESS)
                return ret;
            for (std::uint32_t i = startRange; i < endRange; ++i)
                func(jpegxlOpaque, i, 0);
            return JXL_PARALLEL_RET_SUCCESS;
        }
        if (const JxlParallelRetCode ret = init(jpegxlOpaque, testOrder ? taskCount : taskCount + 1); ret != JXL_PARALLEL_RET_SUCCESS)
            return ret;

        RegionItems items;
        items.next.store(startRange, std::memory_order_relaxed);
        // the caller is worker 0, tasks are workers 1 .. taskCount; in a test order (StartOrder) the caller only waits and
        // all workers are tasks. std::optional: a task cannot be moved, so each is built in place; on the stack up to
        // StackWorkers - 1, beyond that on the heap
        const std::uint32_t firstTask = testOrder ? 0 : 1;
        std::optional<RegionWorkerTask>                    stackTasks[StackWorkers - 1];
        std::unique_ptr<std::optional<RegionWorkerTask>[]> heapTasks;
        std::optional<RegionWorkerTask>*                   tasks = stackTasks;
        if (taskCount > StackWorkers - 1) {
            heapTasks.reset(new (std::nothrow) std::optional<RegionWorkerTask>[taskCount]);
            if (!heapTasks)
                return JXL_PARALLEL_RET_RUNNER_ERROR;
            tasks = heapTasks.get();
        }
        // fork: all tasks in one group, then started; the caller runs items meanwhile; join: the group's wait (one
        // waiter: the caller). Group, tasks and items are local: Wait returns after every task has exited.
        Synchronization::TaskGroup_s group;
        for (std::uint32_t k = 0; k < taskCount; ++k) {
            tasks[k].emplace(items, func, jpegxlOpaque, endRange, k + firstTask);
            group.AddTask(&*tasks[k]);
        }
        if (testOrder) {
            // the start order (ParallelRunner::StartOrder): 1 reversed, else a shuffle seeded by the variant and the region
            std::uint32_t order[StackWorkers];
            const std::uint32_t ordered = taskCount < StackWorkers ? taskCount : StackWorkers;
            for (std::uint32_t i = 0; i < ordered; ++i)
                order[i] = self.order_.variant == 1 ? ordered - 1 - i : i;
            std::uint32_t seed = self.order_.variant * 2654435761u + self.regions_.fetch_add(1, std::memory_order_relaxed) * 2246822519u;
            if (self.order_.variant >= 2) {
                for (std::uint32_t i = ordered; i > 1; --i) {
                    const std::uint32_t j = NextRandom(seed) % i;
                    const std::uint32_t t = order[i - 1];
                    order[i - 1]          = order[j];
                    order[j]              = t;
                }
            }
            // before the first task starts
            items.testSeed       = seed | 1;
            items.first          = startRange;
            items.itemPriorities = self.order_.randomPriorities ? 2 : self.order_.variant == 1 ? 1 : 0;
            for (std::uint32_t i = 0; i < taskCount; ++i) {
                const std::uint32_t k        = i < ordered ? order[i] : i; // beyond StackWorkers in their order
                TaskPriority        priority = TaskPriority::Default;
                if (self.order_.randomPriorities)
                    priority = (NextRandom(seed) & 1) != 0 ? TaskPriority::High : TaskPriority::Normal;
                self.scheduler_->AddTask(&*tasks[k], priority);
            }
            group.Wait();
            return JXL_PARALLEL_RET_SUCCESS;
        }
        for (std::uint32_t k = 0; k < taskCount; ++k)
            self.scheduler_->AddTask(&*tasks[k], TaskPriority::Default); // the caller's priority
        RegionWorkerTask::RunItems(items, func, jpegxlOpaque, endRange, 0);
        group.Wait();
        return JXL_PARALLEL_RET_SUCCESS;
    }

    std::uint32_t ParallelRunner::TakeTasks(std::uint32_t wanted) const noexcept
    {
        std::uint32_t free = freeTasks_.load(std::memory_order_relaxed);
        while (free > 0) {
            const std::uint32_t taken = free < wanted ? free : wanted;
            if (freeTasks_.compare_exchange_weak(free, free - taken, std::memory_order_relaxed))
                return taken;
        }
        return 0;
    }

    bool RunnerNestsRegions(JxlParallelRunner runner, void* runnerOpaque) noexcept
    {
        return runner == &ParallelRunner::Run && runnerOpaque != nullptr &&
               static_cast<const ParallelRunner*>(runnerOpaque)->Workers() >= 2;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jxl
