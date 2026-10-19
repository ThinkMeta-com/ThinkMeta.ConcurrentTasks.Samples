// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "pch.h"

#include <cstdint>

#include "TaskRuntime.h"
#include "TestEnvironment.h"

import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks;
namespace Shared = ThinkMeta::ConcurrentTasks::Samples::Shared;

// What a handoff between two tasks costs with the library events. The result
// decides whether the pipeline hands over single frames or batches (one frame is about 205 us of
// sequential work). Runs only with the environment variable CTLAME_BENCH_EVENTS set, like the
// I/O benchmark; the numbers go to the test log.

namespace LameTests {

    namespace {

        std::int64_t Now() noexcept
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            return now.QuadPart;
        }

        double Nanoseconds(std::int64_t ticks, std::int64_t operations) noexcept
        {
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);
            return 1e9 * static_cast<double>(ticks) / static_cast<double>(frequency.QuadPart) / static_cast<double>(operations);
        }

        void Log(const wchar_t* format, double value)
        {
            wchar_t text[200];
            swprintf_s(text, format, value);
            Logger::WriteMessage(text);
        }

        constexpr std::int32_t RoundTrips = 200000;

        // Two tasks hand a token back and forth: ping wakes the second, pong wakes the first.
        // Returns nanoseconds per round trip (two handoffs).
        double PingPong(ITaskScheduler* first, ITaskScheduler* second)
        {
            Synchronization::AutoResetEvent_s ping;
            Synchronization::AutoResetEvent_s pong;
            std::int64_t                      ticks = 0;

            auto a = Tasks::Task::Create([&] {
                const std::int64_t start = Now();
                for (std::int32_t i = 0; i < RoundTrips; ++i) {
                    ping.Set();
                    pong.Wait();
                }
                ticks = Now() - start;
            });
            auto b = Tasks::Task::Create([&] {
                for (std::int32_t i = 0; i < RoundTrips; ++i) {
                    ping.Wait();
                    pong.Set();
                }
            });

            Synchronization::TaskGroup group;
            group.AddTask(&a);
            group.AddTask(&b);
            second->AddTask(&b, TaskPriority::Normal);
            first->AddTask(&a, TaskPriority::Normal);
            group.Wait();
            return Nanoseconds(ticks, RoundTrips);
        }

        void RunHandoffCosts()
        {
            wchar_t value[8];
            if (!Shared::TestEnvironment::Get(L"CTLAME_BENCH_EVENTS", value)) {
                wchar_t text[200];
                Logger::WriteMessage(Shared::TestEnvironment::SkipText(text, L"CTLAME_BENCH_EVENTS", L"any value"));
                return;
            }
            const Shared::TaskRuntime runtime(0);

            // the fast path: the event is already set when the task waits
            {
                Synchronization::AutoResetEvent_s event;
                constexpr std::int32_t            operations = 10000000;
                const std::int64_t                start      = Now();
                for (std::int32_t i = 0; i < operations; ++i) {
                    event.Set();
                    event.Wait();
                }
                Log(L"set + wait, already set:            %8.1f ns", Nanoseconds(Now() - start, operations));
            }

            // the same scheduler, one thread: every handoff is a fiber switch on that thread
            {
                Shared::RunningScheduler scheduler(SchedulerKind::Default, 1);
                Log(L"round trip, 1 scheduler, 1 thread:  %8.1f ns", PingPong(scheduler.Get(), scheduler.Get()));
                scheduler.Stop();
            }

            // the same scheduler, two threads: a handoff may wake the other thread
            {
                Shared::RunningScheduler scheduler(SchedulerKind::Default, 2);
                Log(L"round trip, 1 scheduler, 2 threads: %8.1f ns", PingPong(scheduler.Get(), scheduler.Get()));
                scheduler.Stop();
            }

            // two schedulers: every handoff crosses threads
            {
                Shared::RunningScheduler first(SchedulerKind::Default, 1);
                Shared::RunningScheduler second(SchedulerKind::Default, 1);
                Log(L"round trip, 2 schedulers:           %8.1f ns", PingPong(first.Get(), second.Get()));
                second.Stop();
                first.Stop();
            }
        }

    } // namespace

    // clang-format off
    TEST_CLASS(EventCostBenchmark)
    {
    public:
        TEST_METHOD(HandoffCosts) { RunHandoffCosts(); }
    };
// clang-format on

} // namespace LameTests
