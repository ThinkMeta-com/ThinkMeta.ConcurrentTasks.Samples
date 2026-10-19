// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "pch.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ByteBuffer.h"
#include "Encoder.h"
#include "EncoderOptions.h"
#include "Image.h"
#include "SimdFeatures.h"

// headers with imports last
#include "EncodeGraph.h"
#include "TaskRuntime.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg;
namespace Shared = ThinkMeta::ConcurrentTasks::Samples::Shared;
namespace Ct = ThinkMeta::ConcurrentTasks;

// The task graph against the sequential encoder: whatever the split of the work (bands, strips, banded scans, chunks,
// workers), the stages must put together exactly the file EncodeJpeg writes. This is the invariant behind Stitch
// (bands of a scan joined into the whole scan) and Splice (chunks of a scan joined into its bytes).
namespace JpegTests {

    namespace {

        // A deterministic photo-like test image: gradients, edges and noise, so that every scan has
        // runs, refinements and long EOB runs.
        std::uint8_t* MakePpm(std::int32_t width, std::int32_t height, std::size_t& size)
        {
            char           header[64];
            const auto     headerSize = static_cast<std::size_t>(std::snprintf(header, sizeof(header), "P6\n%d %d\n255\n", width, height));
            size                      = headerSize + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3;
            auto* data                = static_cast<std::uint8_t*>(HeapAlloc(GetProcessHeap(), 0, size));
            std::memcpy(data, header, headerSize);
            std::uint32_t  rng = 12345;
            std::uint8_t*  p   = data + headerSize;
            for (std::int32_t y = 0; y < height; ++y) {
                for (std::int32_t x = 0; x < width; ++x) {
                    rng                     = rng * 1664525u + 1013904223u;
                    const std::int32_t edge = ((x / 97) + (y / 61)) % 2 == 0 ? 60 : 0;
                    const std::int32_t n    = static_cast<std::int32_t>(rng >> 28);
                    *p++                    = static_cast<std::uint8_t>((x * 255 / width + edge + n) & 0xFF);
                    *p++                    = static_cast<std::uint8_t>((y * 255 / height + n) & 0xFF);
                    *p++                    = static_cast<std::uint8_t>(((x + y) * 128 / (width + height) + edge / 2 + n) & 0xFF);
                }
            }
            return data;
        }

        void ExpectGraphMatchesSequential(Image& image, const EncoderOptions& options, SimdLevel simd, std::uint32_t workers,
                                          const ParallelOptions& split, const ByteBuffer& expected, const wchar_t* label)
        {
            ByteBuffer   output;
            EncodeStatus status = EncodeStatus::Ok;
            {
                Shared::TaskRuntime      runtime(0);
                Shared::RunningScheduler compute(Ct::SchedulerKind::Default, workers);
                Assert::IsTrue(runtime && compute);
                EncodeGraph::Settings settings;
                settings.options = options;
                settings.simd    = simd;
                settings.workers = workers;
                settings.split   = split;
                EncodeGraph graph(image, settings, output, nullptr);
                status = graph.Run(*compute, *compute); // no node Write file: no I/O
                compute.Stop();
            }
            Assert::IsTrue(status == EncodeStatus::Ok, label);
            Assert::AreEqual(expected.Size(), output.Size(), label);
            Assert::IsTrue(std::memcmp(expected.Data(), output.Data(), expected.Size()) == 0, label);
        }

    } // namespace

    TEST_CLASS(GraphTests)
    {
    public:
        TEST_METHOD(AnySplitGivesTheSequentialFile)
        {
            std::size_t   size = 0;
            std::uint8_t* ppm  = MakePpm(1500, 1100, size);
            Image         image;
            wchar_t       error[256];
            Assert::IsTrue(image.ParsePnm(ppm, size, error, 256), error);

            const SimdLevel simd = HighestSimdLevel(CpuFeatures::Detect());
            for (const ChromaSubsampling sub : {ChromaSubsampling::Default, ChromaSubsampling::Yuv420}) {
                EncoderOptions options;
                options.useQuality  = true;
                options.quality     = 85;
                options.subsampling = sub;
                ByteBuffer expected;
                Assert::IsTrue(EncodeJpeg(image, options, simd, expected) == EncodeStatus::Ok);

                struct Case {
                    std::uint32_t   workers;
                    ParallelOptions split;
                    const wchar_t*  label;
                };
                const Case cases[] = {
                    {1, {}, L"1 worker, default split"},
                    {2, {}, L"2 workers, default split"},
                    {7, {}, L"7 workers, default split"},
                    {7, {.bandRows = 1}, L"7 workers, bands of one iMCU row"},
                    {4, {.scanBands = 1}, L"4 workers, whole scans only"},
                    {4, {.scanBands = 9}, L"4 workers, 9 bands per AC scan"},
                    {3, {.stripWidth = 64}, L"3 workers, strips of 64 columns"},
                };
                for (const Case& c : cases)
                    ExpectGraphMatchesSequential(image, options, simd, c.workers, c.split, expected, c.label);
            }
        }
    };

} // namespace JpegTests
