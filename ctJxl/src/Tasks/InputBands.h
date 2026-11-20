// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

// Included by the frontend copy of cjxl (C++17): no ConcurrentTasks or libjxl-internal types here, only the public
// runner type of libjxl (plan section 4.6, project boundary).
#include <jxl/parallel_runner.h>

#include <cstddef>

namespace ThinkMeta::ConcurrentTasks::Samples::Jxl {

    /// <summary>
    /// The rows of an input image while a reader task decodes it and the encoder already reads it (plan D1.6, the
    /// InputImage bands of section 4.3).
    /// <para>The reader runs as a task on the runner's scheduler and publishes how many rows are complete, in order.
    /// The image is split into bands of equal height; a band opens once all its rows are published, and a reader of
    /// rows waits for the band that holds its last row. Any number of readers may wait.</para>
    /// <para>Abort opens every band; a wait then returns false. The reader aborts when it fails, so that no encoder
    /// task waits for rows that never come.</para>
    /// <para>Create, start and join the reader on the same task (the frontend's converted thread). Destroy the bands
    /// only after the runner's scheduler has stopped: any number of tasks wait on a band, and a waiter's suspend
    /// callback may still look at the band's event after the wait has returned (ManualResetEvent remarks). So join the
    /// reader (JoinReader) while the scheduler runs, then stop it, then destroy the bands.</para>
    /// <para>Preliminary.</para>
    /// </summary>
    class InputBands
    {
    public:
        /// <summary>The bands for rows rows on runner, or null when runner is not the ConcurrentTasks runner or has no
        /// workers (the frontend then decodes the whole image before encoding).</summary>
        [[nodiscard]] static InputBands* Create(JxlParallelRunner runner, void* runnerOpaque, std::size_t rows,
                                                std::size_t bandRows) noexcept;

        /// <summary>Waits for the reader task, if one was started, and destroys bands (null is allowed).</summary>
        static void Destroy(InputBands* bands) noexcept;

        /// <summary>Starts read(context) as the reader task; its result tells whether reading succeeded.</summary>
        [[nodiscard]] virtual bool StartReader(bool (*read)(void* context), void* context) noexcept = 0;

        /// <summary>Rows 0 .. rows - 1 are complete (called by the reader, with growing values).</summary>
        virtual void Publish(std::size_t rows) noexcept = 0;

        /// <summary>Waits until rows 0 .. rows - 1 are complete; false after an abort.</summary>
        [[nodiscard]] virtual bool WaitRows(std::size_t rows) noexcept = 0;

        /// <summary>Opens every band; later waits return false.</summary>
        virtual void Abort() noexcept = 0;

        /// <summary>Waits for the reader task; true if it read the whole image.</summary>
        [[nodiscard]] virtual bool JoinReader() noexcept = 0;

    protected:
        InputBands() noexcept = default;
        ~InputBands() = default;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jxl
