// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "pch.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "Encoder.h"
#include "SessionConfig.h"
#include "TestData.h"

// headers with imports last
#include "EncodeGraph.h"
#include "TaskRuntime.h"

import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks;
using namespace ThinkMeta::ConcurrentTasks::Samples::Lame;
namespace Shared = ThinkMeta::ConcurrentTasks::Samples::Shared;

// A task graph is right if its output does not depend on the order in which its tasks run. Here all nodes of the
// encoder graph run on ONE thread, Read and Write included, and are started in many orders: with the priorities of
// the graph, all with the same priority, and with priorities given at random. The start order only decides who runs
// first; the priorities decide at every wait who goes on, so they vary the order through the whole run. Every run
// must write the file of the sequential graph, byte for byte. On one thread a missing dependency between two nodes
// shows as a wrong file, or as a run that does not end, far more reliably than with many threads. It is not strictly
// the same in every run: Read and Write go on when the system completes their request, which is not in the test's
// hand. A run that does not end hangs the test (the group has no wait with a time limit).
//
// The scheduler is a Default scheduler with one thread, not a LocalScheduler: the test thread waits for the group
// while the tasks run, and the completions of the file I/O resume tasks from another thread.
namespace LameTests {

    namespace {

        constexpr std::int32_t  Rate       = 44100;
        constexpr std::uint32_t Samples    = 4 * Rate; // per channel: about 150 frames, each slot of the ring is used several times
        constexpr std::uint32_t DataSize   = Samples * 2 * sizeof(std::int16_t);
        constexpr std::uint32_t HeaderSize = 44; // the graph is told where the samples are; it does not read the header
        constexpr std::uint32_t StageTasks = 3;  // three Prepare and three Search tasks, on the one thread
        constexpr std::uint32_t MaxNodes   = EncodeGraph::MaxNodes(StageTasks);

        // 16 bit stereo: a sawtooth per channel and noise, with a loud burst twice a second (long and short blocks)
        void WriteInput(const Char* path)
        {
            const auto    file = std::make_unique<std::byte[]>(HeaderSize + DataSize);
            auto* const   pcm  = reinterpret_cast<std::int16_t*>(file.get() + HeaderSize);
            std::uint32_t seed = 12345;
            for (std::uint32_t i = 0; i < Samples; ++i) {
                seed                     = seed * 1664525u + 1013904223u;
                const std::int32_t noise = static_cast<std::int32_t>((seed >> 16) & 0x7FFF) - 0x4000;
                const bool         burst = i % (Rate / 2) < Rate / 20;
                const std::int32_t left  = static_cast<std::int32_t>(i * 13 % 4000) - 2000 + noise / (burst ? 1 : 16);
                const std::int32_t right = static_cast<std::int32_t>(i * 29 % 6000) - 3000 + noise / (burst ? 2 : 16);
                pcm[2 * i]               = static_cast<std::int16_t>(left);
                pcm[2 * i + 1]           = static_cast<std::int16_t>(right);
            }
            Assert::IsTrue(Shared::File::Write(path, file.get(), HeaderSize + DataSize));
        }

        enum class Priorities { OfTheGraph, AllTheSame, AtRandom };

        struct Encoded {
            std::unique_ptr<std::byte[]> data;
            std::uint64_t                size = 0;
        };

        // the nodes in start order: 0 as the graph gives them (the order of its diagram), 1 reversed, else shuffled
        // with the variant as the seed
        void MakeOrder(std::uint32_t* order, std::uint32_t count, std::uint32_t variant)
        {
            for (std::uint32_t i = 0; i < count; ++i)
                order[i] = variant == 1 ? count - 1 - i : i;
            if (variant < 2)
                return;
            std::uint32_t seed = variant * 2654435761u;
            for (std::uint32_t i = count - 1; i > 0; --i) {
                seed                  = seed * 1664525u + 1013904223u;
                const std::uint32_t j = (seed >> 8) % (i + 1);
                const std::uint32_t t = order[i];
                order[i]              = order[j];
                order[j]              = t;
            }
        }

