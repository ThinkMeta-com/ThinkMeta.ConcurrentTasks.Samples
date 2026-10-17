// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "pch.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "BlockPipe.h"
#include "Mp3WriterTask.h"
#include "SchedulerHandle.h"
#include "WavReaderTask.h"

import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks;
using namespace ThinkMeta::ConcurrentTasks::Samples::ctLame;

// Measurement of the I/O pipeline (reader on the I/O scheduler, a copy task on the
// compute scheduler, writer on the I/O scheduler) against CopyFileW. Runs only if the environment
// variable CTLAME_BENCH_FILE names a large input file; the output goes to the temp directory.

namespace ctLameTests {

    namespace {

        double Seconds(LARGE_INTEGER start, LARGE_INTEGER end)
        {
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);
            return static_cast<double>(end.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);
        }

        void Log(const wchar_t* format, ...)
        {
            wchar_t text[512];
            va_list args;
            va_start(args, format);
            _vsnwprintf_s(text, _TRUNCATE, format, args);
            va_end(args);
            Logger::WriteMessage(text);
        }

        double CopyWithPipeline(const wchar_t* source, const wchar_t* target, std::uint32_t blockCount, std::uint32_t blockSize, std::uint32_t inFlight,
                                IoStats& readStats, IoStats& writeStats)
        {
            ConvertThreadToTask();
            AsyncIo::EnableAsyncIo();
            auto ioScheduler      = SchedulerHandle::Create(SchedulerKind::Default, 1);
            auto computeScheduler = SchedulerHandle::Create(SchedulerKind::Default, 2);
            ioScheduler->Start();
            computeScheduler->Start();

            BlockPipe pcm;
            BlockPipe mp3;
            pcm.Initialize(blockCount, blockSize);
            mp3.Initialize(blockCount, blockSize);

            WavReaderTask::Settings readSettings;
            readSettings.path          = source;
            readSettings.readsInFlight = inFlight;
            WavReaderTask reader(pcm, readSettings);

            Mp3WriterTask::Settings writeSettings;
            writeSettings.path             = target;
            writeSettings.writesInFlight   = inFlight;
            writeSettings.producerFinished = reader.FinishedFlag();
            Mp3WriterTask writer(mp3, writeSettings);

            auto copy = Tasks::Task::Create([&] {
                BlockPipe::ReadBlock block{};
                while (pcm.AcquireReadBlock(block)) {
                    std::byte* const out = mp3.AcquireWriteBlock();
                    CopyMemory(out, block.data, block.size);
                    mp3.CommitWriteBlock(block.size);
                    pcm.ReleaseReadBlock();
                }
                mp3.SetEndOfStream();
            });

            LARGE_INTEGER start;
            LARGE_INTEGER end;
            QueryPerformanceCounter(&start);
            Synchronization::TaskGroup group;
            group.AddTask(&reader);
            group.AddTask(&writer);
            group.AddTask(&copy);
            ioScheduler->AddTask(&reader, TaskPriority::Normal);
            ioScheduler->AddTask(&writer, TaskPriority::Normal);
            computeScheduler->AddTask(&copy, TaskPriority::Normal);
            group.Wait();
            QueryPerformanceCounter(&end);

            computeScheduler->Stop();
            ioScheduler->Stop();
            AsyncIo::DisableAsyncIo();
            ConvertTaskToThread();

            Assert::AreEqual(static_cast<std::uint32_t>(ERROR_SUCCESS), reader.Error());
            Assert::AreEqual(static_cast<std::uint32_t>(ERROR_SUCCESS), writer.Error());
            readStats  = reader.Stats();
            writeStats = writer.Stats();
            return Seconds(start, end);
        }

        double CopyWithCopyFile(const wchar_t* source, const wchar_t* target)
        {
            LARGE_INTEGER start;
            LARGE_INTEGER end;
            QueryPerformanceCounter(&start);
            Assert::IsTrue(CopyFileW(source, target, FALSE) != 0);
            QueryPerformanceCounter(&end);
            return Seconds(start, end);
        }

    } // namespace

    TEST_CLASS(IoPipelineBenchmark){public: TEST_METHOD(PipelineVersusCopyFile){wchar_t source[MAX_PATH];
    if (GetEnvironmentVariableW(L"CTLAME_BENCH_FILE", source, MAX_PATH) == 0) {
        Logger::WriteMessage(L"CTLAME_BENCH_FILE not set, skipped");
        return;
    }
    wchar_t directory[MAX_PATH];
    wchar_t target[MAX_PATH];
    GetTempPathW(MAX_PATH, directory);
    swprintf_s(target, L"%lsctlame-bench.out", directory);

    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    GetFileAttributesExW(source, GetFileExInfoStandard, &attributes);
    const double megabytes = (static_cast<double>(attributes.nFileSizeHigh) * 4294967296.0 + attributes.nFileSizeLow) / (1024.0 * 1024.0);

    CopyWithCopyFile(source, target); // warm the file cache

    struct Config {
        std::uint32_t blockCount;
        std::uint32_t blockSize;
        std::uint32_t inFlight;
    };
    const Config configs[] = {{32, 256 * 1024, 4}, {16, 1024 * 1024, 4}, {8, 1024 * 1024, 2}, {64, 64 * 1024, 8}};

    for (std::int32_t run = 0; run < 3; ++run) {
        const double copyFile = CopyWithCopyFile(source, target);
        Log(L"run %d CopyFileW                          %7.3f s  %8.1f MB/s", run, copyFile, megabytes / copyFile);
        for (const Config& config : configs) {
            IoStats      readStats;
            IoStats      writeStats;
            const double seconds = CopyWithPipeline(source, target, config.blockCount, config.blockSize, config.inFlight, readStats, writeStats);
            Log(L"run %d pipeline %2u x %4u KiB, %u in flight %7.3f s  %8.1f MB/s | read: %u ops, %u high, %u prio changes, fill %u..%u | write: %u ops, %u "
                L"high, %u prio changes, fill %u..%u",
                run, config.blockCount, config.blockSize / 1024, config.inFlight, seconds, megabytes / seconds, readStats.operations, readStats.highOperations,
                readStats.priorityChanges, readStats.minFilled, readStats.maxFilled, writeStats.operations, writeStats.highOperations,
                writeStats.priorityChanges, writeStats.minFilled, writeStats.maxFilled);
        }
    }
    DeleteFileW(target);
} // namespace ctLameTests
}
;

} // namespace ctLameTests
