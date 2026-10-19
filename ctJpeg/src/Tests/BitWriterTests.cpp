// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "pch.h"

#include <cstdint>

#include "BitWriter.h"
#include "ByteBuffer.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg;

namespace JpegTests {

    TEST_CLASS(BitWriterTests)
    {
    public:
        // The last byte is padded with 1 bits (JumpToByteBoundary).
        TEST_METHOD(PadsWithOnes)
        {
            ByteBuffer out;
            BitWriter  bw(out);
            bw.WriteBits(3, 0b101);
            bw.JumpToByteBoundary();
            Assert::AreEqual(std::size_t{1}, out.Size());
            Assert::AreEqual(std::uint8_t{0xBF}, out.Data()[0]);
        }

        // Every 0xFF in the entropy-coded data is followed by a stuffed zero byte.
        TEST_METHOD(StuffsZeroAfterFF)
        {
            ByteBuffer out;
            BitWriter  bw(out);
            bw.WriteBits(8, 0xFF);
            bw.WriteBits(8, 0x12);
            bw.JumpToByteBoundary();
            Assert::AreEqual(std::size_t{3}, out.Size());
            Assert::AreEqual(std::uint8_t{0xFF}, out.Data()[0]);
            Assert::AreEqual(std::uint8_t{0x00}, out.Data()[1]);
            Assert::AreEqual(std::uint8_t{0x12}, out.Data()[2]);
        }

        // Full 64-bit discharges, with and without 0xFF bytes, keep the bit order.
        TEST_METHOD(DischargesBigEndian)
        {
            ByteBuffer out;
            BitWriter  bw(out);
            for (std::int32_t i = 0; i < 8; ++i)
                bw.WriteBits(16, 0x0102 + 0x0202 * i); // 01 02 03 04 ... 0f 10
            bw.WriteBits(16, 0xFFFF);
            bw.JumpToByteBoundary();
            Assert::AreEqual(std::size_t{16 + 4}, out.Size());
            for (std::int32_t i = 0; i < 16; ++i)
                Assert::AreEqual(static_cast<std::uint8_t>(i + 1), out.Data()[i]);
            Assert::AreEqual(std::uint8_t{0xFF}, out.Data()[16]);
            Assert::AreEqual(std::uint8_t{0x00}, out.Data()[17]);
            Assert::AreEqual(std::uint8_t{0xFF}, out.Data()[18]);
            Assert::AreEqual(std::uint8_t{0x00}, out.Data()[19]);
        }

        // A symbol without a Huffman code (length 0) makes the writer unhealthy, as in jpegli.
        TEST_METHOD(ZeroLengthIsUnhealthy)
        {
            ByteBuffer out;
            BitWriter  bw(out);
            Assert::IsTrue(bw.Healthy());
            bw.WriteBits(0, 0);
            Assert::IsFalse(bw.Healthy());
        }
    };

} // namespace JpegTests
