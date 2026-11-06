// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwchar>

/// <summary>
/// A text literal of the samples' character type (Char): L"text" on Windows, "text" (UTF-8) on Linux.
/// </summary>
#if defined(_WIN32)
#    define CT_TEXT(text) L##text
#else
#    define CT_TEXT(text) text
#endif

/// <summary>
/// The name of the entry point that receives the command line as Char**: wmain (UTF-16) on Windows, main (UTF-8) on Linux.
/// <code>int CT_MAIN(int argc, Shared::Char** argv)</code>
/// </summary>
#if defined(_WIN32)
#    define CT_MAIN wmain
#else
#    define CT_MAIN main
#endif

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    /// <summary>
    /// The character type of the samples' text, command lines and paths: wchar_t (UTF-16) on Windows, char (UTF-8) on Linux,
    /// the same as the file names of ThinkMeta.ConcurrentTasks (Platform::FileName). Literals: CT_TEXT("...").
    /// <para>Preliminary.</para>
    /// </summary>
#if defined(_WIN32)
    using Char = wchar_t;
#else
    using Char = char;
#endif

    /// <summary>
    /// The few text functions the samples use, for Char text: the parts of wcslen, wcscmp, _wcsicmp, wcsrchr, wcscpy_s,
    /// wcstol, wcstoul, wcstoull and wcstod they need, with the same results.
    /// <para>The integer parsers are written out (the C library of newer Linux systems would tie the programs to its
    /// version); ToDouble calls the C runtime (wcstod or strtod). Header only, like Diagnostics. Preliminary.</para>
    /// </summary>
    class Text
    {
    public:
        /// <summary>The number of characters before the terminating zero.</summary>
        static std::size_t Length(const Char* text) noexcept
        {
            std::size_t length = 0;
            while (text[length] != 0)
                ++length;
            return length;
        }

        /// <summary>Like wcscmp: below, at or above zero if a sorts before, equal to or after b (by character value).</summary>
        static std::int32_t Compare(const Char* a, const Char* b) noexcept
        {
            while (*a != 0 && *a == *b) {
                ++a;
                ++b;
            }
            return Unsigned(*a) < Unsigned(*b) ? -1 : (Unsigned(*a) > Unsigned(*b) ? 1 : 0);
        }

        /// <summary>True if both texts are the same.</summary>
        static bool Equal(const Char* a, const Char* b) noexcept { return Compare(a, b) == 0; }

        /// <summary>True if both texts are the same, the ASCII letters A to Z compared without case.</summary>
        static bool EqualIgnoreCase(const Char* a, const Char* b) noexcept
        {
            while (*a != 0 && Lower(*a) == Lower(*b)) {
                ++a;
                ++b;
            }
            return Lower(*a) == Lower(*b);
        }

        /// <summary>The last occurrence of c in text, or nullptr.</summary>
        static const Char* FindLast(const Char* text, Char c) noexcept
        {
            const Char* last = nullptr;
            for (; *text != 0; ++text)
                if (*text == c)
                    last = text;
            return last;
        }

        /// <summary>FindLast in a writable text (like the C++ overloads of wcsrchr).</summary>
        static Char* FindLast(Char* text, Char c) noexcept { return const_cast<Char*>(FindLast(static_cast<const Char*>(text), c)); }

        /// <summary>The first occurrence of c in text, or nullptr.</summary>
        static const Char* Find(const Char* text, Char c) noexcept
        {
            for (; *text != 0; ++text)
                if (*text == c)
                    return text;
            return nullptr;
        }

        /// <summary>Copies text into buffer (size characters including the zero), cut off if it does not fit; returns the length copied.</summary>
        static std::size_t Copy(Char* buffer, std::size_t size, const Char* text) noexcept
        {
            if (size == 0)
                return 0;
            std::size_t i = 0;
            for (; i + 1 < size && text[i] != 0; ++i)
                buffer[i] = text[i];
            buffer[i] = 0;
            return i;
        }

        /// <summary>
        /// Like wcstol with base 10 and a 32-bit long: leading white space, an optional sign, decimal digits. end points
        /// behind the number (to text if there is none); a value out of range gives the nearest limit.
        /// </summary>
        static std::int32_t ToInt32(const Char* text, const Char** end = nullptr) noexcept
        {
            bool                negative  = false;
            bool                overflow  = false;
            const std::uint64_t magnitude = Parse(text, end, negative, overflow, std::uint64_t{INT32_MAX} + 1);
            if (negative)
                return overflow || magnitude > std::uint64_t{INT32_MAX} ? INT32_MIN : -static_cast<std::int32_t>(magnitude);
            return overflow || magnitude > std::uint64_t{INT32_MAX} ? INT32_MAX : static_cast<std::int32_t>(magnitude);
        }

        /// <summary>Like wcstoul with base 10 and a 32-bit unsigned long: out of range gives UINT32_MAX, a minus sign negates (modulo 2^32).</summary>
        static std::uint32_t ToUInt32(const Char* text, const Char** end = nullptr) noexcept
        {
            bool                negative  = false;
            bool                overflow  = false;
            const std::uint64_t magnitude = Parse(text, end, negative, overflow, UINT32_MAX);
            if (overflow || magnitude > UINT32_MAX)
                return UINT32_MAX;
            return negative ? 0u - static_cast<std::uint32_t>(magnitude) : static_cast<std::uint32_t>(magnitude);
        }

        /// <summary>Like wcstoull with base 10: out of range gives UINT64_MAX, a minus sign negates (modulo 2^64).</summary>
        static std::uint64_t ToUInt64(const Char* text, const Char** end = nullptr) noexcept
        {
            bool                negative  = false;
            bool                overflow  = false;
            const std::uint64_t magnitude = Parse(text, end, negative, overflow, UINT64_MAX);
            if (overflow)
                return UINT64_MAX;
            return negative ? 0ull - magnitude : magnitude;
        }

        /// <summary>Like wcstod (Windows) or strtod (Linux): the C runtime parses the number.</summary>
        static double ToDouble(const Char* text, const Char** end = nullptr) noexcept
        {
            Char* stop = nullptr;
#if defined(_WIN32)
            const double value = std::wcstod(text, &stop);
#else
            const double value = std::strtod(text, &stop);
#endif
            if (end != nullptr)
                *end = stop;
            return value;
        }

    private:
        static std::uint32_t Unsigned(Char c) noexcept
        {
            if constexpr (sizeof(Char) == 1)
                return static_cast<unsigned char>(c);
            else
                return static_cast<std::uint32_t>(c);
        }

        static Char Lower(Char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<Char>(c - 'A' + 'a') : c; }

        static bool IsSpace(Char c) noexcept { return c == ' ' || (c >= '\t' && c <= '\r'); }

        // Decimal digits after white space and a sign; the magnitude up to limit (overflow beyond it).
        static std::uint64_t Parse(const Char* text, const Char** end, bool& negative, bool& overflow, std::uint64_t limit) noexcept
        {
            const Char* p = text;
            while (IsSpace(*p))
                ++p;
            if (*p == '+' || *p == '-')
                negative = *p++ == '-';
            if (*p < '0' || *p > '9') {
                if (end != nullptr)
                    *end = text; // no number: nothing consumed
                negative = false;
                return 0;
            }
            std::uint64_t value = 0;
            for (; *p >= '0' && *p <= '9'; ++p) {
                const auto digit = static_cast<std::uint64_t>(*p - '0');
                if (!overflow && (value > (limit - digit) / 10))
                    overflow = true;
                if (!overflow)
                    value = value * 10 + digit;
            }
            if (end != nullptr)
                *end = p;
            return value;
        }
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
