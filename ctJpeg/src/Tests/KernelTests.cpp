// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "pch.h"

#include <cstdint>

#include "PixelPhase.h"
#include "SimdFeatures.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg;

namespace JpegTests {

    // Every SIMD kernel of the pixel phase gives the bits of its scalar reference.
    TEST_CLASS(KernelTests)
    {
    public:
        TEST_METHOD(Sse2KernelsMatchScalar)
        {
            for (std::uint32_t seed = 1; seed <= 200; ++seed)
                Assert::IsTrue(PixelKernelSelfTest(SimdLevel::Sse2, seed), L"sse2 kernel differs from the scalar reference");
        }

        TEST_METHOD(Avx2KernelsMatchScalar)
        {
            if (HighestSimdLevel(CpuFeatures::Detect()) != SimdLevel::Avx2)
                Logger::WriteMessage("no AVX2: avx2 kernels not tested");
            else
                for (std::uint32_t seed = 1; seed <= 200; ++seed)
                    Assert::IsTrue(PixelKernelSelfTest(SimdLevel::Avx2, seed), L"avx2 kernel differs from the scalar reference");
        }
    };

} // namespace JpegTests
