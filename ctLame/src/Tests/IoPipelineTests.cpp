// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "pch.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "TestData.h"

// headers with imports last
#include "BlockPipe.h"
#include "Mp3WriterTask.h"
#include "TaskRuntime.h"
#include "WavReaderTask.h"

import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks;
using namespace ThinkMeta::ConcurrentTasks::Samples::Lame;
namespace Shared = ThinkMeta::ConcurrentTasks::Samples::Shared;

namespace LameTests {

    namespace {

        std::byte PatternByte(std::uint64_t position)
        {
            return static_cast<std::byte>((position * 2654435761ull) >> 13);
        }

        void WritePattern(const Char* path, std::uint64_t size)
        {
            const auto data = std::make_unique<std::byte[]>(static_cast<std::size_t>(size));
            for (std::uint64_t position = 0; position < size; ++position)
                data[position] = PatternByte(position);
            Assert::IsTrue(Shared::File::Write(path, data.get(), static_cast<std::size_t>(size)));
        }

        // Copies the blocks of the PCM pipe into the MP3 file, like the encoder turns PCM blocks into the MP3 stream,
        // and ends it with the header as the LAME tag.
        void CopyBlocks(BlockPipe& from, Mp3File& to, const std::byte* header, std::uint32_t headerSize)
        {
            BlockPipe::ReadBlock block{};
            while (from.AcquireReadBlock(block)) {
                to.Append(block.data, static_cast<std::int32_t>(block.size));
                from.ReleaseReadBlock();
            }
            if (headerSize > 0)
                std::memcpy(to.LameTagBuffer(), header, headerSize);
            to.End(headerSize);
        }

        void RunCopy(const Char* source, const Char* target, std::uint64_t offset, std::uint64_t limit, std::uint32_t blockCount, std::uint32_t blockSize,
                     const std::byte* header, std::uint32_t headerSize)
        {
            Shared::TaskRuntime      runtime(1);
            Shared::RunningScheduler compute(SchedulerKind::Default, 2);
            Assert::IsTrue(runtime && compute);

            BlockPipe pcm;
            Mp3File   mp3(Id3Bytes{});
            Assert::IsTrue(pcm.Initialize(blockCount, blockSize));
            Assert::IsTrue(mp3.Initialize(blockCount, blockSize));

            WavReaderTask::Settings readSettings;
            readSettings.path       = source;
            readSettings.dataOffset = offset;
            readSettings.byteLimit  = limit;
            WavReaderTask reader(pcm, readSettings);

            Mp3WriterTask::Settings writeSettings;
            writeSettings.path = target;
            Mp3WriterTask writer(mp3, writeSettings);

            auto copy = Tasks::Task::Create([&] { CopyBlocks(pcm, mp3, header, headerSize); });

            Synchronization::TaskGroup group;
            group.AddTask(&reader);
            group.AddTask(&writer);
            group.AddTask(&copy);
            runtime.Io()->AddTask(&reader, TaskPriority::Normal);
            runtime.Io()->AddTask(&writer, TaskPriority::High);
            compute->AddTask(&copy, TaskPriority::Normal);
            group.Wait();

            compute.Stop();
            runtime.End();

            Assert::AreEqual(0u, reader.Error());
            Assert::AreEqual(0u, writer.Error());
        }

        void AssertCopied(const Char* target, std::uint64_t offset, std::uint64_t size, const std::byte* header, std::uint32_t headerSize)
        {
            std::uint64_t fileSize = 0;
            Assert::IsTrue(Shared::File::Size(target, fileSize));
            Assert::AreEqual(size, fileSize);

            const auto data = std::make_unique<std::byte[]>(static_cast<std::size_t>(size));
            Assert::IsTrue(Shared::File::Read(target, data.get(), static_cast<std::size_t>(size)));
            bool same = true;
            for (std::uint64_t position = 0; position < size; ++position) {
                const std::byte expected = position < headerSize ? header[position] : PatternByte(offset + position);
                same                     = same && data[position] == expected;
            }
            Assert::IsTrue(same, L"copied bytes differ");
        }

    } // namespace

    TEST_CLASS(IoPipelineTests)
    {
    public:
        // Producer and consumer of a pipe on different schedulers, with a pipe that is much
        // smaller than the data, so that both sides wait on each other all the time.
        TEST_METHOD(BlockPipeAcrossSchedulers)
        {
            const Shared::TaskRuntime runtime(0);
            Shared::RunningScheduler  first(SchedulerKind::Default, 1);
            Shared::RunningScheduler  second(SchedulerKind::Default, 1);
            Assert::IsTrue(runtime && first && second);

            BlockPipe pipe;
            Assert::IsTrue(pipe.Initialize(3, 16));
            std::uint64_t constexpr Blocks = 20000;
            bool ordered                   = true;

            auto producer = Tasks::Task::Create([&] {
                for (std::uint64_t i = 0; i < Blocks; ++i) {
                    std::byte* const block                   = pipe.AcquireWriteBlock();
                    *reinterpret_cast<std::uint64_t*>(block) = i;
                    pipe.CommitWriteBlock(sizeof(std::uint64_t));
                }
                pipe.End();
            });
            auto consumer = Tasks::Task::Create([&] {
                BlockPipe::ReadBlock block{};
                std::uint64_t        expected = 0;
                while (pipe.AcquireReadBlock(block)) {
                    ordered = ordered && block.size == sizeof(std::uint64_t) && *reinterpret_cast<const std::uint64_t*>(block.data) == expected++;
                    pipe.ReleaseReadBlock();
                }
                ordered = ordered && expected == Blocks;
            });

            Synchronization::TaskGroup group;
            group.AddTask(&producer);
            group.AddTask(&consumer);
            first->AddTask(&producer, TaskPriority::Normal);
            second->AddTask(&consumer, TaskPriority::Normal);
            group.Wait();
            first.Stop();
            second.Stop();

            Assert::IsTrue(ordered);
        }

        // Reader, copy task and writer: the output is the data range of the input, the header
        // overwrites its first bytes.
        TEST_METHOD(CopiesDataRangeAndHeader)
        {
            const TempFile source(CT_TEXT("ctr"));
            const TempFile target(CT_TEXT("ctw"));
            std::uint64_t constexpr Size = 3 * 1024 * 1024 + 12345;
            WritePattern(source.path, Size);

            const auto header              = Bytes('H', 'E', 'A', 'D', 'E', 'R');
            std::uint64_t constexpr Offset = 44;
            RunCopy(source.path, target.path, Offset, ~0ull, 4, 64 * 1024, header.data(), static_cast<std::uint32_t>(header.size()));

            AssertCopied(target.path, Offset, Size - Offset, header.data(), static_cast<std::uint32_t>(header.size()));
        }

        // With a byte limit (the size of the data chunk) the reader stops there, even if the
        // file goes on.
        TEST_METHOD(StopsAtByteLimit)
        {
            const TempFile source(CT_TEXT("ctr"));
            const TempFile target(CT_TEXT("ctw"));
            WritePattern(source.path, 500000);

            RunCopy(source.path, target.path, 100, 300001, 8, 4096, nullptr, 0);

            AssertCopied(target.path, 100, 300001, nullptr, 0);
        }

        TEST_METHOD(EmptyData)
        {
            const TempFile source(CT_TEXT("ctr"));
            const TempFile target(CT_TEXT("ctw"));
            WritePattern(source.path, 44);

            RunCopy(source.path, target.path, 44, ~0ull, 4, 4096, nullptr, 0);

            AssertCopied(target.path, 44, 0, nullptr, 0);
        }
    };

} // namespace LameTests
