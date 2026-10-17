// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>

#include "Common.h"
#include "FrameCodec.h"
#include "LoadTest.h"
#include "Runtime.h"
#include "SchedulerHandle.h"
#include "SignalSource.h"

import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::Gui {

    namespace {

        constexpr std::int64_t MaxBacklog = 50; // frames a stream may fall behind before it skips to the newest

        std::int64_t FileTime(const FILETIME& t) noexcept
        {
            return static_cast<std::int64_t>((static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime);
        }

        std::int64_t ProcessCpu() noexcept
        {
            FILETIME created;
            FILETIME exited;
            FILETIME kernel;
            FILETIME user;
            if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
                return 0;
            return FileTime(kernel) + FileTime(user);
        }

        void AtomicMax(std::atomic<std::int64_t>& target, std::int64_t value) noexcept
        {
            std::int64_t current = target.load(std::memory_order_relaxed);
            while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
            }
        }

    } // namespace

    struct LoadTest::Impl {
        struct Stream;

        class StreamTask final : public Tasks::Task
        {
        public:
            StreamTask(Impl& impl, Stream& stream) noexcept : impl_(impl), stream_(stream) {}
            void Run() noexcept override;

        private:
            Impl&   impl_;
            Stream& stream_;
        };

        // One stream; written by its own worker only (the counters are read by Poll).
        struct alignas(64) Stream {
            Impl*                         impl = nullptr;
            std::unique_ptr<FrameEncoder> encoder;
            std::int64_t                  offset = 0; // where in the signal this stream plays
            std::int64_t                  next   = 0; // the next frame to encode

            std::atomic<std::int64_t> frames{0};
            std::atomic<std::int64_t> misses{0};
            std::atomic<std::int64_t> dropped{0};
            std::atomic<std::int64_t> encodeSum{0};  // 100 ns
            std::atomic<std::int64_t> latencySum{0}; // 100 ns
            std::atomic<std::int64_t> latencyMax{0};
            std::atomic<std::int64_t> windowMax{0};
            std::atomic<std::int64_t> histogram[LoadSnapshot::Bins]; // value-initialised (zero)

            Synchronization::AutoResetEvent_s wake;  // tasks
            Handle                            event; // threads
            Handle                            thread;
            std::unique_ptr<StreamTask>       task;

            float        pcm[2 * 960]           = {};
            std::uint8_t packet[MaxPacketBytes] = {};
        };

        LoadConfig   config;
        std::int64_t frameLength = 0; // 100 ns
        std::int64_t binLength   = 0; // 100 ns
        std::int64_t start       = 0;

        Stream*      streams = nullptr;
        std::int32_t count   = 0;

        alignas(64) std::atomic<std::int64_t> published{0}; // frames due per stream
        alignas(64) std::atomic<bool> stop{false};
        std::atomic<std::int64_t> pacerLate{0};

        SchedulerHandle scheduler;
        TaskExitCounter exits;
        bool            tasksAdded = false;
        Handle          pacer;
        std::uint32_t   schedulerThreads = 0;

        std::int64_t startWall  = 0; // for the CPU load since the start
        std::int64_t startCpu   = 0;
        std::int64_t lastMisses = 0;

        ~Impl() { delete[] streams; }

        void Process(Stream& s) noexcept
        {
            const std::int64_t due = published.load(std::memory_order_acquire);
            if (due - s.next > MaxBacklog) {
                s.dropped.store(s.dropped.load(std::memory_order_relaxed) + due - 1 - s.next, std::memory_order_relaxed);
                s.next = due - 1;
            }
            const std::int32_t frameSize = config.frameSize;
            for (; s.next < due && !stop.load(std::memory_order_relaxed); ++s.next) {
                config.signal->Read(s.offset + s.next * frameSize, s.pcm, frameSize);
                const std::int64_t t0 = Now100ns();
                s.encoder->Encode(s.pcm, frameSize, s.packet, MaxPacketBytes);
                const std::int64_t t1       = Now100ns();
                const std::int64_t lateness = t1 - (start + (s.next + 1) * frameLength);
                const std::int64_t l        = lateness > 0 ? lateness : 0;
                s.frames.store(s.frames.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
                s.encodeSum.store(s.encodeSum.load(std::memory_order_relaxed) + t1 - t0, std::memory_order_relaxed);
                s.latencySum.store(s.latencySum.load(std::memory_order_relaxed) + l, std::memory_order_relaxed);
                if (l > frameLength)
                    s.misses.store(s.misses.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
                if (l > s.latencyMax.load(std::memory_order_relaxed))
                    s.latencyMax.store(l, std::memory_order_relaxed);
                AtomicMax(s.windowMax, l);
                std::int64_t bin = l / binLength;
                if (bin >= LoadSnapshot::Bins)
                    bin = LoadSnapshot::Bins - 1;
                auto& slot = s.histogram[bin];
                slot.store(slot.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            }
        }

        static unsigned long __stdcall StreamThreadProc(void* param) noexcept
        {
            auto&  s     = *static_cast<Stream*>(param);
            Impl&  impl  = *s.impl;
            HANDLE mmcss = impl.config.mmcss ? EnterProAudio() : nullptr;
            for (;;) {
                WaitForSingleObject(s.event.Get(), INFINITE);
                if (impl.stop.load(std::memory_order_acquire))
                    break;
                impl.Process(s);
            }
            LeaveProAudio(mmcss);
            return 0;
        }

        static unsigned long __stdcall PacerThreadProc(void* param) noexcept
        {
            static_cast<Impl*>(param)->PacerLoop();
            return 0;
        }

        void PacerLoop() noexcept
        {
            HANDLE     mmcss = config.mmcss ? EnterProAudio() : nullptr;
            Handle     timer(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS));
            const bool tasks = UsesTasks(config.engine);
            for (std::int64_t k = 1; !stop.load(std::memory_order_acquire); ++k) {
                const std::int64_t due  = start + k * frameLength;
                const std::int64_t wait = due - Now100ns();
                if (wait > 0 && timer) {
                    LARGE_INTEGER relative;
                    relative.QuadPart = -wait;
                    SetWaitableTimer(timer.Get(), &relative, 0, nullptr, nullptr, FALSE);
                    WaitForSingleObject(timer.Get(), 100);
                } else if (wait > 0) {
                    Sleep(static_cast<unsigned long>(wait / 10'000));
                }
                AtomicMax(pacerLate, Now100ns() - due);
                published.store(k, std::memory_order_release);
                for (std::int32_t i = 0; i < count; ++i) {
                    if (tasks)
                        streams[i].wake.Set();
                    else
                        SetEvent(streams[i].event.Get());
                }
            }
            LeaveProAudio(mmcss);
        }

        bool Start(wchar_t* error, std::size_t errorCapacity) noexcept
        {
            if (config.signal == nullptr || config.streams < 1) {
                CopyText(error, errorCapacity, L"No signal or no streams.");
                return false;
            }
            frameLength = FramesTo100ns(config.frameSize);
            binLength   = 2 * frameLength / (LoadSnapshot::Bins - 1);
            count       = config.streams;
            streams     = new (std::nothrow) Stream[static_cast<std::size_t>(count)];
            if (streams == nullptr) {
                CopyText(error, errorCapacity, L"Out of memory.");
                return false;
            }
            EncoderSettings settings;
            settings.channels       = Channels;
            settings.application    = config.application;
            settings.bitrate        = config.bitrate;
            settings.complexity     = config.complexity;
            const EncoderKind kind  = EncoderOf(config.engine);
            const bool        tasks = UsesTasks(config.engine);
            for (std::int32_t i = 0; i < count; ++i) {
                Stream& s = streams[i];
                s.impl    = this;
                s.offset  = static_cast<std::int64_t>(i) * 33'613;
                s.encoder.reset(CreateEncoder(kind, settings));
                if (!s.encoder) {
                    swprintf_s(error, errorCapacity, L"Encoder %d could not be created.", i);
                    return false;
                }
                if (tasks) {
                    s.task.reset(new (std::nothrow) StreamTask(*this, s));
                    if (!s.task) {
                        CopyText(error, errorCapacity, L"Out of memory.");
                        return false;
                    }
                } else {
                    s.event.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
                    if (!s.event) {
                        CopyText(error, errorCapacity, L"CreateEvent failed.");
                        return false;
                    }
                }
            }

            start     = Now100ns() + 200'000; // 20 ms to get everything going
            startWall = Now100ns();
            startCpu  = ProcessCpu();
            if (tasks) {
                schedulerThreads = config.workerThreads > 0 ? config.workerThreads : GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
                scheduler        = CreateAudioScheduler(schedulerThreads, config.mmcss);
                if (!scheduler) {
                    CopyText(error, errorCapacity, L"The task scheduler could not be created.");
                    return false;
                }
                scheduler->Start();
                for (std::int32_t i = 0; i < count; ++i) {
                    exits.Add(streams[i].task.get());
                    scheduler->AddTask(streams[i].task.get(), TaskPriority::High);
                }
                tasksAdded = true;
            } else {
                for (std::int32_t i = 0; i < count; ++i) {
                    streams[i].thread.Reset(CreateThread(nullptr, 0, StreamThreadProc, &streams[i], 0, nullptr));
                    if (!streams[i].thread) {
                        swprintf_s(error, errorCapacity, L"Thread %d could not be created.", i);
                        return false;
                    }
                }
            }
            pacer.Reset(CreateThread(nullptr, 0, PacerThreadProc, this, 0, nullptr));
            if (!pacer) {
                CopyText(error, errorCapacity, L"The clock thread could not be created.");
                return false;
            }
            return true;
        }

        void Stop() noexcept
        {
            stop.store(true, std::memory_order_release);
            if (pacer)
                WaitForSingleObject(pacer.Get(), INFINITE);
            pacer.Reset();
            for (std::int32_t i = 0; streams != nullptr && i < count; ++i) {
                if (streams[i].thread) {
                    SetEvent(streams[i].event.Get());
                    WaitForSingleObject(streams[i].thread.Get(), INFINITE);
                    streams[i].thread.Reset();
                }
            }
            if (tasksAdded) {
                for (std::int32_t i = 0; i < count; ++i)
                    streams[i].wake.Set();
                exits.Wait(INFINITE);
                tasksAdded = false;
            }
            if (scheduler) {
                scheduler->Stop();
                scheduler.Reset();
            }
        }

        void Poll(LoadSnapshot& out) noexcept
        {
            out                     = {};
            out.running             = true;
            out.streams             = count;
            out.osThreads           = UsesTasks(config.engine) ? static_cast<std::int32_t>(schedulerThreads) : count;
            out.frameMs             = ToMs(frameLength);
            out.binMs               = ToMs(binLength);
            const auto now          = Now100ns();
            out.seconds             = now > start ? ToMs(now - start) / 1000.0 : 0;
            out.ticks               = published.load(std::memory_order_acquire);
            out.pacerLateMs         = ToMs(pacerLate.load(std::memory_order_relaxed));
            std::int64_t encodeSum  = 0;
            std::int64_t latencySum = 0;
            std::int64_t latencyMax = 0;
            std::int64_t windowMax  = 0;
            for (std::int32_t i = 0; i < count; ++i) {
                Stream& s = streams[i];
                out.frames += s.frames.load(std::memory_order_relaxed);
                out.misses += s.misses.load(std::memory_order_relaxed);
                out.dropped += s.dropped.load(std::memory_order_relaxed);
                encodeSum += s.encodeSum.load(std::memory_order_relaxed);
                latencySum += s.latencySum.load(std::memory_order_relaxed);
                const std::int64_t m = s.latencyMax.load(std::memory_order_relaxed);
                latencyMax           = m > latencyMax ? m : latencyMax;
                const std::int64_t w = s.windowMax.exchange(0, std::memory_order_relaxed);
                windowMax            = w > windowMax ? w : windowMax;
                for (std::int32_t b = 0; b < LoadSnapshot::Bins; ++b)
                    out.histogram[b] += s.histogram[b].load(std::memory_order_relaxed);
            }
            if (out.frames > 0) {
                out.encodeUs            = static_cast<double>(encodeSum) / 10.0 / static_cast<double>(out.frames);
                out.latencyMs           = ToMs(latencySum) / static_cast<double>(out.frames);
                std::int64_t cumulative = 0;
                bool         have50     = false;
                for (std::int32_t b = 0; b < LoadSnapshot::Bins; ++b) {
                    cumulative += out.histogram[b];
                    if (!have50 && cumulative * 2 >= out.frames) {
                        out.p50Ms = (b + 1) * out.binMs;
                        have50    = true;
                    }
                    if (cumulative * 100 >= out.frames * 99) {
                        out.p99Ms = (b + 1) * out.binMs;
                        break;
                    }
                }
            }
            out.maxMs        = ToMs(latencyMax);
            out.windowMaxMs  = ToMs(windowMax);
            out.windowMisses = out.misses - lastMisses;
            lastMisses       = out.misses;

            const std::int64_t cpu  = ProcessCpu();
            const std::int64_t wall = now - startWall;
            const auto         cpus = static_cast<double>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
            if (wall > 0)
                out.cpuPercent = 100.0 * static_cast<double>(cpu - startCpu) / static_cast<double>(wall) / cpus;

            const wchar_t* names[] = {L"ctOpus, tasks", L"ctOpus, thread per stream", L"libopus, thread per stream", L"libopus, tasks"};
            CopyText(out.engineName, sizeof(out.engineName) / sizeof(out.engineName[0]), names[static_cast<std::int32_t>(config.engine)]);
            CopyText(out.status, sizeof(out.status) / sizeof(out.status[0]), L"Running.");
        }
    };

    void LoadTest::Impl::StreamTask::Run() noexcept
    {
        for (;;) {
            stream_.wake.Wait();
            if (impl_.stop.load(std::memory_order_acquire))
                break;
            impl_.Process(stream_);
        }
    }

    LoadTest::LoadTest() noexcept = default;

    LoadTest::~LoadTest()
    {
        Stop();
    }

    bool LoadTest::Start(const LoadConfig& config, wchar_t* error, std::size_t errorCapacity) noexcept
    {
        Stop();
        impl_ = new (std::nothrow) Impl;
        if (impl_ == nullptr) {
            CopyText(error, errorCapacity, L"Out of memory.");
            return false;
        }
        impl_->config = config;
        if (!impl_->Start(error, errorCapacity)) {
            Stop();
            return false;
        }
        return true;
    }

    void LoadTest::Stop() noexcept
    {
        if (impl_ == nullptr)
            return;
        impl_->Stop();
        delete impl_;
        impl_ = nullptr;
    }

    void LoadTest::Poll(LoadSnapshot& snapshot) noexcept
    {
        if (impl_ == nullptr) {
            snapshot.running = false;
            return;
        }
        impl_->Poll(snapshot);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::Gui