        // One encoding of the input with all nodes on the one thread of the scheduler; the file it wrote.
        Encoded Encode(ITaskScheduler& oneThread, const Char* input, bool sequential, std::uint32_t orderVariant, Priorities priorities)
        {
            const TempFile output(CT_TEXT("cto"));

            EncoderOptions options;
            options.SetInSamplerate(Rate);
            options.SetNumChannels(2);
            options.SetNumSamples(Samples);
            options.SetVbrQuality(4.0f);
            options.SetFindReplayGain(1); // with the two Gain tasks
            SessionConfig config;
            Assert::AreEqual(0, InitParams(options, config));
            const auto encoder = std::make_unique<Encoder>(); // too large for the stack
            encoder->Initialize(config);

            EncodeGraph::Settings settings;
            settings.inPath          = input;
            settings.outPath         = output.path;
            settings.dataOffset      = HeaderSize;
            settings.byteLimit       = DataSize;
            settings.maxInputSamples = Samples;
            settings.format          = {2, 16, false, false};
            settings.threads         = StageTasks;
            settings.sequential      = sequential;
            settings.readSize        = 16 * 1024; // small blocks, so that Read and Dispatch wait for each other

            EncodeGraph graph(*encoder, config, settings, nullptr);
            Assert::IsTrue(graph.Initialize());
            // the nodes of the graph, started here instead of in EncodeGraph::Run: in another order, with other
            // priorities, all on the one scheduler
            const EncodeGraph::Nodes nodes = graph.CreateNodes();
            Assert::IsTrue(nodes.count <= MaxNodes);
            std::uint32_t order[MaxNodes];
            MakeOrder(order, nodes.count, orderVariant);

            Synchronization::TaskGroup group;
            for (const EncodeGraph::Node& node : nodes)
                group.AddTask(node.task);
            std::uint32_t seed = (orderVariant + 1) * 2246822519u;
            for (std::uint32_t i = 0; i < nodes.count; ++i) {
                const EncodeGraph::Node& node     = nodes.first[order[i]];
                TaskPriority             priority = node.priority;
                if (priorities == Priorities::AllTheSame)
                    priority = TaskPriority::Normal;
                else if (priorities == Priorities::AtRandom) {
                    seed     = seed * 1664525u + 1013904223u;
                    priority = (seed >> 16 & 1) != 0 ? TaskPriority::High : TaskPriority::Normal;
                }
                oneThread.AddTask(node.task, priority);
            }
            group.Wait();

            const EncodeGraph::Result errors = graph.Errors();
            Assert::AreEqual(0u, errors.readError);
            Assert::AreEqual(0u, errors.writeError);
            Assert::AreEqual(0, errors.encodeError);

            Encoded encoded;
            Assert::IsTrue(Shared::File::Size(output.path, encoded.size));
            encoded.data = std::make_unique<std::byte[]>(static_cast<std::size_t>(encoded.size));
            Assert::IsTrue(Shared::File::Read(output.path, encoded.data.get(), static_cast<std::size_t>(encoded.size)));
            return encoded;
        }

        void RunOrders(Priorities priorities)
        {
            constexpr std::uint32_t Orders = 12;

            const Shared::TaskRuntime runtime(0); // no I/O scheduler: Read and Write run on the one thread too
            Shared::RunningScheduler  oneThread(SchedulerKind::Default, 1);
            Assert::IsTrue(runtime && oneThread);

            const TempFile input(CT_TEXT("cti"));
            WriteInput(input.path);

            const Encoded reference = Encode(*oneThread, input.path, true, 0, Priorities::OfTheGraph);
            Assert::IsTrue(reference.size > 16 * 1024, L"the reference is too small to be an MP3 file of the input");

            for (std::uint32_t variant = 0; variant < Orders; ++variant) {
                const Encoded encoded = Encode(*oneThread, input.path, false, variant, priorities);
                Char          text[100];
                FormatTo(text, CT_TEXT("start order %u"), variant);
                Assert::AreEqual(reference.size, encoded.size, text);
                Assert::IsTrue(std::memcmp(reference.data.get(), encoded.data.get(), static_cast<std::size_t>(reference.size)) == 0, text);
            }
            oneThread.Stop();
        }

    } // namespace

    TEST_CLASS(EncodeGraphOrderTests)
    {
    public:
        // the priorities of the graph: the later stages go first, whatever the start order
        TEST_METHOD(AnyStartOrder)
        {
            RunOrders(Priorities::OfTheGraph);
        }

        // all nodes alike: the start order alone decides who runs first
        TEST_METHOD(AnyStartOrderWithoutPriorities)
        {
            RunOrders(Priorities::AllTheSame);
        }

        // High or Normal at random: any stage may overtake any other, wherever a node waits
        TEST_METHOD(AnyStartOrderWithAnyPriorities)
        {
            RunOrders(Priorities::AtRandom);
        }
    };

} // namespace LameTests
