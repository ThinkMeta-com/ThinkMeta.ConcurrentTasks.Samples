// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "ParallelRunner.h"

#include "InputBands.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>

import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.ManualResetEvent;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

namespace ThinkMeta::ConcurrentTasks::Samples::Jxl {

    namespace {

        class TaskInputBands;

        /// <summary>The reader: runs the read function once, then reports to the bands.</summary>
        class ReaderTask final : public Tasks::Task
        {
        public:
            explicit ReaderTask(TaskInputBands& bands) noexcept : bands_(bands) {}

            void Run() noexcept override;

            bool (*read)(void* context) = nullptr;
            void* context                = nullptr;

        private:
            TaskInputBands& bands_;
        };

        /// <summary>
        /// InputBands on ConcurrentTasks events, one per band; the reader task in a task group for the join. In a test order
        /// of the runner (ParallelRunner::StartOrder) the reader runs with Normal priority and yields after each band it
        /// opened, so on one thread the groups encode while the input is still being read.
        /// </summary>
        class TaskInputBands final : public InputBands
        {
        public:
            TaskInputBands(ITaskScheduler* scheduler, std::size_t rows, std::size_t bandRows, std::size_t bands,
                           std::unique_ptr<Synchronization::ManualResetEvent[]> events, bool testOrder) noexcept
                : scheduler_(scheduler), rows_(rows), bandRows_(bandRows), bands_(bands), events_(std::move(events)), testOrder_(testOrder),
                  reader_(*this)
            {}

            bool StartReader(bool (*read)(void* context), void* context) noexcept override
            {
                if (started_)
                    return false;
                reader_.read    = read;
                reader_.context = context;
                started_        = true;
                group_.AddTask(&reader_);
                // High: the reader is on the critical path of every group
                scheduler_->AddTask(&reader_, testOrder_ ? TaskPriority::Normal : TaskPriority::High);
                return true;
            }

            void Publish(std::size_t rows) noexcept override
            {
                // open every band whose rows are all complete
                const std::size_t complete = rows >= rows_ ? bands_ : rows / bandRows_;
                const bool        opened   = opened_ < complete;
                while (opened_ < complete)
                    events_[opened_++].Set();
                if (testOrder_ && opened)
                    YieldTask();
            }

            bool WaitRows(std::size_t rows) noexcept override
            {
                if (rows == 0)
                    return !aborted_.load(std::memory_order_acquire);
                const std::size_t band = (rows > rows_ ? rows_ : rows) - 1;
                events_[band / bandRows_].Wait();
                return !aborted_.load(std::memory_order_acquire);
            }

            void Abort() noexcept override
            {
                aborted_.store(true, std::memory_order_release);
                for (std::size_t b = 0; b < bands_; ++b)
                    events_[b].Set();
            }

            bool JoinReader() noexcept override
            {
                if (!started_)
                    return false;
                if (!joined_) {
                    group_.Wait(); // returns after the reader task has exited
                    joined_ = true;
                }
                return succeeded_ && !aborted_.load(std::memory_order_acquire);
            }

            /// <summary>Called by the reader task when read returned.</summary>
            void ReaderDone(bool succeeded) noexcept
            {
                succeeded_ = succeeded;
                if (succeeded)
                    Publish(rows_);
                else
                    Abort();
            }

        private:
            ITaskScheduler*                                      scheduler_;
            std::size_t                                          rows_;
            std::size_t                                          bandRows_;
            std::size_t                                          bands_;
            std::unique_ptr<Synchronization::ManualResetEvent[]> events_;
            bool                                                 testOrder_;
            std::size_t                                          opened_ = 0; // written by the reader only
            std::atomic<bool>                                    aborted_{false};
            bool                                                 succeeded_ = false;
            bool                                                 started_   = false;
            bool                                                 joined_    = false;
            Synchronization::TaskGroup_s                         group_;
            ReaderTask                                           reader_;
        };

        void ReaderTask::Run() noexcept { bands_.ReaderDone(read(context)); }

    } // namespace

    InputBands* InputBands::Create(JxlParallelRunner runner, void* runnerOpaque, std::size_t rows, std::size_t bandRows) noexcept
    {
        if (runner != &ParallelRunner::Run || runnerOpaque == nullptr || rows == 0 || bandRows == 0)
            return nullptr;
        const auto&           parallelRunner = *static_cast<const ParallelRunner*>(runnerOpaque);
        ITaskScheduler* const scheduler      = parallelRunner.Scheduler();
        if (scheduler == nullptr)
            return nullptr;
        const std::size_t bands  = (rows + bandRows - 1) / bandRows;
        auto              events = std::unique_ptr<Synchronization::ManualResetEvent[]>(new (std::nothrow) Synchronization::ManualResetEvent[bands]);
        if (!events)
            return nullptr;
        return new (std::nothrow) TaskInputBands(scheduler, rows, bandRows, bands, std::move(events), parallelRunner.Order().variant != 0);
    }

    void InputBands::Destroy(InputBands* bands) noexcept
    {
        if (bands == nullptr)
            return;
        auto* const self = static_cast<TaskInputBands*>(bands);
        [[maybe_unused]] const bool read = self->JoinReader(); // the reader task must have exited before its object goes
        delete self;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jxl
