// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "pch.h"

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <atomic>

// headers with imports last
#include "FileIo.h"
#include "GraphEdges.h"
#include "TestTasks.h"

import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus;

// The graph's own edges under load, with stand-in nodes on a real scheduler: the block ring
// (Read → Resample → Encode and Analyse) and the block pipe of FileRead and FileWrite. A node only
// counts what it sees (Assert throws, which a task must not); the test checks after the group.
namespace ctOpusTests {

    namespace {

        constexpr std::int64_t NoAbort = -1;

        struct RingResult {
            std::int64_t encoded  = 0; // blocks Encode took
            std::int64_t analysed = 0; // blocks Analyse took
            std::int64_t wrong    = 0; // samples of a block that held another block's values
        };

        // Read dispatches blocks, the workers fill each slot with its block number, Encode and Analyse
        // check every sample of the blocks in order; Encode aborts after block abortAt.
        RingResult RunRing(std::int64_t blocks, std::int64_t slots, std::uint32_t workers, bool analysed, std::int64_t abortAt)
        {
            constexpr std::int64_t Frames = BlockRing::BlockFrames;
            BlockRing              ring;
            Assert::IsTrue(ring.Initialize(slots, 1, analysed));
            ring.PublishEnd(blocks * Frames, blocks * Frames, blocks * Frames);

            std::atomic<std::int64_t> wrong{0};
            RingResult                result;
            auto                      checkSlot = [&](std::int64_t b) {
                const float* slot = ring.SlotOf(b);
                for (std::int64_t k = 0; k < Frames; ++k)
                    if (slot[k] != static_cast<float>(b))
                        wrong.fetch_add(1);
            };

            auto read = [&] {
                std::int64_t next = 0;
                while (next < blocks && ring.AcquireRead(next)) {
                    ring.CompleteRead(next);
                    ++next;
                }
                ring.EndRead();
            };
            auto resample = [&] {
                for (;;) {
                    const std::int64_t b = ring.AcquireResample();
                    if (b < 0)
                        return;
                    if (!ring.Aborted()) {
                        float* slot = ring.SlotOf(b);
                        for (std::int64_t k = 0; k < Frames; ++k)
                            slot[k] = static_cast<float>(b);
                    }
                    ring.CompleteResample(b);
                }
            };
            auto encode = [&] {
                std::int64_t next = 0;
                for (; ring.AcquireEncode(next); ++next) {
                    if (!ring.Aborted())
                        checkSlot(next);
                    if (next == abortAt)
                        ring.Abort();
                    ring.CompleteEncode(next);
                }
                result.encoded = next;
            };
            auto analyse = [&] {
                std::int64_t b = ring.AnalyseNext();
                for (; ring.AcquireAnalyse(b); ++b) {
                    if (!ring.Aborted())
                        checkSlot(b);
                    ring.CompleteAnalyse(b);
                }
                ring.DetachAnalyse();
                result.analysed = b;
            };

            TestSchedulers schedulers(workers + 3);
            Assert::IsTrue(schedulers.Valid());
            FunctionTask readTask(read);
            FunctionTask encodeTask(encode);
            FunctionTask analyseTask(analyse);
            std::vector<std::unique_ptr<FunctionTask<decltype(resample)>>> resampleTasks;
            for (std::uint32_t i = 0; i < workers; ++i)
                resampleTasks.push_back(std::make_unique<FunctionTask<decltype(resample)>>(resample));

            Ct::Synchronization::TaskGroup group;
            group.AddTask(&readTask);
            group.AddTask(&encodeTask);
            if (analysed)
                group.AddTask(&analyseTask);
            for (auto& task : resampleTasks)
                group.AddTask(task.get());
            schedulers.Compute()->AddTask(&encodeTask, Ct::TaskPriority::High);
            if (analysed)
                schedulers.Compute()->AddTask(&analyseTask, Ct::TaskPriority::High);
            schedulers.Compute()->AddTask(&readTask, Ct::TaskPriority::High);
            for (auto& task : resampleTasks)
                schedulers.Compute()->AddTask(task.get(), Ct::TaskPriority::Normal);
            group.Wait();

            result.wrong = wrong.load();
            return result;
        }

    } // namespace

