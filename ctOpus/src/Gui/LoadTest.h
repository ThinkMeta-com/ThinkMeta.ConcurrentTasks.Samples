// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstddef>
#include <cstdint>

#include "FrameCodec.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus::Gui {

    class SignalBuffer;

    /// <summary>The encoder and the way the streams are run (each combination, so the two effects can be told apart).</summary>
    enum class LoadEngine { CtOpusTasks, CtOpusThreads, LibopusThreads, LibopusTasks };

    inline bool UsesTasks(LoadEngine engine) noexcept
    {
        return engine == LoadEngine::CtOpusTasks || engine == LoadEngine::LibopusTasks;
    }

    inline EncoderKind EncoderOf(LoadEngine engine) noexcept
    {
        return engine == LoadEngine::CtOpusTasks || engine == LoadEngine::CtOpusThreads ? EncoderKind::CtOpus : EncoderKind::Libopus;
    }

    struct LoadConfig {
        LoadEngine          engine        = LoadEngine::CtOpusTasks;
        std::int32_t        streams       = 64;
        std::uint32_t       workerThreads = 0; // tasks: scheduler threads, 0 = one per logical processor
        std::int32_t        frameSize     = 240;
        Application         application   = Application::RestrictedLowDelay;
        std::int32_t        bitrate       = 64000;
        std::int32_t        complexity    = 10;
        bool                mmcss         = true;
        const SignalBuffer* signal        = nullptr; // must outlive the test
    };

    struct LoadSnapshot {
        static constexpr std::int32_t Bins = 64; // lateness histogram: Bins - 1 bins over two frame lengths, then the rest

        bool    running        = false;
        wchar_t status[256]    = {};
        wchar_t engineName[48] = {};

        std::int32_t streams         = 0;
        std::int32_t osThreads       = 0; // threads that encode (scheduler threads or one per stream)
        double       frameMs         = 0;
        double       seconds         = 0;
        std::int64_t ticks           = 0; // frames due per stream so far
        std::int64_t frames          = 0; // frames encoded, all streams
        std::int64_t misses          = 0; // finished more than a frame length after the frame was due
        std::int64_t dropped         = 0; // skipped because a stream fell too far behind
        double       encodeUs        = 0; // average encode time of a frame
        double       latencyMs       = 0; // average time from the frame being due to its packet
        double       p50Ms           = 0;
        double       p99Ms           = 0;
        double       maxMs           = 0; // since the start
        double       windowMaxMs     = 0; // since the previous Poll
        std::int64_t windowMisses    = 0; // since the previous Poll
        double       pacerLateMs     = 0; // the worst lateness of the clock thread itself
        double       cpuPercent      = 0; // of all logical processors, since the start
        std::int64_t histogram[Bins] = {};
        double       binMs           = 0;
    };

    /// <summary>
    /// Plan section 3.3, item 4: many live streams at once. A clock thread makes frame k of every
    /// stream due at start + (k + 1) * frame length and wakes the streams; each stream encodes its
    /// frames and counts a deadline miss when a packet is ready more than one frame length after
    /// its frame was due. The streams run as ConcurrentTasks tasks on one scheduler, or as one OS
    /// thread per stream (the classic libopus setup).
    /// </summary>
    class LoadTest
    {
    public:
        LoadTest() noexcept;
        LoadTest(const LoadTest&)            = delete;
        LoadTest& operator=(const LoadTest&) = delete;
        ~LoadTest();

        bool Start(const LoadConfig& config, wchar_t* error, std::size_t errorCapacity) noexcept;
        void Stop() noexcept;
        bool Running() const noexcept { return impl_ != nullptr; }

        /// <summary>The totals; the window values are since the previous call.</summary>
        void Poll(LoadSnapshot& snapshot) noexcept;

    private:
        struct Impl;
        Impl* impl_ = nullptr;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus::Gui
