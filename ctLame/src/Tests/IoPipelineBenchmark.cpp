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
#include "TaskRuntime.h"
#include "TestEnvironment.h"
#include "WavReaderTask.h"

import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks;
using namespace ThinkMeta::ConcurrentTasks::Samples::Lame;
namespace Shared = ThinkMeta::ConcurrentTasks::Samples::Shared;

// Measurement of the I/O pipeline (reader on the I/O scheduler, a copy task on the
// compute scheduler, writer on the I/O scheduler) against CopyFileW. Runs only if the environment
// variable CTLAME_BENCH_FILE names a large input file; the output goes to the temp directory.

namespace LameTests {

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

        double CopyWithPipeline(const wchar_t* source, const wchar_t* target, std::uint32_t blockCount, std::uint32_t blockSize)
        {
            Shared::TaskRuntime      runtime(1);
            Shared::RunningScheduler compute(SchedulerKind::Default, 2);
            Assert::IsTrue(runtime && compute);

            BlockPipe pcm;
            Mp3File   mp3(Id3Bytes{});
            pcm.Initialize(blockCount, blockSize);
            mp3.Initialize(blockCount, blockSize);

            WavReaderTask::Settings readSettings;
            readSettings.path = source;
            WavReaderTask reader(pcm, readSettings);

            Mp3WriterTask::Settings writeSettings;
            writeSettings.path = target;
            Mp3WriterTask writer(mp3, writeSettings);

            auto copy = Tasks::Task::Create([&] {
                BlockPipe::ReadBlock block{};
                while (pcm.AcquireReadBlock(block)) {
                    mp3.Append(block.data, static_cast<std::int32_t>(block.size));
                    pcm.ReleaseReadBlock();
                }
                mp3.End(0);
            });

            LARGE_INTEGER start;
            LARGE_INTEGER end;
            QueryPerformanceCounter(&start);
            Synchronization::TaskGroup group;
            group.AddTask(&reader);
            group.AddTask(&writer);
            group.AddTask(&copy);
            runtime.Io()->AddTask(&reader, TaskPriority::Normal);
            runtime.Io()->AddTask(&writer, TaskPriority::High);
            compute->AddTask(&copy, TaskPriority::Normal);
            group.Wait();
            QueryPerformanceCounter(&end);

            compute.Stop();
            runtime.End();

            Assert::AreEqual(static_cast<std::uint32_t>(ERROR_SUCCESS), reader.Error());
            Assert::AreEqual(static_cast<std::uint32_t>(ERROR_SUCCESS), writer.Error());
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

    TEST_CLASS(IoPipelineBenchmark)
    {
    public:
        TEST_METHOD(PipelineVersusCopyFile)
        {
            wchar_t source[MAX_PATH];
            if (!Shared::TestEnvironment::Get(L"CTLAME_BENCH_FILE", source)) {
                wchar_t text[200];
                Logger::WriteMessage(Shared::TestEnvironment::SkipText(text, L"CTLAME_BENCH_FILE", L"a large file to copy"));
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
            };
            const Config configs[] = {{16, 256 * 1024}, {4, 1024 * 1024}, {4, 4 * 1024 * 1024}, {64, 64 * 1024}};

            for (std::int32_t run = 0; run < 3; ++run) {
                const double copyFile = CopyWithCopyFile(source, target);
                Log(L"run %d CopyFileW                          %7.3f s  %8.1f MB/s", run, copyFile, megabytes / copyFile);
                for (const Config& config : configs) {
                    const double seconds = CopyWithPipeline(source, target, config.blockCount, config.blockSize);
                    Log(L"run %d pipeline %2u x %4u KiB            %7.3f s  %8.1f MB/s", run, config.blockCount, config.blockSize / 1024, seconds,
                        megabytes / seconds);
                }
            }
            DeleteFileW(target);
        }
    };

} // namespace LameTests
