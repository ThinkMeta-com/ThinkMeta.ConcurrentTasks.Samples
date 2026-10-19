//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#include "pch.h"

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "Compressor.h"
#include "TestData.h"

#include <libdeflate.h>

// headers with imports last
#include "DeflateGraph.h"
#include "TaskRuntime.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::Deflate;
namespace Shared = ThinkMeta::ConcurrentTasks::Samples::Shared;
namespace Ct = ThinkMeta::ConcurrentTasks;

// The task graph itself (DeflateGraph on real schedulers): the parallel algorithm with 1, 2 and 7 workers, from memory
// and from file to file, against libdeflate as the oracle; and a read error that must end the graph with the error
// instead of hanging.
namespace DeflateTests {

    namespace {

        std::vector<std::uint8_t> Oracle(const std::vector<std::uint8_t>& data, std::int32_t level)
        {
            libdeflate_compressor*    c = libdeflate_alloc_compressor(level);
            std::vector<std::uint8_t> out(Compressor::GzipBound(data.size()) + 64);
            out.resize(libdeflate_gzip_compress(c, data.data(), data.size(), out.data(), out.size()));
            libdeflate_free_compressor(c);
            return out;
        }

        std::wstring TempPath(const wchar_t* name)
        {
            wchar_t dir[MAX_PATH];
            GetTempPathW(MAX_PATH, dir);
            return std::wstring(dir) + L"ctDeflateGraphTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + name;
        }

        void WriteFileAll(const std::wstring& path, const std::vector<std::uint8_t>& data)
        {
            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            Assert::IsTrue(file != INVALID_HANDLE_VALUE);
            DWORD written = 0; // DWORD of WriteFile
            Assert::IsTrue(WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size());
            CloseHandle(file);
        }

        std::vector<std::uint8_t> ReadFileAll(const std::wstring& path)
        {
            HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            Assert::IsTrue(file != INVALID_HANDLE_VALUE);
            LARGE_INTEGER size;
            GetFileSizeEx(file, &size);
            std::vector<std::uint8_t> data(static_cast<std::size_t>(size.QuadPart));
            DWORD                     read = 0; // DWORD of ReadFile
            Assert::IsTrue(ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) && read == data.size());
            CloseHandle(file);
            return data;
        }

        void ExpectInMemory(DataKind kind, std::size_t size, std::int32_t level, std::uint32_t workers, std::size_t chunk, bool sequential)
        {
            const auto                data     = MakeData(kind, size, size + level);
            const auto                expected = Oracle(data, level);
            std::vector<std::uint8_t> out(Compressor::GzipBound(size) + 64);
            DeflateGraph::Result      result;
            {
                Shared::TaskRuntime      runtime(2); // the setup of every sample (Application.cpp)
                Shared::RunningScheduler compute(Ct::SchedulerKind::Default, workers);
                Assert::IsTrue(runtime && compute);
                ChunkOptions chunks;
                chunks.chunkSize = chunk;
                DeflateGraph graph({.level = level, .workers = workers, .chunks = chunks, .sequential = sequential});
                graph.AttachInput(data.data(), data.size());
                graph.AttachOutput(out.data());
                result = graph.Run(*runtime.Io(), *compute);
            }
            wchar_t label[160];
            swprintf_s(label, L"kind %d size %zu level %d workers %u chunk %zu%s", static_cast<int>(kind), size, level, workers, chunk,
                       sequential ? L" sequential" : L"");
            Assert::AreEqual(0u, result.error, label);
            Assert::AreEqual(expected.size(), static_cast<std::size_t>(result.outSize), label);
            Assert::IsTrue(std::memcmp(expected.data(), out.data(), expected.size()) == 0, label);
        }

    } // namespace

    TEST_CLASS(GraphTests)
    {
    public:
        // Many small chunks: the parse window, the feedback and the ring wrap around many times.
        TEST_METHOD(InMemoryMatchesOracle)
        {
            for (const std::uint32_t workers : {1u, 2u, 7u}) {
                for (const std::int32_t level : {1, 6, 9}) {
                    ExpectInMemory(DataKind::Mixed, 3 << 20, level, workers, 20000, false);
                    ExpectInMemory(DataKind::Log, 2 << 20, level, workers, 65536, false);
                }
            }
        }

        TEST_METHOD(InMemorySequentialMatchesOracle)
        {
            for (const std::int32_t level : {0, 3, 8})
                ExpectInMemory(DataKind::Text, 1 << 20, level, 2, 1 << 20, true);
        }

        TEST_METHOD(FileToFileMatchesOracle)
        {
            const auto data = MakeData(DataKind::Mixed, 12 << 20, 7);
            const auto in   = TempPath(L"in.bin");
            const auto out  = TempPath(L"out.gz");
            WriteFileAll(in, data);
            for (const bool sequential : {false, true}) {
                DeflateGraph::Result result;
                {
                    Shared::TaskRuntime      runtime(2); // the setup of every sample (Application.cpp)
                    Shared::RunningScheduler compute(Ct::SchedulerKind::Default, 4);
                    Assert::IsTrue(runtime && compute);
                    DeflateGraph graph({.level = 6, .workers = 4, .chunks = {}, .sequential = sequential});
                    Assert::AreEqual(0u, graph.OpenInput(in.c_str()));
                    Assert::AreEqual(0u, graph.OpenOutput(out.c_str()));
                    result = graph.Run(*runtime.Io(), *compute);
                }
                Assert::AreEqual(0u, result.error);
                const auto actual = ReadFileAll(out);
                Assert::IsTrue(actual == Oracle(data, 6), sequential ? L"sequential" : L"parallel");
            }
            DeleteFileW(in.c_str());
            DeleteFileW(out.c_str());
        }

        // A byte range of the input locked by another handle makes the reads of it fail (ERROR_LOCK_VIOLATION): the
        // graph must end and report the error, with either graph.
        TEST_METHOD(ReadErrorEndsTheGraph)
        {
            const auto data = MakeData(DataKind::Text, 8 << 20, 3);
            const auto in   = TempPath(L"locked.bin");
            const auto out  = TempPath(L"locked.gz");
            WriteFileAll(in, data);
            HANDLE locker = CreateFileW(in.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            Assert::IsTrue(locker != INVALID_HANDLE_VALUE);
            OVERLAPPED at{};
            at.Offset = 5 << 20;
            Assert::IsTrue(LockFileEx(locker, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1 << 20, 0, &at) != 0);
            for (const bool sequential : {false, true}) {
                DeflateGraph::Result result;
                {
                    Shared::TaskRuntime      runtime(2); // the setup of every sample (Application.cpp)
                    Shared::RunningScheduler compute(Ct::SchedulerKind::Default, 4);
                    Assert::IsTrue(runtime && compute);
                    DeflateGraph graph({.level = 6, .workers = 4, .chunks = {}, .sequential = sequential});
                    Assert::AreEqual(0u, graph.OpenInput(in.c_str()));
                    Assert::AreEqual(0u, graph.OpenOutput(out.c_str()));
                    result = graph.Run(*runtime.Io(), *compute);
                }
                Assert::AreEqual(static_cast<std::uint32_t>(ERROR_LOCK_VIOLATION), result.error, sequential ? L"sequential" : L"parallel");
            }
            UnlockFileEx(locker, 0, 1 << 20, 0, &at);
            CloseHandle(locker);
            DeleteFileW(in.c_str());
            DeleteFileW(out.c_str());
        }
    };

} // namespace DeflateTests
