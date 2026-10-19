//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#include "pch.h"

#include "Checksums.h"
#include "TestData.h"

#include <libdeflate.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::ctDeflate;

namespace ctDeflateTests {

    TEST_CLASS(ChecksumTests)
    {
    public:
        TEST_METHOD(Crc32MatchesLibdeflate)
        {
            const auto data = MakeData(DataKind::Random, 100003);
            for (const std::size_t size : {0u, 1u, 7u, 8u, 9u, 63u, 4096u, 100003u}) {
                Assert::AreEqual(libdeflate_crc32(0, data.data(), size), Crc32(0, data.data(), size));
                Assert::AreEqual(libdeflate_crc32(0x12345678, data.data(), size), Crc32(0x12345678, data.data(), size));
            }
        }

        TEST_METHOD(Crc32CombineMatchesConcatenation)
        {
            const auto data = MakeData(DataKind::Text, 70001);
            for (const std::size_t split : {0u, 1u, 1000u, 65536u, 70001u}) {
                const auto a = Crc32(0, data.data(), split);
                const auto b = Crc32(0, data.data() + split, data.size() - split);
                Assert::AreEqual(Crc32(0, data.data(), data.size()), Crc32Combine(a, b, data.size() - split));
            }
        }

        TEST_METHOD(Adler32MatchesLibdeflate)
        {
            const auto data = MakeData(DataKind::Random, 100003);
            for (const std::size_t size : {0u, 1u, 5552u, 5553u, 100003u})
                Assert::AreEqual(libdeflate_adler32(1, data.data(), size), Adler32(1, data.data(), size));
        }
    };

} // namespace ctDeflateTests
