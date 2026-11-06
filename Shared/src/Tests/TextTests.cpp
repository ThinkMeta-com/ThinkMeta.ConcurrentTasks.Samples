// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "pch.h"

#include "Text.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using ThinkMeta::ConcurrentTasks::Samples::Shared::Char;
using ThinkMeta::ConcurrentTasks::Samples::Shared::Text;

// Text: the integer parsers against the results of wcstol, wcstoul and wcstoull with a 32-bit long (Windows), written
// out, so that both platforms check the same values; the other functions on a few cases.
namespace SharedTests {

    TEST_CLASS(TextTests)
    {
    public:
        TEST_METHOD(ToInt32)
        {
            CheckInt32(CT_TEXT("0"), 0, 1);
            CheckInt32(CT_TEXT("42"), 42, 2);
            CheckInt32(CT_TEXT("  -17x"), -17, 5);
            CheckInt32(CT_TEXT("\t+8"), 8, 3);
            CheckInt32(CT_TEXT("2147483647"), INT32_MAX, 10);
            CheckInt32(CT_TEXT("2147483648"), INT32_MAX, 10);
            CheckInt32(CT_TEXT("-2147483648"), INT32_MIN, 11);
            CheckInt32(CT_TEXT("-2147483649"), INT32_MIN, 11);
            CheckInt32(CT_TEXT("99999999999999999999999"), INT32_MAX, 23);
            CheckInt32(CT_TEXT("abc"), 0, 0);
            CheckInt32(CT_TEXT("-"), 0, 0);
            CheckInt32(CT_TEXT(""), 0, 0);
            CheckInt32(CT_TEXT("12.5"), 12, 2);
        }

        TEST_METHOD(ToUnsigned)
        {
            const Char* end = nullptr;
            Assert::AreEqual(4096u, Text::ToUInt32(CT_TEXT("4096"), &end));
            Assert::AreEqual(UINT32_MAX, Text::ToUInt32(CT_TEXT("4294967295")));
            Assert::AreEqual(UINT32_MAX, Text::ToUInt32(CT_TEXT("4294967296")));
            Assert::AreEqual(4294967291u, Text::ToUInt32(CT_TEXT("-5"))); // negated modulo 2^32, as wcstoul
            Assert::AreEqual(std::uint64_t{18446744073709551615ull}, Text::ToUInt64(CT_TEXT("18446744073709551615")));
            Assert::AreEqual(std::uint64_t{18446744073709551615ull}, Text::ToUInt64(CT_TEXT("18446744073709551616")));
            Assert::AreEqual(std::uint64_t{1048576}, Text::ToUInt64(CT_TEXT(" 1048576 "), &end));
            Assert::AreEqual(std::string(" "), Narrow(end));
            Assert::AreEqual(std::uint64_t{18446744073709551615ull}, Text::ToUInt64(CT_TEXT("-1")));
        }

        TEST_METHOD(ToDouble)
        {
            const Char* end = nullptr;
            Assert::AreEqual(6.5, Text::ToDouble(CT_TEXT("6.5"), &end));
            Assert::AreEqual(0, static_cast<int>(*end));
            Assert::AreEqual(-0.25, Text::ToDouble(CT_TEXT("-0.25k"), &end));
            Assert::AreEqual(std::string("k"), Narrow(end));
        }

        TEST_METHOD(CompareAndFind)
        {
            Assert::IsTrue(Text::Equal(CT_TEXT("lame"), CT_TEXT("lame")));
            Assert::IsFalse(Text::Equal(CT_TEXT("lame"), CT_TEXT("lam")));
            Assert::IsTrue(Text::Compare(CT_TEXT("a"), CT_TEXT("b")) < 0);
            Assert::IsTrue(Text::Compare(CT_TEXT("b"), CT_TEXT("a")) > 0);
            Assert::IsTrue(Text::EqualIgnoreCase(CT_TEXT("ctLame.EXE"), CT_TEXT("ctlame.exe")));
            Assert::IsFalse(Text::EqualIgnoreCase(CT_TEXT(".exe"), CT_TEXT(".ex")));
            Assert::AreEqual(std::size_t{5}, Text::Length(CT_TEXT("hello")));
            const Char* path = CT_TEXT("dir/sub/file.wav");
            Assert::IsTrue(Text::FindLast(path, '/') == path + 7);
            Assert::IsTrue(Text::Find(path, '/') == path + 3);
            Assert::IsNull(Text::Find(path, '\\'));
            Char buffer[5];
            Assert::AreEqual(std::size_t{4}, Text::Copy(buffer, 5, CT_TEXT("abcdef")));
            Assert::AreEqual(std::string("abcd"), Narrow(buffer));
        }

    private:
        static void CheckInt32(const Char* text, std::int32_t expected, std::size_t consumed)
        {
            const Char*        end   = nullptr;
            const std::int32_t value = Text::ToInt32(text, &end);
            Assert::AreEqual(expected, value);
            Assert::AreEqual(consumed, static_cast<std::size_t>(end - text));
        }

        // ASCII only, for the comparisons
        static std::string Narrow(const Char* text)
        {
            std::string result;
            for (; *text != 0; ++text)
                result += static_cast<char>(*text);
            return result;
        }
    };

} // namespace SharedTests
