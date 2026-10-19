// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "pch.h"

#include <array>
#include <cstdint>

#include "ByteBuffer.h"
#include "JpegTypes.h"
#include "Markers.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg;

namespace ctJpegTests {

    namespace {

        // The frame of cjpegli for a 33x31 RGB image with default settings: YCbCr 4:4:4,
        // component ids 1..3, one quantization table per component.
        FrameInfo Frame33x31()
        {
            FrameInfo frame;
            frame.width      = 33;
            frame.height     = 31;
            frame.components = 3;
            for (std::int32_t c = 0; c < 3; ++c)
                frame.component[static_cast<std::size_t>(c)] = {static_cast<std::uint8_t>(c + 1), 1, 1, c};
            return frame;
        }

        void AssertBytes(const std::uint8_t* expected, std::size_t size, const ByteBuffer& actual)
        {
            Assert::AreEqual(size, actual.Size());
            for (std::size_t i = 0; i < size; ++i)
                Assert::AreEqual(expected[i], actual.Data()[i]);
        }

    } // namespace

    TEST_CLASS(MarkerTests)
    {
    public:
        // Byte for byte the SOF2 of cjpegli-oracle for edge-33x31.ppm -q 85 (offset 201).
        TEST_METHOD(SofMatchesJpegli)
        {
            ByteBuffer         out;
            const std::uint8_t expected[] = {0xFF, 0xC2, 0x00, 0x11, 0x08, 0x00, 0x1F, 0x00, 0x21, 0x03,
                                             0x01, 0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x02};
            Assert::IsTrue(WriteSof(out, Frame33x31(), false));
            AssertBytes(expected, sizeof(expected), out);
        }

        // The interleaved DC scan of the same file (offset 294): all components on tables 0/0.
        TEST_METHOD(DcSosMatchesJpegli)
        {
            ByteBuffer         out;
            ScanInfo           scan;
            scan.componentCount            = 3;
            scan.components                = {0, 1, 2, 0};
            const std::int32_t slots[3]    = {0, 0, 0};
            const std::uint8_t expected[]  = {0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00};
            Assert::IsTrue(WriteSos(out, Frame33x31(), scan, slots, slots));
            AssertBytes(expected, sizeof(expected), out);
        }

        // A refinement scan: Ss 3, Se 63, Ah 2, Al 1 (offset 502).
        TEST_METHOD(RefinementSosMatchesJpegli)
        {
            ByteBuffer         out;
            ScanInfo           scan;
            scan.componentCount           = 1;
            scan.components               = {0, 0, 0, 0};
            scan.ss                       = 3;
            scan.se                       = 63;
            scan.ah                       = 2;
            scan.al                       = 1;
            const std::int32_t slots[1]   = {0};
            const std::uint8_t expected[] = {0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x03, 0x3F, 0x21};
            Assert::IsTrue(WriteSos(out, Frame33x31(), scan, slots, slots));
            AssertBytes(expected, sizeof(expected), out);
        }

        // DQT: tables in index order, zigzag order inside, 16-bit precision only where needed.
        TEST_METHOD(DqtPrecisionAndOrder)
        {
            std::array<QuantTable, NumQuantTables> tables{};
            for (std::int32_t k = 0; k < DctSize2; ++k) {
                tables[0].values[static_cast<std::size_t>(k)] = static_cast<std::uint16_t>(k + 1);
                tables[1].values[static_cast<std::size_t>(k)] = 2;
                tables[2].values[static_cast<std::size_t>(k)] = 300;
            }
            ByteBuffer out;
            bool       baseline = true;
            Assert::IsTrue(WriteDqt(out, Frame33x31(), tables.data(), baseline));
            Assert::IsFalse(baseline);
            Assert::AreEqual(std::size_t{4 + 65 + 65 + 129}, out.Size());
            const std::uint8_t* d = out.Data();
            Assert::AreEqual(std::uint8_t{0x00}, d[4]);           // table 0, 8 bit
            Assert::AreEqual(std::uint8_t{1}, d[5]);              // zigzag 0 = natural 0
            Assert::AreEqual(std::uint8_t{9}, d[7]);              // zigzag 2 = natural 8
            Assert::AreEqual(std::uint8_t{0x01}, d[4 + 65]);      // table 1
            Assert::AreEqual(std::uint8_t{0x12}, d[4 + 130]);     // table 2, 16 bit
            Assert::AreEqual(std::uint8_t{300 >> 8}, d[4 + 131]); // big-endian
        }

        TEST_METHOD(DhtLength)
        {
            HuffmanTable table;
            table.bits[1]      = 1;
            table.bits[2]      = 2;
            table.values[0]    = 0;
            table.values[1]    = 1;
            table.values[2]    = 2;
            const std::uint8_t slot = 0x10;
            ByteBuffer         out;
            Assert::IsTrue(WriteDht(out, &table, &slot, 1));
            Assert::AreEqual(std::size_t{4 + 1 + 16 + 3}, out.Size());
            Assert::AreEqual(std::uint8_t{0}, out.Data()[2]);
            Assert::AreEqual(std::uint8_t{2 + 1 + 16 + 3}, out.Data()[3]);
            Assert::AreEqual(std::uint8_t{0x10}, out.Data()[4]);
        }
    };

} // namespace ctJpegTests
