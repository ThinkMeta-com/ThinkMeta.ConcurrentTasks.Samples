// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstdarg>
#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    /// <summary>
    /// printf-style formatting into a buffer, without the C runtime, with the same result as the UCRT for the
    /// conversions of the samples: %d %i %u %x %X (length hh, h, l, ll, z), %c, %s, %f %F %e %E %g %G and %%, with the
    /// flags - + space 0, a width and a precision (also as *). Narrow text is UTF-8. %s is a string of the type of the
    /// format (as in the UCRT), %hs always narrow and %ls always wide; %c and %lc alike.
    /// <para>Not supported (written as they are): %a, %n, %o, %p, the flag # and long double. A precision above 100 digits after
    /// the point counts as 100.</para>
    /// <para>Preliminary: the conversions follow what the samples print and may grow.</para>
    /// </summary>
    class Formatter
    {
    public:
        /// <summary>
        /// Formats into buffer, which holds size characters including the terminating zero. Like snprintf: returns the
        /// length of the whole result without the zero, also when it did not fit; the buffer always ends with a zero
        /// (size above 0), a longer result is cut off.
        /// </summary>
        static std::int32_t Format(char* buffer, std::int32_t size, const char* format, ...) noexcept;

        /// <summary>Format with wide text.</summary>
        static std::int32_t Format(wchar_t* buffer, std::int32_t size, const wchar_t* format, ...) noexcept;

        /// <summary>Format with the arguments as a va_list.</summary>
        static std::int32_t FormatV(char* buffer, std::int32_t size, const char* format, std::va_list args) noexcept;

        /// <summary>Format with wide text and the arguments as a va_list.</summary>
        static std::int32_t FormatV(wchar_t* buffer, std::int32_t size, const wchar_t* format, std::va_list args) noexcept;

    private:
        struct Spec;
        class Decimal;
        template <typename Char>
        class Output;

        template <typename Char>
        static std::int32_t Run(Char* buffer, std::int32_t size, const Char* format, std::va_list args) noexcept;
        template <typename Char>
        static bool ParseSpec(const Char*& format, Spec& spec, std::va_list& args) noexcept;
        template <typename Char>
        static void Pad(Output<Char>& out, const Spec& spec, const char* sign, const char* body, std::int32_t bodyLength, bool zeroPadding) noexcept;
        template <typename Char>
        static void Integer(Output<Char>& out, const Spec& spec, std::va_list& args) noexcept;
        template <typename Char>
        static void Floating(Output<Char>& out, const Spec& spec, double value) noexcept;
        template <typename Char, typename Source>
        static void String(Output<Char>& out, const Spec& spec, const Source* text) noexcept;

        static std::int32_t FormatFixed(const Decimal& decimal, std::int32_t precision, char* body) noexcept;
        static std::int32_t FormatExponent(const Decimal& decimal, std::int32_t precision, bool upper, char* body) noexcept;
        static std::int32_t FormatGeneral(const Decimal& decimal, std::int32_t precision, bool upper, char* body) noexcept;

        static char32_t     NextCodePoint(const char*& text) noexcept;
        static char32_t     NextCodePoint(const wchar_t*& text) noexcept;
        static std::int32_t Encode(char32_t codePoint, char* out) noexcept;
        static std::int32_t Encode(char32_t codePoint, wchar_t* out) noexcept;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
