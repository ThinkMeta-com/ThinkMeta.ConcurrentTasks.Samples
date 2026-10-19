// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "pch.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <new>
#include <optional>

#include <immintrin.h>

// headers with imports last
#include "SchedulerHandle.h"

import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg;
namespace Ct = ThinkMeta::ConcurrentTasks;

// The cost of one stage join of the encode graph (StageWorkers.cpp: fork the workers, wait for the group) with
// TaskGroup and with TaskGroup_s, measured alone: the converted calling thread forks the workers and waits, as in
// RunStageJobs. Not a gate (the name contains Benchmark); prints microseconds per join.
namespace ctJpegTests {

    namespace {

        struct Jobs {
            std::int32_t              count  = 0;
            std::int64_t              spinNs = 0;
            std::atomic<std::int32_t> next{0};
        };

        double TicksPerNs()
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return static_cast<double>(f.QuadPart) / 1e9;
        }

        class Worker : public Ct::Tasks::Task
        {
        public:
            Worker(Jobs& jobs, double ticksPerNs) noexcept : jobs_(jobs), ticksPerNs_(ticksPerNs) {}

            void Run() noexcept override
            {
                while (jobs_.next.fetch_add(1, std::memory_order_relaxed) < jobs_.count) {
                    if (jobs_.spinNs == 0)
                        continue;
                    LARGE_INTEGER t0, t;
                    QueryPerformanceCounter(&t0);
                    const auto until = t0.QuadPart + static_cast<long long>(jobs_.spinNs * ticksPerNs_);
                    do {
                        _mm_pause();
                        QueryPerformanceCounter(&t);
                    } while (t.QuadPart < until);
                }
            }

        private:
            Jobs&  jobs_;
            double ticksPerNs_;
        };

        // Microseconds per join, the median of the rounds.
        template <typename Group>
        double JoinMicros(Ct::ITaskScheduler& scheduler, std::uint32_t workers, std::int32_t jobsPerJoin, std::int64_t spinNs, std::int32_t joins)
        {
            const double ticksPerNs = TicksPerNs();
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            for (std::int32_t j = 0; j < joins; ++j) {
                Jobs jobs;
                jobs.count                    = jobsPerJoin;
                jobs.spinNs                   = spinNs;
                const std::uint32_t taskCount = std::min<std::uint32_t>(workers, static_cast<std::uint32_t>(jobsPerJoin));
                const auto tasks = std::unique_ptr<std::optional<Worker>[]>(new (std::nothrow) std::optional<Worker>[taskCount]);
                Group      group;
                for (std::uint32_t i = 0; i < taskCount; ++i) {
                    tasks[i].emplace(jobs, ticksPerNs);
                    group.AddTask(&*tasks[i]);
                }
                for (std::uint32_t i = 0; i < taskCount; ++i)
                    scheduler.AddTask(&*tasks[i], Ct::TaskPriority::Normal);
                group.Wait();
            }
            QueryPerformanceCounter(&t1);
            return static_cast<double>(t1.QuadPart - t0.QuadPart) / ticksPerNs / 1000.0 / joins;
        }

        double Median(double* v, std::int32_t n)
        {
            std::sort(v, v + n);
            return v[n / 2];
        }

    } // namespace

    TEST_CLASS(StageJoinBenchmark)
    {
    public:
        TEST_METHOD(TaskGroupVersusTaskGroup_s)
        {
            constexpr std::uint32_t Workers = 16;
            constexpr std::int32_t  Rounds  = 9;
            Ct::ConvertThreadToTask();
            {
                SchedulerHandle compute = SchedulerHandle::Create(Ct::SchedulerKind::Default, Workers);
                Assert::IsTrue(static_cast<bool>(compute));
                compute->Start();
                struct Case {
                    std::int32_t jobs;
                    std::int64_t spinNs;
                    std::int32_t joins;
                };
                // empty jobs (the join alone), one short job per worker, and a stage like ctJpeg's (many jobs of 20 us)
                const Case cases[] = {{16, 0, 20000}, {16, 2000, 20000}, {256, 20000, 400}};
                for (const Case& c : cases) {
                    double multi[Rounds], single[Rounds];
                    for (std::int32_t r = 0; r < Rounds; ++r) {
                        // alternate the order, so that warm-up and drift hit both alike
                        if (r % 2 == 0) {
                            multi[r]  = JoinMicros<Ct::Synchronization::TaskGroup>(*compute.Get(), Workers, c.jobs, c.spinNs, c.joins);
                            single[r] = JoinMicros<Ct::Synchronization::TaskGroup_s>(*compute.Get(), Workers, c.jobs, c.spinNs, c.joins);
                        } else {
                            single[r] = JoinMicros<Ct::Synchronization::TaskGroup_s>(*compute.Get(), Workers, c.jobs, c.spinNs, c.joins);
                            multi[r]  = JoinMicros<Ct::Synchronization::TaskGroup>(*compute.Get(), Workers, c.jobs, c.spinNs, c.joins);
                        }
                    }
                    const double m = Median(multi, Rounds);
                    const double s = Median(single, Rounds);
                    wchar_t      line[200];
                    std::swprintf(line, 200, L"jobs %d x %lld ns, %u workers: TaskGroup %.2f us, TaskGroup_s %.2f us per join (_s/multi %.3f)\n", c.jobs,
                                  static_cast<long long>(c.spinNs), Workers, m, s, s / m);
                    Logger::WriteMessage(line);
                    std::fputws(line, stdout);
                }
                compute->Stop();
            }
            Ct::ConvertTaskToThread();
        }
    };

} // namespace ctJpegTests
