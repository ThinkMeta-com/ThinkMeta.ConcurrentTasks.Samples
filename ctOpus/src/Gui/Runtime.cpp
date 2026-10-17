// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include <Windows.h>

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <avrt.h>

#include <cstdint>

#include "Runtime.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::Gui {

    namespace {

        // the MMCSS handle of a scheduler thread, from its Starting to its Exiting callback
        thread_local HANDLE schedulerThreadMmcss = nullptr;

        void ProAudioThreadCallback(ITaskScheduler*, DWORD, HANDLE, void*, ThreadCallbackReason reason) noexcept
        {
            if (reason == ThreadCallbackReason::Starting) {
                schedulerThreadMmcss = EnterProAudio();
            } else {
                LeaveProAudio(schedulerThreadMmcss);
                schedulerThreadMmcss = nullptr;
            }
        }

    } // namespace

    HANDLE EnterProAudio() noexcept
    {
        unsigned long taskIndex = 0; // DWORD of AvSetMmThreadCharacteristics
        HANDLE        mmcss     = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        if (mmcss == nullptr) {
            taskIndex = 0;
            mmcss     = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
        }
        return mmcss;
    }

    void LeaveProAudio(HANDLE mmcss) noexcept
    {
        if (mmcss != nullptr)
            AvRevertMmThreadCharacteristics(mmcss);
    }

    SchedulerHandle CreateAudioScheduler(std::uint32_t threads, bool mmcss) noexcept
    {
        SchedulerInfo info;
        info.numberOfThreads = threads;
        if (mmcss)
            info.threadCallback = ProAudioThreadCallback;
        return SchedulerHandle(CreateTaskScheduler(SchedulerKind::Default, &info));
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::Gui
