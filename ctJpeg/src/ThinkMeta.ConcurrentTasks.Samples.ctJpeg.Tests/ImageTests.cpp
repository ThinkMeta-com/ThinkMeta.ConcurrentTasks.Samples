// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "pch.h"

#include <cstdint>
#include <cstring>

#include "Image.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg;

namespace ctJpegTests {

    namespace {

        // A HeapAlloc copy of the bytes, as ReadPnm hands it to ParsePnm.
        std::uint8_t* HeapCopy(const char* bytes, std::size_t size)
        {
            auto* data = static_cast<std::uint8_t*>(HeapAlloc(GetProcessHeap(), 0, size));
            std::memcpy(data, bytes, size);
            return data;
        }

    } // namespace

    TEST_CLASS(ImageTests)
    {
    public:
        TEST_METHOD(ParsesPpmWithComment)
        {
            const char file[] = "P6\n# comment\n2 1\n255\n\x01\x02\x03\xff\xfe\xfd";
            Image      image;
            wchar_t    error[256];
            Assert::IsTrue(image.ParsePnm(HeapCopy(file, sizeof(file) - 1), sizeof(file) - 1, error, 256));
            Assert::AreEqual(2, image.Width());
            Assert::AreEqual(1, image.Height());
            Assert::AreEqual(3, image.Components());
            Assert::AreEqual(std::uint8_t{3}, image.Row(0)[2]);
            Assert::AreEqual(std::uint8_t{0xfd}, image.Row(0)[5]);
        }

        TEST_METHOD(ParsesPgm)
        {
            const char file[] = "P5 3 2 255 abcdef";
            Image      image;
            wchar_t    error[256];
            Assert::IsTrue(image.ParsePnm(HeapCopy(file, sizeof(file) - 1), sizeof(file) - 1, error, 256));
            Assert::AreEqual(1, image.Components());
            Assert::AreEqual(std::uint8_t{'d'}, image.Row(1)[0]);
        }

        TEST_METHOD(RejectsSixteenBit)
        {
            const char file[] = "P5 1 1 65535 \x01\x02";
            Image      image;
            wchar_t    error[256];
            Assert::IsFalse(image.ParsePnm(HeapCopy(file, sizeof(file) - 1), sizeof(file) - 1, error, 256));
            Assert::IsFalse(static_cast<bool>(image));
        }

        TEST_METHOD(RejectsTruncated)
        {
            const char file[] = "P6 2 2 255 \x01\x02\x03";
            Image      image;
            wchar_t    error[256];
            Assert::IsFalse(image.ParsePnm(HeapCopy(file, sizeof(file) - 1), sizeof(file) - 1, error, 256));
        }

        TEST_METHOD(RejectsAscii)
        {
            const char file[] = "P3 1 1 255 1 2 3";
            Image      image;
            wchar_t    error[256];
            Assert::IsFalse(image.ParsePnm(HeapCopy(file, sizeof(file) - 1), sizeof(file) - 1, error, 256));
        }
    };

} // namespace ctJpegTests
