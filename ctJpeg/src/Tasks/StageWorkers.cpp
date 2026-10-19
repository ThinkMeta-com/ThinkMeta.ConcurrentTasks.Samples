// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "StageWorkers.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>

import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    namespace {

        /// <summary>The jobs 0..count-1 of one stage node; its workers claim them in order.</summary>
        struct StageJobs {
            StageJob                  job     = nullptr;
            void*                     context = nullptr;
            std::int32_t              count   = 0;
            std::atomic<std::int32_t> next{0};
            std::atomic<bool>         failed{false};
        };

        /// <summary>
        /// One worker of a stage node (Pixel, Tokenize, Stitch, Count, Write, Splice, Release; see
        /// EncodeGraph.h): runs the stage's jobs until none is left.
        /// </summary>
        class StageWorkerTask : public Tasks::Task
        {
        public:
            StageWorkerTask(StageJobs& jobs, std::uint32_t worker) noexcept : jobs_(jobs), worker_(worker) {}

            void Run() noexcept override
            {
                for (;;) {
                    const std::int32_t index = jobs_.next.fetch_add(1, std::memory_order_relaxed);
                    if (index >= jobs_.count)
                        break;
                    if (!jobs_.job(jobs_.context, index, worker_))
                        jobs_.failed.store(true, std::memory_order_relaxed);
                }
            }

        private:
            StageJobs&    jobs_;
            std::uint32_t worker_;
        };

    } // namespace

    bool RunStageJobs(ITaskScheduler& scheduler, std::uint32_t workers, std::int32_t count, StageJob job, void* context) noexcept
    {
        if (count <= 0)
            return true;
        StageJobs jobs;
        jobs.job                      = job;
        jobs.context                  = context;
        jobs.count                    = count;
        const std::uint32_t taskCount = static_cast<std::uint32_t>(count) < workers ? static_cast<std::uint32_t>(count) : workers;
        // std::optional: a task cannot be moved, so each is built in place
        const auto tasks = std::unique_ptr<std::optional<StageWorkerTask>[]>(new (std::nothrow) std::optional<StageWorkerTask>[taskCount]);
        if (!tasks)
            return false;
        // fork: all workers in one group, then started; join: the group's wait. Group and tasks are local: the group
        // may go once Wait has returned and its tasks have exited (TaskGroup remarks), which Wait guarantees here.
        // One waiter: the caller, so TaskGroup_s (a join of 16 workers costs about 30 us with either variant).
        Synchronization::TaskGroup_s group;
        for (std::uint32_t i = 0; i < taskCount; ++i) {
            tasks[i].emplace(jobs, i);
            group.AddTask(&*tasks[i]);
        }
        for (std::uint32_t i = 0; i < taskCount; ++i)
            scheduler.AddTask(&*tasks[i], TaskPriority::Normal);
        group.Wait();
        return !jobs.failed.load(std::memory_order_relaxed);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
