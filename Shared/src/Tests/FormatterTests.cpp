// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "pch.h"

#include "Formatter.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using ThinkMeta::ConcurrentTasks::Samples::Shared::Formatter;

// Formatter against the UCRT (snprintf, swprintf) as the oracle: every case must give the same text and the same
// length. The floating-point cases cover the formats of the samples and more, with fixed, random and tie values.
namespace SharedTests {

    TEST_CLASS(FormatterTests)
    {
    public:
        TEST_METHOD(Integers)
        {
            Mismatches m;
            for (const int v : {0, 1, -1, 7, 42, -42, 99, 100, 12345, -98765, INT32_MAX, INT32_MIN}) {
                for (const char* f : {"%d",    "%i",     "%5d",  "%-5d|", "%05d", "%02d", "%2i",  "%+d", "% d",  "%.3d", "%.0d",
                                      "%8.3d", "%-08d|", "%04X", "%x",    "%X",   "%u",   "%10u", "%hd", "%hhd", "%hu",  "%hhu"})
                    m.Narrow(f, v);
            }
            for (const long long v : {0ll, -1ll, 1234567890123ll, INT64_MAX, INT64_MIN}) {
                m.Narrow("%lld", v);
                m.Narrow("%llu", v);
                m.Narrow("%20lld|", v);
                m.Narrow("%llx", v);
            }
            for (const long v : {0l, -5l, 2147483647l}) {
                m.Narrow("%ld", v);
                m.Narrow("%lu", v);
            }
            for (const std::size_t v : {std::size_t{0}, std::size_t{4096}, SIZE_MAX}) {
                m.Narrow("%zu", v);
                m.Narrow("%10zu", v);
            }
            m.Narrow("%*d|%-*d|", 6, 12, 6, 12);
            m.Narrow("%.*d", 4, 7);
            m.Narrow("100%% done %d%%", 5);
            m.Wide(L"%d %5i %04X %lld %zu %%", 12, -3, 0xBEEF, -9ll, std::size_t{77});
            m.Check();
        }

        TEST_METHOD(Strings)
        {
            Mismatches m;
            m.Narrow("[%s] [%10s] [%-10s] [%.4s] [%.0s] [%3s]", "text", "text", "text", "longer text", "x", "abcdef");
            m.Narrow("[%c%c] [%3c] [%-3c]", 'a', 'b', 'c', 'd');
            m.Narrow("%s, %s", "libopus 1.6.1", "libopusenc 0.2.1");
            m.Wide(L"[%s] [%ls] [%hs] [%-10ls] [%10hs] [%.4s]", L"wide", L"wide", "narrow", L"left", "right", L"truncated");
            m.Wide(L"[%c] [%lc] [%hc] [%3c]", L'w', L'x', 'n', L'y');
            m.Wide(L"cannot open '%ls' (error %lu)", L"C:\\path\\file.ppm", 2ul);
            m.Check();

            // non-ASCII: wide into narrow is UTF-8, narrow (UTF-8) into wide is UTF-16
            char narrow[64];
            Formatter::Format(narrow, 64, "%ls|%-6ls|", L"\u00C4\u20AC\U0001F600", L"\u00E9");
            Assert::AreEqual(std::string("\xC3\x84\xE2\x82\xAC\xF0\x9F\x98\x80|\xC3\xA9    |"), std::string(narrow));
            wchar_t wide[64];
            Formatter::Format(wide, 64, L"%hs|", "\xC3\x84\xE2\x82\xAC\xF0\x9F\x98\x80");
            Assert::AreEqual(std::wstring(L"\u00C4\u20AC\U0001F600|"), std::wstring(wide));
            Formatter::Format(narrow, 64, "[%s] [%ls]", static_cast<const char*>(nullptr), static_cast<const wchar_t*>(nullptr));
            Assert::AreEqual(std::string("[(null)] [(null)]"), std::string(narrow));
        }

        TEST_METHOD(Truncation)
        {
            char       expected[8], actual[8];
            const int  e = std::snprintf(expected, sizeof(expected), "%s-%d", "abcdef", 12345);
            const auto a = Formatter::Format(actual, 8, "%s-%d", "abcdef", 12345);
            Assert::AreEqual(e, static_cast<int>(a));
            Assert::AreEqual(std::string(expected), std::string(actual));
            Assert::AreEqual(12, static_cast<int>(Formatter::Format(static_cast<char*>(nullptr), 0, "%s-%d", "abcdef", 12345)));
            wchar_t w[4];
            Assert::AreEqual(6, static_cast<int>(Formatter::Format(w, 4, L"%ls", L"abcdef")));
            Assert::AreEqual(std::wstring(L"abc"), std::wstring(w));
        }

        TEST_METHOD(FloatsFixedValues)
        {
            const double values[] = {0.0,
                                     -0.0,
                                     0.5,
                                     1.5,
                                     2.5,
                                     -2.5,
                                     0.125,
                                     0.375,
                                     1.0 / 3.0,
                                     2.0 / 3.0,
                                     9.5,
                                     99.5,
                                     999.9999,
                                     9.9999995,
                                     0.00001234,
                                     0.0001,
                                     0.00009999995,
                                     44.1,
                                     48.0,
                                     128.0,
                                     1000000.0,
                                     123456789.0,
                                     1e15,
                                     1e16,
                                     1e21,
                                     1e22,
                                     1e300,
                                     1.7976931348623157e308,
                                     2.2250738585072014e-308,
                                     4.9406564584124654e-324,
                                     1e-300,
                                     0.1,
                                     0.2,
                                     0.3,
                                     0.7,
                                     5e-5,
                                     5e-7,
                                     0.05,
                                     0.005,
                                     0.0005,
                                     12.345,
                                     2.675,
                                     1.005,
                                     std::numeric_limits<double>::infinity(),
                                     -std::numeric_limits<double>::infinity(),
                                     std::numeric_limits<double>::quiet_NaN(),
                                     -std::numeric_limits<double>::quiet_NaN()};
            Mismatches   m;
            for (const double v : values) {
                for (const char* f : Formats)
                    m.Narrow(f, v);
                m.Wide(L"%g %.3f %0.6g %10.1f %e", v, v, v, v, v);
            }
            m.Check();
        }

        TEST_METHOD(FloatsRandom)
        {
            std::mt19937_64 random(20261020);
            Mismatches      m;
            // values of every size: random bit patterns (no NaN or infinity)
            for (std::int32_t i = 0; i < 20000; ++i) {
                std::uint64_t bits = random();
                if (((bits >> 52) & 0x7FF) == 0x7FF)
                    bits ^= std::uint64_t{1} << 62;
                double v;
                std::memcpy(&v, &bits, sizeof(v));
                for (const char* f : Formats)
                    m.Narrow(f, v);
            }
            // the sizes the samples print: 10^-8 to 10^12
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            std::uniform_int_distribution<int>     power(-8, 12);
            for (std::int32_t i = 0; i < 100000; ++i) {
                const double v = unit(random) * std::pow(10.0, power(random));
                for (const char* f : Formats)
                    m.Narrow(f, v);
            }
            m.Check();
        }

        TEST_METHOD(FloatsTies)
        {
            // n / 2^k is exact in binary, so many of these are exact ties at the precision of the format
            std::mt19937_64                    random(42);
            std::uniform_int_distribution<int> numerator(0, 2000000);
            std::uniform_int_distribution<int> shift(1, 12);
            Mismatches                         m;
            for (std::int32_t i = 0; i < 100000; ++i) {
                const double v = std::ldexp(static_cast<double>(numerator(random)), -shift(random));
                for (const char* f : Formats)
                    m.Narrow(f, v);
            }
            m.Check();
        }

    private:
        // the formats of the samples, and the other floating-point conversions
        static constexpr const char* Formats[] = {"%f",    "%.0f",  "%.1f",  "%.2f",     "%.3f",  "%5.0f", "%8.1f",   "%9.2f",  "%10.1f", "%6.1f", "%g",
                                                  "%0.2g", "%0.3g", "%0.4g", "%0.6g",    "%4.3g", "%5.4g", "%.1g",    "%.10g",  "%.17g",  "%e",    "%.0e",
                                                  "%.3e",  "%E",    "%G",    "%-12.3f|", "%+.2f", "% .2f", "%012.4f", "%08.3g", "%.20f",  "%.15e"};

        // collects the cases that differ from the UCRT; Check fails with the first ones
        class Mismatches
        {
        public:
            template <typename... Args>
            void Narrow(const char* format, Args... args)
            {
                char       expected[1024], actual[1024];
                const int  e = std::snprintf(expected, sizeof(expected), format, args...);
                const auto a = Formatter::Format(actual, sizeof(actual), format, args...);
                if (e != a || std::strcmp(expected, actual) != 0)
                    Add(Widen(format) + L": expected \"" + Widen(expected) + L"\", got \"" + Widen(actual) + L"\"");
                ++cases_;
            }

            template <typename... Args>
            void Wide(const wchar_t* format, Args... args)
            {
                wchar_t    expected[1024], actual[1024];
                const int  e = std::swprintf(expected, 1024, format, args...);
                const auto a = Formatter::Format(actual, 1024, format, args...);
                if (e != a || std::wcscmp(expected, actual) != 0)
                    Add(std::wstring(format) + L": expected \"" + expected + L"\", got \"" + actual + L"\"");
                ++cases_;
            }

            void Check() const
            {
                Logger::WriteMessage((std::to_wstring(cases_) + L" cases, " + std::to_wstring(count_) + L" mismatches").c_str());
                if (count_ != 0)
                    Assert::Fail((std::to_wstring(count_) + L" of " + std::to_wstring(cases_) + L" differ:\n" + first_).c_str());
            }

        private:
            static std::wstring Widen(const char* text) { return std::wstring(text, text + std::strlen(text)); }

            void Add(const std::wstring& line)
            {
                if (count_++ < 20)
                    first_ += line + L"\n";
            }

            std::int64_t cases_ = 0;
            std::int64_t count_ = 0;
            std::wstring first_;
        };
    };

} // namespace SharedTests
