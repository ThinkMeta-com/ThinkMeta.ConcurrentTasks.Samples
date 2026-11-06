// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "pch.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>

#if !defined(_WIN32)
#    include <unistd.h>
#endif

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

        // a new empty file in the temp directory, deleted at the end of the test
        struct TempFile {
            Char path[Shared::File::MaxPath];

            explicit TempFile(const Char* prefix)
            {
#if defined(_WIN32)
                wchar_t directory[MAX_PATH];
                GetTempPathW(MAX_PATH, directory);
                GetTempFileNameW(directory, prefix, 0, path);
#else
                const char* directory = std::getenv("TMPDIR");
                FormatTo(path, "%s/%sXXXXXX", directory != nullptr && *directory != 0 ? directory : "/tmp", prefix);
                const int file = mkstemp(path);
                Assert::IsTrue(file >= 0);
                close(file);
#endif
            }
            ~TempFile() { Shared::File::Delete(path); }
        };

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

        // Copies the blocks of one pipe into another, like the encoder will turn PCM blocks into
        // MP3 blocks.
        void CopyBlocks(BlockPipe& from, BlockPipe& to)
        {
            BlockPipe::ReadBlock block{};
            while (from.AcquireReadBlock(block)) {
                std::byte* const out = to.AcquireWriteBlock();
                std::memcpy(out, block.data, block.size);
                to.CommitWriteBlock(block.size);
                from.ReleaseReadBlock();
            }
            to.SetEndOfStream();
        }

        void RunCopy(const Char* source, const Char* target, std::uint64_t offset, std::uint64_t limit, std::uint32_t blockCount, std::uint32_t blockSize,
                     IoStats& readStats, IoStats& writeStats, const std::byte* header, std::uint32_t headerSize)
        {
            Shared::TaskRuntime      runtime(1);
            Shared::RunningScheduler compute(SchedulerKind::Default, 2);
            Assert::IsTrue(runtime && compute);

            BlockPipe pcm;
            BlockPipe mp3;
            Assert::IsTrue(pcm.Initialize(blockCount, blockSize));
            Assert::IsTrue(mp3.Initialize(blockCount, blockSize));

            WavReaderTask::Settings readSettings;
            readSettings.path       = source;
            readSettings.dataOffset = offset;
            readSettings.byteLimit  = limit;
            WavReaderTask reader(pcm, readSettings);

            Mp3WriterTask::Settings writeSettings;
            writeSettings.path             = target;
            writeSettings.producerFinished = reader.FinishedFlag();
            Mp3WriterTask writer(mp3, writeSettings);
            writer.SetFinalHeader(header, headerSize);

            auto copy = Tasks::Task::Create([&] { CopyBlocks(pcm, mp3); });

            Synchronization::TaskGroup group;
            group.AddTask(&reader);
            group.AddTask(&writer);
            group.AddTask(&copy);
            runtime.Io()->AddTask(&reader, TaskPriority::Normal);
            runtime.Io()->AddTask(&writer, TaskPriority::Normal);
            compute->AddTask(&copy, TaskPriority::Normal);
            group.Wait();

            compute.Stop();
            runtime.End();

            Assert::AreEqual(0u, reader.Error());
            Assert::AreEqual(0u, writer.Error());
            readStats  = reader.Stats();
            writeStats = writer.Stats();
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

    TEST_CLASS(IoPipelineTests){public:
                                    // Producer and consumer of a pipe on different schedulers, with a pipe that is much
                                    // smaller than the data, so that both sides wait on each other all the time.
                                    TEST_METHOD(BlockPipeAcrossSchedulers){const Shared::TaskRuntime runtime(0);
    Shared::RunningScheduler first(SchedulerKind::Default, 1);
    Shared::RunningScheduler second(SchedulerKind::Default, 1);
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
        pipe.SetEndOfStream();
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
} // namespace LameTests

// Reader, copy task and writer: the output is the data range of the input, the header
// overwrites its first bytes.
TEST_METHOD(CopiesDataRangeAndHeader)
{
    const TempFile source(CT_TEXT("ctr"));
    const TempFile target(CT_TEXT("ctw"));
    std::uint64_t constexpr Size = 3 * 1024 * 1024 + 12345;
    WritePattern(source.path, Size);

    const auto header = Bytes('H', 'E', 'A', 'D', 'E', 'R');
    IoStats    readStats;
    IoStats    writeStats;
    std::uint64_t constexpr Offset = 44;
    RunCopy(source.path, target.path, Offset, ~0ull, 4, 64 * 1024, readStats, writeStats, header.data(), static_cast<std::uint32_t>(header.size()));

    AssertCopied(target.path, Offset, Size - Offset, header.data(), static_cast<std::uint32_t>(header.size()));
    Assert::AreEqual(Size - Offset, readStats.bytes);
    Assert::AreEqual(Size - Offset, writeStats.bytes);
}

// With a byte limit (the size of the data chunk) the reader stops there, even if the
// file goes on.
TEST_METHOD(StopsAtByteLimit)
{
    const TempFile source(CT_TEXT("ctr"));
    const TempFile target(CT_TEXT("ctw"));
    WritePattern(source.path, 500000);

    IoStats readStats;
    IoStats writeStats;
    RunCopy(source.path, target.path, 100, 300001, 8, 4096, readStats, writeStats, nullptr, 0);

    AssertCopied(target.path, 100, 300001, nullptr, 0);
}

TEST_METHOD(EmptyData)
{
    const TempFile source(CT_TEXT("ctr"));
    const TempFile target(CT_TEXT("ctw"));
    WritePattern(source.path, 44);

    IoStats readStats;
    IoStats writeStats;
    RunCopy(source.path, target.path, 44, ~0ull, 4, 4096, readStats, writeStats, nullptr, 0);

    AssertCopied(target.path, 44, 0, nullptr, 0);
}
}
;

} // namespace LameTests
