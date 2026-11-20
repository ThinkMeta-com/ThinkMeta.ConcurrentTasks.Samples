// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "ParallelRunner.h"

#include "GroupSync.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>

import ThinkMeta.ConcurrentTasks.Synchronization.ManualResetEvent;

namespace ThinkMeta::ConcurrentTasks::Samples::Jxl {

    namespace {

        /// <summary>
        /// GroupSync on ConcurrentTasks events: one single-waiter event per turn and one per group for the gate (every
        /// group waits there once), so the object may go as soon as the group tasks have exited: the Set of a
        /// single-waiter event is one operation, a multi-waiter event may still be in use after its waiters have passed
        /// (ManualResetEvent remarks). In a test order of
        /// the runner (ParallelRunner::StartOrder) a group yields right after it ended a turn or opened the gate, so the
        /// next group can run before the group goes on, also where no wait lies between two of its steps.
        /// </summary>
        class TaskGroupSync final : public GroupSync
        {
        public:
            TaskGroupSync(std::size_t workers, std::size_t groups, std::size_t sequences,
                          std::unique_ptr<Synchronization::ManualResetEvent_s[]> turns, bool yieldAtHandOvers) noexcept
                : workers_(workers), groups_(groups), sequences_(sequences), turns_(std::move(turns)), yieldAtHandOvers_(yieldAtHandOvers)
            {
                // the first turn of every sequence is free
                for (std::size_t s = 0; s < sequences_; ++s)
                    turns_[s * groups_].Set();
            }

            std::size_t Workers() const noexcept override { return workers_; }

            bool WaitTurn(std::size_t sequence, std::size_t group) noexcept override
            {
                turns_[sequence * groups_ + group].Wait();
                return !aborted_.load(std::memory_order_acquire);
            }

            void EndTurn(std::size_t sequence, std::size_t group) noexcept override
            {
                if (group + 1 < groups_)
                    turns_[sequence * groups_ + group + 1].Set();
                if (yieldAtHandOvers_)
                    YieldTask();
            }

            bool WaitGate(std::size_t group) noexcept override
            {
                turns_[sequences_ * groups_ + group].Wait();
                return !aborted_.load(std::memory_order_acquire);
            }

            void OpenGate() noexcept override
            {
                for (std::size_t g = 1; g < groups_; ++g)
                    turns_[sequences_ * groups_ + g].Set();
                if (yieldAtHandOvers_)
                    YieldTask();
            }

            void Abort() noexcept override
            {
                aborted_.store(true, std::memory_order_release);
                for (std::size_t i = 0; i < (sequences_ + 1) * groups_; ++i)
                    turns_[i].Set();
            }

        private:
            std::size_t                                            workers_;
            std::size_t                                            groups_;
            std::size_t                                            sequences_;
            std::unique_ptr<Synchronization::ManualResetEvent_s[]> turns_; // sequences x groups turns, then the gate per group
            std::atomic<bool>                                      aborted_{false};
            bool                                                   yieldAtHandOvers_;
        };

    } // namespace

    GroupSync* GroupSync::Create(JxlParallelRunner runner, void* runnerOpaque, std::size_t groups, std::size_t sequences) noexcept
    {
        if (runner != &ParallelRunner::Run || runnerOpaque == nullptr || groups == 0 || sequences == 0)
            return nullptr;
        const auto&       parallelRunner = *static_cast<const ParallelRunner*>(runnerOpaque);
        const std::size_t workers        = parallelRunner.Workers();
        if (workers < 2)
            return nullptr;
        auto turns = std::unique_ptr<Synchronization::ManualResetEvent_s[]>(new (std::nothrow) Synchronization::ManualResetEvent_s[(sequences + 1) * groups]);
        if (!turns)
            return nullptr;
        return new (std::nothrow) TaskGroupSync(workers, groups, sequences, std::move(turns), parallelRunner.Order().variant != 0);
    }

    void GroupSync::Destroy(GroupSync* sync) noexcept { delete static_cast<TaskGroupSync*>(sync); }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jxl
