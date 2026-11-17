// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "pch.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>

// headers with imports last
#include "FrameRing.h"
#include "TaskRuntime.h"

import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks;
using namespace ThinkMeta::ConcurrentTasks::Samples::Lame;
namespace Shared = ThinkMeta::ConcurrentTasks::Samples::Shared;

// The frame ring under load, with stand-in stages on a real scheduler: every frame passes Dispatch,
// Prepare, Resolve, Search and Encode once, the serial stages in frame order, and no stage sees a
// frame before the stage ahead of it is done. Each stage stamps the frame (modeExt as the stage
// number); a stage only counts what it sees (Assert throws, which a task must not), the test
// checks after the group.
namespace LameTests {

    namespace {

        enum Stage : std::int32_t { Dispatched = 1, Prepared, Resolved, Searched };

        constexpr std::uint32_t MaxStageTasks = 15;

        template <typename Function>
        class FunctionTask final : public Tasks::Task
        {
        public:
            explicit FunctionTask(Function function) noexcept : function_(function) {}
            void Run() noexcept override { function_(); }

        private:
            Function function_;
        };

        struct RingResult {
            std::int64_t encoded = 0;
            std::int64_t wrong   = 0;
        };

        RingResult RunRing(std::int64_t frames, std::uint32_t slots, std::uint32_t stageTasks)
        {
            Assert::IsTrue(stageTasks <= MaxStageTasks);
            FrameRing ring;
            Assert::IsTrue(ring.Initialize(slots));

            std::atomic<std::int64_t> wrong{0};
            RingResult                result;
            auto                      expect = [&](bool ok) {
                if (!ok)
                    wrong.fetch_add(1);
            };

            auto dispatch = [&] {
                for (std::int64_t n = 0; n < frames; ++n) {
                    FrameData& frame = ring.AcquireDispatch();
                    expect(frame.frameNumber == n);
                    frame.modeExt = Dispatched;
                    ring.CompleteDispatch();
                }
                ring.EndDispatch();
            };
            auto prepare = [&] {
                while (FrameData* frame = ring.AcquirePrepare()) {
                    expect(frame->modeExt == Dispatched);
                    frame->modeExt = Prepared;
                    ring.CompletePrepare(*frame);
                }
            };
            auto search = [&] {
                while (FrameData* frame = ring.AcquireSearch()) {
                    expect(frame->modeExt == Resolved);
                    frame->modeExt = Searched;
                    ring.CompleteSearch(*frame);
                }
            };
            auto resolve = [&] {
                std::int64_t next = 0;
                while (FrameData* frame = ring.AcquireResolve()) {
                    expect(frame->frameNumber == next++ && frame->modeExt == Prepared);
                    frame->modeExt = Resolved;
                    ring.CompleteResolve();
                }
            };
            auto encode = [&] {
                std::int64_t next = 0;
                while (FrameData* frame = ring.AcquireEncode()) {
                    expect(frame->frameNumber == next++ && frame->modeExt == Searched);
                    ring.CompleteEncode();
                }
                result.encoded = next;
            };

            {
                Shared::TaskRuntime      runtime(0);
                Shared::RunningScheduler compute(SchedulerKind::Default, stageTasks + 3);
                Assert::IsTrue(runtime && compute);
                FunctionTask dispatchTask(dispatch);
                FunctionTask resolveTask(resolve);
                FunctionTask encodeTask(encode);
                std::array<std::optional<FunctionTask<decltype(prepare)>>, MaxStageTasks> prepareTasks;
                std::array<std::optional<FunctionTask<decltype(search)>>, MaxStageTasks>  searchTasks;
                for (std::uint32_t i = 0; i < stageTasks; ++i) {
                    prepareTasks[i].emplace(prepare);
                    searchTasks[i].emplace(search);
                }

                Synchronization::TaskGroup group;
                group.AddTask(&dispatchTask);
                group.AddTask(&resolveTask);
                group.AddTask(&encodeTask);
                for (std::uint32_t i = 0; i < stageTasks; ++i) {
                    group.AddTask(&*prepareTasks[i]);
                    group.AddTask(&*searchTasks[i]);
                }
                // the priorities of the encoder graph: the stages behind Prepare with High
                compute->AddTask(&dispatchTask, TaskPriority::Normal);
                for (std::uint32_t i = 0; i < stageTasks; ++i)
                    compute->AddTask(&*prepareTasks[i], TaskPriority::Normal);
                compute->AddTask(&resolveTask, TaskPriority::High);
                for (std::uint32_t i = 0; i < stageTasks; ++i)
                    compute->AddTask(&*searchTasks[i], TaskPriority::High);
                compute->AddTask(&encodeTask, TaskPriority::High);
                group.Wait();
                compute.Stop();
            }

            result.wrong = wrong.load();
            return result;
        }

    } // namespace

    TEST_CLASS(FrameRingTests)
    {
    public:
        // Every frame through all five stages, in order where the stage is serial; with few slots the
        // ring wraps all the time, with many tasks per stage the parallel stages overtake each other.
        TEST_METHOD(EveryFrameThroughEveryStage)
        {
            for (std::int32_t round = 0; round < 5; ++round) {
                for (const std::uint32_t stageTasks : {1u, 2u, 7u, 15u}) {
                    const RingResult r = RunRing(20000, FrameRing::SlotsFor(stageTasks), stageTasks);
                    Assert::AreEqual(std::int64_t{20000}, r.encoded);
                    Assert::AreEqual(std::int64_t{0}, r.wrong);
                }
            }
        }

        // An empty stream ends every stage.
        TEST_METHOD(NoFrames)
        {
            const RingResult r = RunRing(0, 16, 4);
            Assert::AreEqual(std::int64_t{0}, r.encoded);
            Assert::AreEqual(std::int64_t{0}, r.wrong);
        }
    };

} // namespace LameTests
