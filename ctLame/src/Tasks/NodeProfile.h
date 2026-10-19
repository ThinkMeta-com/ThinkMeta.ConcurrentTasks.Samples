// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <atomic>
#include <cstdint>

#include "Profiling.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    /// <summary>
    /// CTLAME_PROFILE for a node: counts the lifetime of its Run into the profile (one object at the top of Run). Without a
    /// profile it does nothing but one null check.
    /// </summary>
    class ProfiledRun
    {
    public:
        explicit ProfiledRun(TaskProfile* profile) noexcept : profile_(profile), begin_(ProfileTaskBegin(profile)) {}
        ~ProfiledRun() { ProfileTaskEnd(profile_, begin_); }

        ProfiledRun(const ProfiledRun&)            = delete;
        ProfiledRun& operator=(const ProfiledRun&) = delete;

    private:
        TaskProfile* profile_;
        std::int64_t begin_;
    };

    /// <summary>Calls wait() (an Acquire on an edge) and counts its time as the node's idle time.</summary>
    template <typename Wait>
    decltype(auto) WhileIdle(TaskProfile* profile, Wait&& wait) noexcept
    {
        ProfileScope idle(IdleCounter(profile));
        return wait();
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