    TEST_CLASS(BlockRingTests)
    {
    public:
        // Every block reaches Encode (and Analyse) once, in order, and no slot is reused before both
        // have its block: with few slots and many workers the ring wraps all the time.
        TEST_METHOD(AllBlocksInOrder)
        {
            for (std::int32_t round = 0; round < 10; ++round) {
                for (const bool analysed : {false, true}) {
                    for (const std::uint32_t workers : {1u, 2u, 7u}) {
                        const RingResult r = RunRing(200, 8, workers, analysed, NoAbort);
                        Assert::AreEqual(std::int64_t{200}, r.encoded);
                        Assert::AreEqual(analysed ? std::int64_t{200} : std::int64_t{0}, r.analysed);
                        Assert::AreEqual(std::int64_t{0}, r.wrong);
                    }
                }
            }
        }

        // An abort by Encode ends every stage: the run returns instead of hanging.
        TEST_METHOD(AbortEndsEveryStage)
        {
            for (std::int32_t round = 0; round < 10; ++round) {
                for (const std::int64_t abortAt : {std::int64_t{0}, std::int64_t{5}, std::int64_t{50}}) {
                    const RingResult r = RunRing(200, 8, 4, true, abortAt);
                    Assert::IsTrue(r.encoded > abortAt);
                    Assert::AreEqual(std::int64_t{0}, r.wrong);
                }
            }
        }
    };

    TEST_CLASS(BlockPipeTests)
    {
    public:
        // The producer's blocks reach the consumer in order with their sizes and contents, through
        // four blocks of 256 bytes.
        TEST_METHOD(BlocksInOrder)
        {
            constexpr std::int64_t Blocks = 100000;
            BlockPipe              pipe;
            Assert::IsTrue(pipe.Initialize(4, 256));

            std::int64_t received = 0;
            std::int64_t wrong    = 0;
            auto         produce  = [&] {
                for (std::int64_t i = 0; i < Blocks; ++i) {
                    std::byte* block = pipe.AcquireWriteBlock();
                    if (block == nullptr)
                        return;
                    const std::uint32_t size = 1 + static_cast<std::uint32_t>(i * 7 % 256);
                    for (std::uint32_t k = 0; k < size; ++k)
                        block[k] = static_cast<std::byte>(i + k);
                    pipe.CommitWriteBlock(size);
                }
                pipe.SetEndOfStream();
            };
            auto consume = [&] {
                BlockPipe::ReadBlock block{};
                for (; pipe.AcquireReadBlock(block); ++received) {
                    const std::uint32_t size = 1 + static_cast<std::uint32_t>(received * 7 % 256);
                    if (block.size != size)
                        ++wrong;
                    else
                        for (std::uint32_t k = 0; k < size; ++k)
                            if (block.data[k] != static_cast<std::byte>(received + k))
                                ++wrong;
                    pipe.ReleaseReadBlock();
                }
            };

            TestSchedulers schedulers(2);
            Assert::IsTrue(schedulers.Valid());
            FunctionTask                   producer(produce);
            FunctionTask                   consumer(consume);
            Ct::Synchronization::TaskGroup group;
            group.AddTask(&producer);
            group.AddTask(&consumer);
            schedulers.Compute()->AddTask(&producer, Ct::TaskPriority::Normal);
            schedulers.Compute()->AddTask(&consumer, Ct::TaskPriority::Normal);
            group.Wait();

            Assert::AreEqual(Blocks, received);
            Assert::AreEqual(std::int64_t{0}, wrong);
        }

        // A consumer that closes the pipe early ends a producer that waits for a free block.
        TEST_METHOD(CloseEndsProducer)
        {
            BlockPipe pipe;
            Assert::IsTrue(pipe.Initialize(4, 64));

            std::int64_t produced = 0;
            auto         produce  = [&] {
                while (std::byte* block = pipe.AcquireWriteBlock()) {
                    block[0] = std::byte{1};
                    pipe.CommitWriteBlock(1);
                    ++produced;
                }
            };
            auto consume = [&] {
                BlockPipe::ReadBlock block{};
                for (std::int32_t i = 0; i < 50 && pipe.AcquireReadBlock(block); ++i)
                    pipe.ReleaseReadBlock();
                pipe.Close();
            };

            TestSchedulers schedulers(2);
            Assert::IsTrue(schedulers.Valid());
            FunctionTask                   producer(produce);
            FunctionTask                   consumer(consume);
            Ct::Synchronization::TaskGroup group;
            group.AddTask(&producer);
            group.AddTask(&consumer);
            schedulers.Compute()->AddTask(&producer, Ct::TaskPriority::Normal);
            schedulers.Compute()->AddTask(&consumer, Ct::TaskPriority::Normal);
            group.Wait();

            Assert::IsTrue(produced >= 50);
        }
    };

} // namespace ctOpusTests
