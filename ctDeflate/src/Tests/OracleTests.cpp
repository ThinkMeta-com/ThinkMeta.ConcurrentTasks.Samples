//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#include "pch.h"

#include "Compressor.h"
#include "TestData.h"

#include <libdeflate.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate;

namespace ctDeflateTests {

    namespace {

        enum class Format { Raw, Gzip, Zlib };

        /// <summary>Compresses with libdeflate (the oracle) and with ctDeflate and requires identical bytes.</summary>
        void ExpectIdentical(const std::vector<std::uint8_t>& data, std::int32_t level, Format format, const wchar_t* label)
        {
            libdeflate_compressor* oracle = libdeflate_alloc_compressor(level);
            Assert::IsNotNull(oracle);
            const std::size_t          bound = Compressor::GzipBound(data.size()) + 64;
            std::vector<std::uint8_t>  expected(bound);
            std::vector<std::uint8_t>  actual(bound);
            std::size_t                expectedSize = 0;
            switch (format) {
                case Format::Raw: expectedSize = libdeflate_deflate_compress(oracle, data.data(), data.size(), expected.data(), bound); break;
                case Format::Gzip: expectedSize = libdeflate_gzip_compress(oracle, data.data(), data.size(), expected.data(), bound); break;
                case Format::Zlib: expectedSize = libdeflate_zlib_compress(oracle, data.data(), data.size(), expected.data(), bound); break;
            }
            libdeflate_free_compressor(oracle);

            auto        compressor = Compressor::Create(level);
            std::size_t actualSize = 0;
            switch (format) {
                case Format::Raw: actualSize = compressor->CompressDeflate(data.data(), data.size(), actual.data()); break;
                case Format::Gzip: actualSize = compressor->CompressGzip(data.data(), data.size(), actual.data()); break;
                case Format::Zlib: actualSize = compressor->CompressZlib(data.data(), data.size(), actual.data()); break;
            }

            std::size_t firstDiff = 0;
            while (firstDiff < expectedSize && firstDiff < actualSize && expected[firstDiff] == actual[firstDiff])
                ++firstDiff;
            if (expectedSize != actualSize || firstDiff != expectedSize) {
                wchar_t msg[512];
                swprintf_s(msg, L"%s size %zu level %d: oracle %zu bytes, ctDeflate %zu bytes, first difference at byte %zu", label, data.size(),
                           level, expectedSize, actualSize, firstDiff);
                Assert::Fail(msg);
            }
        }

        void ExpectIdenticalAllLevels(DataKind kind, const wchar_t* label, std::initializer_list<std::size_t> sizes)
        {
            for (const std::size_t size : sizes) {
                const auto data = MakeData(kind, size, size + 17);
                for (std::int32_t level = 0; level <= Compressor::MaxLevel; ++level)
                    ExpectIdentical(data, level, Format::Gzip, label);
            }
        }

    } // namespace

    TEST_CLASS(OracleTests)
    {
    public:
        TEST_METHOD(TinyInputs)
        {
            for (std::size_t size = 0; size <= 64; ++size) {
                for (std::int32_t level = 0; level <= Compressor::MaxLevel; ++level) {
                    ExpectIdentical(MakeData(DataKind::Text, size), level, Format::Gzip, L"text");
                    ExpectIdentical(MakeData(DataKind::Random, size), level, Format::Gzip, L"random");
                }
            }
        }

        TEST_METHOD(Text) { ExpectIdenticalAllLevels(DataKind::Text, L"text", {300, 511, 512, 4999, 5000, 5001, 10001, 65535, 65536, 300000, 1500000}); }

        TEST_METHOD(Random) { ExpectIdenticalAllLevels(DataKind::Random, L"random", {1000, 65535, 65536, 65537, 200000, 700000}); }

        TEST_METHOD(Zeros) { ExpectIdenticalAllLevels(DataKind::Zeros, L"zeros", {100, 32768, 32769, 65536, 1000000}); }

        TEST_METHOD(Periodic) { ExpectIdenticalAllLevels(DataKind::Periodic, L"periodic", {50000, 400000}); }

        TEST_METHOD(Dna) { ExpectIdenticalAllLevels(DataKind::Dna, L"dna", {20000, 400000}); }

        TEST_METHOD(Mixed) { ExpectIdenticalAllLevels(DataKind::Mixed, L"mixed", {2000000}); }

        TEST_METHOD(Binary) { ExpectIdenticalAllLevels(DataKind::Binary, L"binary", {100000, 900000}); }

        TEST_METHOD(RawAndZlibFormats)
        {
            const auto data = MakeData(DataKind::Mixed, 400000);
            for (std::int32_t level = 0; level <= Compressor::MaxLevel; ++level) {
                ExpectIdentical(data, level, Format::Raw, L"mixed raw");
                ExpectIdentical(data, level, Format::Zlib, L"mixed zlib");
            }
        }
    };

} // namespace ctDeflateTests
