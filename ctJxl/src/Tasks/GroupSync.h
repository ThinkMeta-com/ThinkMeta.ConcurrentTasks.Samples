// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

// Included by the libjxl driver copy (ctJxl.Encoder, C++17): no ConcurrentTasks or libjxl-internal types here, only the
// public runner type of libjxl (plan section 4.6, project boundary).
#include <jxl/parallel_runner.h>

#include <cstddef>

namespace ThinkMeta::ConcurrentTasks::Samples::Jxl {

    /// <summary>
    /// Where the DC groups of one streamed frame meet while they are encoded as tasks at the same time (plan D1.5).
    /// <para>Turns: a sequence lets the groups through one at a time in their order; group g waits for its turn after
    /// group g - 1 has ended its own. Each turn has one waiter (group g), its predecessor ends it. The gate: group 0
    /// opens it once the state every group needs is final; every other group waits for it once.</para>
    /// <para>Abort ends every wait at once, also future ones; a wait then returns false. A group that fails calls it,
    /// so that no other group waits for a turn or a gate that never comes.</para>
    /// <para>The object lives until every group task has exited (the driver creates it before the tasks and destroys it
    /// after their join).</para>
    /// <para>Preliminary.</para>
    /// </summary>
    class GroupSync
    {
    public:
        /// <summary>
        /// The synchronization for groups encoded on runner, or null when runner is not the ConcurrentTasks runner or
        /// has fewer than two workers (the driver then encodes the groups one after another).
        /// </summary>
        [[nodiscard]] static GroupSync* Create(JxlParallelRunner runner, void* runnerOpaque, std::size_t groups,
                                               std::size_t sequences) noexcept;

        /// <summary>Destroys sync (null is allowed).</summary>
        static void Destroy(GroupSync* sync) noexcept;

        /// <summary>The worker tasks the runner forks per region.</summary>
        [[nodiscard]] virtual std::size_t Workers() const noexcept = 0;

        /// <summary>Waits until group may enter sequence; false after an abort.</summary>
        [[nodiscard]] virtual bool WaitTurn(std::size_t sequence, std::size_t group) noexcept = 0;

        /// <summary>Lets the next group into sequence.</summary>
        virtual void EndTurn(std::size_t sequence, std::size_t group) noexcept = 0;

        /// <summary>Waits until group 0 has opened the gate (group 1 or later, once each); false after an abort.</summary>
        [[nodiscard]] virtual bool WaitGate(std::size_t group) noexcept = 0;

        /// <summary>Opens the gate for all groups.</summary>
        virtual void OpenGate() noexcept = 0;

        /// <summary>Ends every wait; later waits return false at once.</summary>
        virtual void Abort() noexcept = 0;

    protected:
        GroupSync() noexcept = default;
        ~GroupSync() = default;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jxl
