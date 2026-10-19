// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <cstdint>

#include <Windows.h>

#include <ThinkMeta.ConcurrentTasks/Core.h>

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>
    /// Priority of the I/O tasks by the fill level of their pipe, with hysteresis between the
    /// two watermarks so the priority does not flip on every block:
    ///   reader (fills its pipe):   High below the low watermark, Normal from the high one on;
    ///   writer (drains its pipe):  High above the high watermark or once its producer has
    ///                              finished, Normal from the low one down.
    /// </summary>
    struct FillLevelPolicy {
        std::uint32_t lowPercent  = 25;
        std::uint32_t highPercent = 50;

        TaskPriority ForProducer(std::uint32_t filled, std::uint32_t capacity, TaskPriority current) const noexcept
        {
            if (filled * 100 < lowPercent * capacity)
                return TaskPriority::High;
            if (filled * 100 >= highPercent * capacity)
                return TaskPriority::Normal;
            return current;
        }

        TaskPriority ForConsumer(std::uint32_t filled, std::uint32_t capacity, bool producerFinished, TaskPriority current) const noexcept
        {
            if (producerFinished || filled * 100 > highPercent * capacity)
                return TaskPriority::High;
            if (filled * 100 <= lowPercent * capacity)
                return TaskPriority::Normal;
            return current;
        }
    };

    /// <summary>What an I/O task did, for measurements of the I/O pipeline.</summary>
    struct IoStats {
        std::uint64_t bytes           = 0;
        std::uint32_t operations      = 0;
        std::uint32_t priorityChanges = 0;
        std::uint32_t highOperations  = 0; // operations completed while the task ran with High priority
        std::uint32_t maxFilled       = 0;
        std::uint32_t minFilled       = 0xFFFFFFFFu;
    };

    /// <summary>
    /// Sets the priority of the calling task if it changed. When it drops to Normal the task
    /// yields, so that a waiting High task gets the I/O thread first.
    /// </summary>
    inline void ApplyPriority(TaskPriority& current, TaskPriority wanted, IoStats& stats) noexcept
    {
        if (wanted == current)
            return;
        current = wanted;
        SetTaskPriority(wanted);
        ++stats.priorityChanges;
        if (wanted != TaskPriority::High)
            YieldTask(YieldFlags::HigherPriority);
    }

    inline void CountOperation(IoStats& stats, TaskPriority current, std::uint32_t filled, std::uint64_t bytes) noexcept
    {
        stats.bytes += bytes;
        ++stats.operations;
        if (current == TaskPriority::High)
            ++stats.highOperations;
        if (filled > stats.maxFilled)
            stats.maxFilled = filled;
        if (filled < stats.minFilled)
            stats.minFilled = filled;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
