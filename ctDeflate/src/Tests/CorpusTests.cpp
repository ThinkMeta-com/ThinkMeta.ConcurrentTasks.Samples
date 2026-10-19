//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#include "pch.h"

#include "ChunkedDeflate.h"
#include "Compressor.h"
#include "TestEnvironment.h"

#include <libdeflate.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::Deflate;
using ThinkMeta::ConcurrentTasks::Samples::Shared::TestEnvironment;

namespace DeflateTests {

    namespace {

        bool ReadFile(const wchar_t* path, std::vector<std::uint8_t>& data, std::size_t maxSize)
        {
            HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;
            LARGE_INTEGER size;
            GetFileSizeEx(file, &size);
            const auto n = static_cast<std::size_t>(size.QuadPart < static_cast<long long>(maxSize) ? size.QuadPart : maxSize);
            data.resize(n);
            unsigned long read = 0; // DWORD of ReadFile
            const bool    ok   = ::ReadFile(file, data.data(), static_cast<unsigned long>(n), &read, nullptr) != 0 && read == n;
            CloseHandle(file);
            return ok;
        }

    } // namespace

    /// <summary>
    /// The chunked pipeline on real files (the first 8 MiB of each), listed in the environment variable
    /// CTDEFLATE_TEST_FILES (paths separated by ';'); without it nothing is tested. Real data reaches resolver paths that
    /// the synthetic data does not.
    /// </summary>
    TEST_CLASS(CorpusTests)
    {
    public:
        TEST_METHOD(ChunkedMatchesLibdeflate)
        {
            static wchar_t list[32768];
            (void)TestEnvironment::Get(L"CTDEFLATE_TEST_FILES", list);
            const wchar_t* next   = list;
            std::size_t    tested = 0;
            wchar_t        path[MAX_PATH];
            while (TestEnvironment::NextPath(next, path)) {
                std::vector<std::uint8_t> data;
                if (!ReadFile(path, data, 8u << 20))
                    continue;
                ++tested;
                for (const std::int32_t level : {2, 5, 6, 7, 9}) {
                    libdeflate_compressor*    oracle = libdeflate_alloc_compressor(level);
                    const std::size_t         bound  = Compressor::GzipBound(data.size()) + 64;
                    std::vector<std::uint8_t> expected(bound);
                    std::vector<std::uint8_t> actual(bound);
                    const std::size_t         expectedSize = libdeflate_gzip_compress(oracle, data.data(), data.size(), expected.data(), bound);
                    libdeflate_free_compressor(oracle);
                    for (const std::size_t chunk : {std::size_t{16384}, std::size_t{262144}}) {
                        ChunkOptions options;
                        options.chunkSize            = chunk;
                        const std::size_t actualSize = CompressGzipChunked(level, data.data(), data.size(), actual.data(), options);
                        if (actualSize != expectedSize || std::memcmp(expected.data(), actual.data(), expectedSize) != 0) {
                            wchar_t msg[600];
                            swprintf_s(msg, L"%s level %d chunk %zu: oracle %zu bytes, chunked %zu bytes", path, level, chunk, expectedSize, actualSize);
                            Assert::Fail(msg);
                        }
                    }
                }
            }
            if (tested == 0) {
                wchar_t text[200];
                Logger::WriteMessage(TestEnvironment::SkipText(text, L"CTDEFLATE_TEST_FILES", L"files to compress, separated by ';'"));
            }
        }
    };

} // namespace DeflateTests
