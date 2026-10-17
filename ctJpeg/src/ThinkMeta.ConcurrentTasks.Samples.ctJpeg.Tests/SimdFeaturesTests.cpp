// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "pch.h"

#include "SimdFeatures.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg;

namespace ctJpegTests {

    TEST_CLASS(SimdFeaturesTests)
    {
    public:
        TEST_METHOD(NamesRoundTrip)
        {
            SimdLevel level = SimdLevel::Sse2;
            Assert::IsTrue(ParseSimdLevel(L"avx2", level));
            Assert::IsTrue(level == SimdLevel::Avx2);
            Assert::IsTrue(ParseSimdLevel(L"sse2", level));
            Assert::IsTrue(level == SimdLevel::Sse2);
            Assert::IsFalse(ParseSimdLevel(L"sse4.1", level)); // jpegli's SSE4 output equals SSE2: no own level
        }

        TEST_METHOD(ChooseNeverExceedsCpu)
        {
            CpuFeatures noAvx2;
            Assert::IsTrue(ChooseSimdLevel(SimdLevel::Avx2, noAvx2) == SimdLevel::Sse2);
            CpuFeatures avx2;
            avx2.avx2 = true;
            Assert::IsTrue(ChooseSimdLevel(SimdLevel::Avx2, avx2) == SimdLevel::Avx2);
            Assert::IsTrue(ChooseSimdLevel(SimdLevel::Sse2, avx2) == SimdLevel::Sse2);
        }
    };

} // namespace ctJpegTests
