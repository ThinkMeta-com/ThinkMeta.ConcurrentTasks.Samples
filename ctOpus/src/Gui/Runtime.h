// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <Windows.h>

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <atomic>
#include <cstdint>

#include "Common.h"
#include "SchedulerHandle.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::Gui {

    /// <summary>Puts the calling thread into the MMCSS "Pro Audio" class (falls back to "Audio"); nullptr if neither works.</summary>
    HANDLE EnterProAudio() noexcept;
    void   LeaveProAudio(HANDLE mmcss) noexcept;

    /// <summary>A scheduler whose threads join MMCSS "Pro Audio" (plan 3.3: MMCSS through the thread callbacks) if mmcss is set.</summary>
    Shared::SchedulerHandle CreateAudioScheduler(std::uint32_t threads, bool mmcss) noexcept;

    /// <summary>
    /// Counts the tasks that are still running and signals a Win32 event when the last one has
    /// exited, so that a thread outside the scheduler (the UI thread) can wait for them.
    /// </summary>
    class TaskExitCounter final : public INotifyTaskStateChange
    {
    public:
        TaskExitCounter() noexcept : done_(CreateEventW(nullptr, TRUE, TRUE, nullptr)) {}

        void Add(ITask* task) noexcept
        {
            if (count_.fetch_add(1, std::memory_order_relaxed) == 0)
                ResetEvent(done_.Get());
            task->SetNotifyCallback(this);
        }

        void OnTaskExited() noexcept override
        {
            if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1)
                SetEvent(done_.Get());
        }

        /// <summary>True if all tasks have exited within the timeout.</summary>
        bool Wait(unsigned long timeoutMs) const noexcept { return WaitForSingleObject(done_.Get(), timeoutMs) == WAIT_OBJECT_0; }

    private:
        std::atomic<std::int64_t> count_{0};
        Handle                    done_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::Gui
