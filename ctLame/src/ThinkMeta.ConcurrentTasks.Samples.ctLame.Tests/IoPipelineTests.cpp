// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "pch.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "TestData.h"

// headers with imports last
#include "BlockPipe.h"
#include "Mp3WriterTask.h"
#include "SchedulerHandle.h"
#include "WavReaderTask.h"

import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks;
using namespace ThinkMeta::ConcurrentTasks::Samples::ctLame;

namespace ctLameTests {

    namespace {

        // The calling thread becomes a task, so that it can wait on the synchronization primitives.
        struct ThreadAsTask {
            ThreadAsTask() { ConvertThreadToTask(); }
            ~ThreadAsTask() { ConvertTaskToThread(); }
        };

        struct TempFile {
            wchar_t path[MAX_PATH];

            explicit TempFile(const wchar_t* prefix)
            {
                wchar_t directory[MAX_PATH];
                GetTempPathW(MAX_PATH, directory);
                GetTempFileNameW(directory, prefix, 0, path);
            }
            ~TempFile() { DeleteFileW(path); }
        };

        std::byte PatternByte(std::uint64_t position)
        {
            return static_cast<std::byte>((position * 2654435761ull) >> 13);
        }

        void WritePattern(const wchar_t* path, std::uint64_t size)
        {
            HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            Assert::IsTrue(file != INVALID_HANDLE_VALUE);
            static std::byte buffer[65536];
            for (std::uint64_t position = 0; position < size;) {
                const std::uint32_t chunk = static_cast<std::uint32_t>(size - position < sizeof(buffer) ? size - position : sizeof(buffer));
                for (std::uint32_t i = 0; i < chunk; ++i)
                    buffer[i] = PatternByte(position + i);
                unsigned long written = 0; // DWORD of WriteFile
                WriteFile(file, buffer, chunk, &written, nullptr);
                position += chunk;
            }
            CloseHandle(file);
        }

        // Copies the blocks of one pipe into another, like the encoder will turn PCM blocks into
        // MP3 blocks.
        void CopyBlocks(BlockPipe& from, BlockPipe& to)
        {
            BlockPipe::ReadBlock block{};
            while (from.AcquireReadBlock(block)) {
                std::byte* const out = to.AcquireWriteBlock();
                CopyMemory(out, block.data, block.size);
                to.CommitWriteBlock(block.size);
                from.ReleaseReadBlock();
            }
            to.SetEndOfStream();
        }

        void RunCopy(const wchar_t* source, const wchar_t* target, std::uint64_t offset, std::uint64_t limit, std::uint32_t blockCount, std::uint32_t blockSize,
                     IoStats& readStats, IoStats& writeStats, const std::byte* header, std::uint32_t headerSize)
        {
            const ThreadAsTask threadAsTask;
            Assert::IsTrue(AsyncIo::EnableAsyncIo());

            auto ioScheduler      = SchedulerHandle::Create(SchedulerKind::Default, 1);
            auto computeScheduler = SchedulerHandle::Create(SchedulerKind::Default, 2);
            ioScheduler->Start();
            computeScheduler->Start();

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
            ioScheduler->AddTask(&reader, TaskPriority::Normal);
            ioScheduler->AddTask(&writer, TaskPriority::Normal);
            computeScheduler->AddTask(&copy, TaskPriority::Normal);
            group.Wait();

            computeScheduler->Stop();
            ioScheduler->Stop();
            AsyncIo::DisableAsyncIo();

            Assert::AreEqual(static_cast<std::uint32_t>(ERROR_SUCCESS), reader.Error());
            Assert::AreEqual(static_cast<std::uint32_t>(ERROR_SUCCESS), writer.Error());
            readStats  = reader.Stats();
            writeStats = writer.Stats();
        }

        void AssertCopied(const wchar_t* target, std::uint64_t offset, std::uint64_t size, const std::byte* header, std::uint32_t headerSize)
        {
            HANDLE file = CreateFileW(target, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            Assert::IsTrue(file != INVALID_HANDLE_VALUE);
            LARGE_INTEGER fileSize{};
            GetFileSizeEx(file, &fileSize);
            Assert::AreEqual(size, static_cast<std::uint64_t>(fileSize.QuadPart));

            static std::byte buffer[65536];
            std::uint64_t    position = 0;
            bool             same     = true;
            while (position < size) {
                unsigned long read = 0; // DWORD of ReadFile
                ReadFile(file, buffer, sizeof(buffer), &read, nullptr);
                if (read == 0)
                    break;
                for (std::uint32_t i = 0; i < read; ++i) {
                    const std::byte expected = position + i < headerSize ? header[position + i] : PatternByte(offset + position + i);
                    same                     = same && buffer[i] == expected;
                }
                position += read;
            }
            CloseHandle(file);
            Assert::IsTrue(same, L"copied bytes differ");
        }

    } // namespace

    TEST_CLASS(IoPipelineTests){public:
                                    // Producer and consumer of a pipe on different schedulers, with a pipe that is much
                                    // smaller than the data, so that both sides wait on each other all the time.
                                    TEST_METHOD(BlockPipeAcrossSchedulers){const ThreadAsTask threadAsTask;
    auto first  = SchedulerHandle::Create(SchedulerKind::Default, 1);
    auto second = SchedulerHandle::Create(SchedulerKind::Default, 1);
    first->Start();
    second->Start();

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
    first->Stop();
    second->Stop();

    Assert::IsTrue(ordered);
} // namespace ctLameTests

// Reader, copy task and writer: the output is the data range of the input, the header
// overwrites its first bytes.
TEST_METHOD(CopiesDataRangeAndHeader)
{
    const TempFile source(L"ctr");
    const TempFile target(L"ctw");
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
    const TempFile source(L"ctr");
    const TempFile target(L"ctw");
    WritePattern(source.path, 500000);

    IoStats readStats;
    IoStats writeStats;
    RunCopy(source.path, target.path, 100, 300001, 8, 4096, readStats, writeStats, nullptr, 0);

    AssertCopied(target.path, 100, 300001, nullptr, 0);
}

TEST_METHOD(EmptyData)
{
    const TempFile source(L"ctr");
    const TempFile target(L"ctw");
    WritePattern(source.path, 44);

    IoStats readStats;
    IoStats writeStats;
    RunCopy(source.path, target.path, 44, ~0ull, 4, 4096, readStats, writeStats, nullptr, 0);

    AssertCopied(target.path, 44, 0, nullptr, 0);
}
}
;

} // namespace ctLameTests
