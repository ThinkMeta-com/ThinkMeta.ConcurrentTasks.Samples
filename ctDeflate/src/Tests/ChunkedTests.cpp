//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#include "pch.h"

#include "ChunkedDeflate.h"
#include "Compressor.h"
#include "TestData.h"

#include <libdeflate.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::Deflate;

namespace DeflateTests {

    namespace {

        /// <summary>The chunked pipeline must produce exactly libdeflate's gzip stream.</summary>
        ResolveStats ExpectChunkedIdentical(const std::vector<std::uint8_t>& data, std::int32_t level, std::size_t chunkSize, const wchar_t* label)
        {
            libdeflate_compressor* oracle = libdeflate_alloc_compressor(level);
            const std::size_t      bound  = Compressor::GzipBound(data.size()) + 64;
            std::vector<std::uint8_t> expected(bound);
            std::vector<std::uint8_t> actual(bound);
            const std::size_t         expectedSize = libdeflate_gzip_compress(oracle, data.data(), data.size(), expected.data(), bound);
            libdeflate_free_compressor(oracle);

            ChunkOptions options;
            options.chunkSize = chunkSize;
            ResolveStats      stats;
            const std::size_t actualSize = CompressGzipChunked(level, data.data(), data.size(), actual.data(), options, &stats);

            std::size_t firstDiff = 0;
            while (firstDiff < expectedSize && firstDiff < actualSize && expected[firstDiff] == actual[firstDiff])
                ++firstDiff;
            if (expectedSize != actualSize || firstDiff != expectedSize) {
                wchar_t msg[512];
                swprintf_s(msg, L"%s size %zu level %d chunk %zu: oracle %zu bytes, chunked %zu bytes, first difference at byte %zu", label,
                           data.size(), level, chunkSize, expectedSize, actualSize, firstDiff);
                Assert::Fail(msg);
            }
            return stats;
        }

        void ExpectChunkedAllLevels(DataKind kind, const wchar_t* label, std::size_t size)
        {
            const auto data = MakeData(kind, size, size + 5);
            for (const std::size_t chunk : {std::size_t{4096}, std::size_t{20000}, std::size_t{65536}, std::size_t{1} << 20}) {
                std::size_t syncs = 0, repairs = 0, repaired = 0;
                for (std::int32_t level = 1; level <= Compressor::MaxLevel; ++level) {
                    const auto stats = ExpectChunkedIdentical(data, level, chunk, label);
                    syncs += stats.syncs;
                    repairs += stats.repairs;
                    repaired += stats.repairedBytes;
                }
                wchar_t msg[256];
                swprintf_s(msg, L"%s chunk %zu: syncs %zu, repairs %zu, repaired bytes %zu (levels 1-9)\n", label, chunk, syncs, repairs, repaired);
                Logger::WriteMessage(msg);
            }
        }

    } // namespace

    TEST_CLASS(ChunkedTests)
    {
    public:
        TEST_METHOD(Text) { ExpectChunkedAllLevels(DataKind::Text, L"text", 1500000); }
        TEST_METHOD(Random) { ExpectChunkedAllLevels(DataKind::Random, L"random", 300000); }
        TEST_METHOD(Zeros) { ExpectChunkedAllLevels(DataKind::Zeros, L"zeros", 400000); }
        TEST_METHOD(Periodic) { ExpectChunkedAllLevels(DataKind::Periodic, L"periodic", 400000); }
        TEST_METHOD(Dna) { ExpectChunkedAllLevels(DataKind::Dna, L"dna", 400000); }
        TEST_METHOD(Mixed) { ExpectChunkedAllLevels(DataKind::Mixed, L"mixed", 2000000); }
        TEST_METHOD(Binary) { ExpectChunkedAllLevels(DataKind::Binary, L"binary", 900000); }
        TEST_METHOD(Log) { ExpectChunkedAllLevels(DataKind::Log, L"log", 2500000); }

        TEST_METHOD(SmallInputs)
        {
            for (const std::size_t size : {std::size_t{0}, std::size_t{100}, std::size_t{1023}, std::size_t{1024}, std::size_t{5000}, std::size_t{9000},
                                           std::size_t{70000}}) {
                const auto data = MakeData(DataKind::Text, size, 3);
                for (std::int32_t level = 0; level <= Compressor::MaxLevel; ++level)
                    ExpectChunkedIdentical(data, level, 4096, L"small text");
            }
        }
    };

} // namespace DeflateTests
