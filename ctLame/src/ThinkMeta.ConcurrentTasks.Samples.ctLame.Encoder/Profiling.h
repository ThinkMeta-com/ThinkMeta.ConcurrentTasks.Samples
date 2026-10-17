// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    // Opt-in profile of ctLame: time per encoder stage and busy/idle time per task.
    // It is switched on with the environment variable CTLAME_PROFILE (any value but "0"); when
    // it is off, the encoder and the tasks get no profile objects and pay one null check.

    /// <summary>The encoder stages that are timed.</summary>
    enum class ProfileStage : std::int32_t { Input, Psy, PsyResolve, Mdct, Stereo, Search, Quantize, Bitstream, Count };

    /// <summary>Name of a stage for the report.</summary>
    const wchar_t* ProfileStageName(ProfileStage stage) noexcept;

    /// <summary>A timestamp in ticks (QueryPerformanceCounter).</summary>
    std::int64_t ProfileNow() noexcept;

    /// <summary>Ticks per second.</summary>
    std::int64_t ProfileTicksPerSecond() noexcept;

    /// <summary>Whether CTLAME_PROFILE asks for a profile.</summary>
    bool ProfileRequested() noexcept;

    /// <summary>Accumulated time per stage; stages may run on several threads at once.</summary>
    struct StageProfile {
        std::array<std::atomic<std::int64_t>, static_cast<std::int32_t>(ProfileStage::Count)> ticks  = {};
        std::atomic<std::int64_t>                                                             frames = 0;

        std::atomic<std::int64_t>* Counter(ProfileStage stage) noexcept { return &ticks[static_cast<std::int32_t>(stage)]; }
    };

    /// <summary>
    /// Life time and waiting time of a task, or summed over several tasks of one kind (the
    /// search workers): busy = life - idle.
    /// </summary>
    struct TaskProfile {
        std::atomic<std::int64_t> life  = 0;
        std::atomic<std::int64_t> idle  = 0;
        std::atomic<std::int32_t> tasks = 0;
    };

    /// <summary>The start of a task's Run, for ProfileTaskEnd; 0 without a profile.</summary>
    inline std::int64_t ProfileTaskBegin(const TaskProfile* profile) noexcept
    {
        return profile != nullptr ? ProfileNow() : 0;
    }

    /// <summary>The end of a task's Run: adds its life time.</summary>
    inline void ProfileTaskEnd(TaskProfile* profile, std::int64_t begin) noexcept
    {
        if (profile != nullptr) {
            profile->life.fetch_add(ProfileNow() - begin, std::memory_order_relaxed);
            profile->tasks.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /// <summary>Adds the time of a scope to a counter; does nothing for a null counter.</summary>
    class ProfileScope
    {
    public:
        explicit ProfileScope(std::atomic<std::int64_t>* counter) noexcept : counter_(counter), start_(counter != nullptr ? ProfileNow() : 0) {}

        ~ProfileScope()
        {
            if (counter_ != nullptr)
                counter_->fetch_add(ProfileNow() - start_, std::memory_order_relaxed);
        }

        ProfileScope(const ProfileScope&)            = delete;
        ProfileScope& operator=(const ProfileScope&) = delete;

    private:
        std::atomic<std::int64_t>* counter_;
        std::int64_t               start_;
    };

    /// <summary>The idle counter of a task profile, or null.</summary>
    inline std::atomic<std::int64_t>* IdleCounter(TaskProfile* profile) noexcept
    {
        return profile != nullptr ? &profile->idle : nullptr;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
