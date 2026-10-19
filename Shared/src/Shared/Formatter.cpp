// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "Formatter.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// No function of the C runtime: the floating-point conversions work on the exact decimal expansion of the double, so
// they round like the UCRT (to the nearest, a tie to even) without strtod/printf internals.

namespace ThinkMeta::ConcurrentTasks::Samples::Shared {

    // One conversion specification: %[flags][width][.precision][length]conversion.
    struct Formatter::Spec {
        bool         left       = false; // -
        bool         plus       = false; // +
        bool         space      = false; // space
        bool         zero       = false; // 0
        std::int32_t width      = 0;
        std::int32_t precision  = -1; // -1: none
        char         length     = 0;  // 'H' hh, 'h', 'l', 'L' ll, 'z', 0 none
        char         conversion = 0;
    };

    // The exact decimal expansion of a finite, non-negative double: digits d1 d2 ... dn (d1 and dn not 0) with the value
    // 0.d1d2...dn * 10^intDigits. A double has at most 767 significant decimal digits.
    class Formatter::Decimal
    {
    public:
        static constexpr std::int32_t MaxDigits    = 800;
        static constexpr std::int32_t MaxPrecision = 100; // digits after the point at most, so the buffers stay small
        static constexpr std::int32_t MaxIntDigits = 309; // digits before the point of the largest double
        static constexpr std::int32_t RoundSize    = MaxIntDigits + MaxPrecision + 2;
        static constexpr std::int32_t BodySize     = MaxIntDigits + MaxPrecision + 16;

        explicit Decimal(double value) noexcept
        {
            const auto    bits     = std::bit_cast<std::uint64_t>(value);
            const auto    biased   = static_cast<std::int32_t>((bits >> 52) & 0x7FF);
            std::uint64_t mantissa = bits & ((std::uint64_t{1} << 52) - 1);
            std::int32_t  exponent = -1074;
            if (biased != 0) {
                mantissa |= std::uint64_t{1} << 52;
                exponent = biased - 1075;
            }
            if (mantissa == 0)
                return; // zero
            while ((mantissa & 1) == 0 && exponent < 0) {
                mantissa >>= 1;
                ++exponent;
            }

            // value = mantissa * 2^exponent; with exponent < 0 that is mantissa * 5^-exponent / 10^-exponent
            limbs_[0]                   = static_cast<std::uint32_t>(mantissa % LimbBase);
            limbs_[1]                   = static_cast<std::uint32_t>(mantissa / LimbBase % LimbBase);
            limbs_[2]                   = static_cast<std::uint32_t>(mantissa / LimbBase / LimbBase);
            limbCount_                  = limbs_[2] != 0 ? 3 : limbs_[1] != 0 ? 2 : 1;
            std::int32_t fractionDigits = 0;
            if (exponent > 0) {
                for (; exponent >= 29; exponent -= 29)
                    Multiply(std::uint32_t{1} << 29);
                Multiply(std::uint32_t{1} << exponent);
            } else {
                fractionDigits     = -exponent;
                std::int32_t fives = fractionDigits;
                for (; fives >= 13; fives -= 13)
                    Multiply(1220703125u); // 5^13
                std::uint32_t rest = 1;
                for (; fives > 0; --fives)
                    rest *= 5;
                Multiply(rest);
            }

            // the limbs as decimal digits, most significant first
            std::int32_t count = 0;
            for (std::int32_t i = limbCount_ - 1; i >= 0; --i) {
                char          group[9];
                std::uint32_t limb = limbs_[i];
                for (std::int32_t k = 8; k >= 0; --k) {
                    group[k] = static_cast<char>(limb % 10);
                    limb /= 10;
                }
                std::int32_t k = 0;
                if (i == limbCount_ - 1)
                    while (k < 8 && group[k] == 0)
                        ++k;
                for (; k < 9; ++k)
                    digits_[count++] = static_cast<std::uint8_t>(group[k]);
            }
            intDigits_ = count - fractionDigits;
            while (count > 0 && digits_[count - 1] == 0)
                --count;
            count_ = count;
        }

        bool         IsZero() const noexcept { return count_ == 0; }
        std::int32_t IntDigits() const noexcept { return intDigits_; }

        /// <summary>
        /// The first keep digits (keep 0 or more), rounded to the nearest, a tie to even, as keep + 1 digits into out:
        /// out[0] is the carry digit (0 or 1). True if the rounding carried into out[0].
        /// </summary>
        bool Round(std::int32_t keep, std::uint8_t* out) const noexcept
        {
            out[0] = 0;
            for (std::int32_t i = 0; i < keep; ++i)
                out[i + 1] = i < count_ ? digits_[i] : 0;
            bool up = false;
            if (keep < count_) {
                const std::uint8_t next = digits_[keep];
                // the last digit is not 0: anything behind the next digit makes it more than a tie
                up = next > 5 || (next == 5 && (keep + 1 < count_ || (keep > 0 && (digits_[keep - 1] & 1) != 0)));
            }
            if (up) {
                std::int32_t i = keep;
                while (out[i] == 9)
                    out[i--] = 0;
                ++out[i];
            }
            return out[0] != 0;
        }

    private:
        static constexpr std::uint32_t LimbBase = 1000000000u;

        void Multiply(std::uint32_t factor) noexcept
        {
            std::uint64_t carry = 0;
            for (std::int32_t i = 0; i < limbCount_; ++i) {
                const std::uint64_t product = static_cast<std::uint64_t>(limbs_[i]) * factor + carry;
                limbs_[i]                   = static_cast<std::uint32_t>(product % LimbBase);
                carry                       = product / LimbBase;
            }
            while (carry != 0) {
                limbs_[limbCount_++] = static_cast<std::uint32_t>(carry % LimbBase);
                carry /= LimbBase;
            }
        }

        std::uint32_t limbs_[100]; // base 10^9, least significant first
        std::int32_t  limbCount_ = 0;
        std::uint8_t  digits_[MaxDigits];
        std::int32_t  count_     = 0;
        std::int32_t  intDigits_ = 0;
    };

    // The output buffer: counts every character, stores what fits and keeps room for the terminating zero.
    template <typename Char>
    class Formatter::Output
    {
    public:
        Output(Char* buffer, std::int32_t size) noexcept : buffer_(buffer), size_(size) {}

        void Put(Char c) noexcept
        {
            if (count_ < size_ - 1)
                buffer_[count_] = c;
            ++count_;
        }

        void Repeat(Char c, std::int32_t n) noexcept
        {
            for (std::int32_t i = 0; i < n; ++i)
                Put(c);
        }

        std::int32_t Finish() noexcept
        {
            if (size_ > 0)
                buffer_[count_ < size_ - 1 ? count_ : size_ - 1] = Char{};
            return count_;
        }

    private:
        Char*        buffer_;
        std::int32_t size_;
        std::int32_t count_ = 0;
    };

    std::int32_t Formatter::Format(char* buffer, std::int32_t size, const char* format, ...) noexcept
    {
        std::va_list args;
        va_start(args, format);
        const std::int32_t length = Run(buffer, size, format, args);
        va_end(args);
        return length;
    }

    std::int32_t Formatter::Format(wchar_t* buffer, std::int32_t size, const wchar_t* format, ...) noexcept
    {
        std::va_list args;
        va_start(args, format);
        const std::int32_t length = Run(buffer, size, format, args);
        va_end(args);
        return length;
    }

    std::int32_t Formatter::FormatV(char* buffer, std::int32_t size, const char* format, std::va_list args) noexcept
    {
        return Run(buffer, size, format, args);
    }

    std::int32_t Formatter::FormatV(wchar_t* buffer, std::int32_t size, const wchar_t* format, std::va_list args) noexcept
    {
        return Run(buffer, size, format, args);
    }

    template <typename Char>
    std::int32_t Formatter::Run(Char* buffer, std::int32_t size, const Char* format, std::va_list args) noexcept
    {
        Output<Char> out(buffer, size);
        while (*format != 0) {
            if (*format != '%') {
                out.Put(*format++);
                continue;
            }
            const Char* start = format++;
            Spec        spec;
            if (!ParseSpec(format, spec, args)) {
                out.Put(*start); // not supported: the text as it is
                format = start + 1;
                continue;
            }
            switch (spec.conversion) {
                case '%': out.Put('%'); break;
                case 'd':
                case 'i':
                case 'u':
                case 'x':
                case 'X': Integer(out, spec, args); break;
                case 'f':
                case 'F':
                case 'e':
                case 'E':
                case 'g':
                case 'G': Floating(out, spec, va_arg(args, double)); break;
                case 'c': {
                    // %c is a character of the type of the format, %hc narrow, %lc wide
                    const bool narrow = spec.length == 'h' || (spec.length != 'l' && std::is_same_v<Char, char>);
                    const int  value  = va_arg(args, int);
                    if (narrow) {
                        const char text[2] = {static_cast<char>(value), 0};
                        String(out, spec, text);
                    } else {
                        const wchar_t text[2] = {static_cast<wchar_t>(value), 0};
                        String(out, spec, text);
                    }
                    break;
                }
                case 's':
                    // %s is a string of the type of the format, %hs narrow, %ls wide
                    if (spec.length == 'h')
                        String(out, spec, va_arg(args, const char*));
                    else if (spec.length == 'l')
                        String(out, spec, va_arg(args, const wchar_t*));
                    else
                        String(out, spec, va_arg(args, const Char*));
                    break;
                default: break;
            }
        }
        return out.Finish();
    }

    template <typename Char>
    bool Formatter::ParseSpec(const Char*& format, Spec& spec, std::va_list& args) noexcept
    {
        for (;; ++format) {
            if (*format == '-')
                spec.left = true;
            else if (*format == '+')
                spec.plus = true;
            else if (*format == ' ')
                spec.space = true;
            else if (*format == '0')
                spec.zero = true;
            else
                break;
        }
        if (*format == '*') {
            spec.width = va_arg(args, int);
            if (spec.width < 0) {
                spec.left  = true;
                spec.width = -spec.width;
            }
            ++format;
        } else {
            while (*format >= '0' && *format <= '9')
                spec.width = spec.width * 10 + (*format++ - '0');
        }
        if (*format == '.') {
            ++format;
            spec.precision = 0;
            if (*format == '*') {
                spec.precision = va_arg(args, int);
                if (spec.precision < 0)
                    spec.precision = -1;
                ++format;
            } else {
                while (*format >= '0' && *format <= '9')
                    spec.precision = spec.precision * 10 + (*format++ - '0');
            }
        }
        if (*format == 'h') {
            spec.length = format[1] == 'h' ? 'H' : 'h';
            format += spec.length == 'H' ? 2 : 1;
        } else if (*format == 'l') {
            spec.length = format[1] == 'l' ? 'L' : 'l';
            format += spec.length == 'L' ? 2 : 1;
        } else if (*format == 'z') {
            spec.length = 'z';
            ++format;
        }
        switch (*format) {
            case '%':
            case 'd':
            case 'i':
            case 'u':
            case 'x':
            case 'X':
            case 'c':
            case 's':
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G': spec.conversion = static_cast<char>(*format++); return true;
            default: return false;
        }
    }

    template <typename Char>
    void Formatter::Pad(Output<Char>& out, const Spec& spec, const char* sign, const char* body, std::int32_t bodyLength, bool zeroPadding) noexcept
    {
        std::int32_t signLength = 0;
        while (sign[signLength] != 0)
            ++signLength;
        const std::int32_t fill = spec.width > signLength + bodyLength ? spec.width - signLength - bodyLength : 0;
        const auto         put  = [&out](const char* text, std::int32_t length) {
            for (std::int32_t i = 0; i < length; ++i)
                out.Put(static_cast<Char>(text[i]));
        };
        if (spec.left) {
            put(sign, signLength);
            put(body, bodyLength);
            out.Repeat(' ', fill);
        } else if (zeroPadding && spec.zero) {
            put(sign, signLength);
            out.Repeat('0', fill);
            put(body, bodyLength);
        } else {
            out.Repeat(' ', fill);
            put(sign, signLength);
            put(body, bodyLength);
        }
    }

    template <typename Char>
    void Formatter::Integer(Output<Char>& out, const Spec& spec, std::va_list& args) noexcept
    {
        const bool    isSigned  = spec.conversion == 'd' || spec.conversion == 'i';
        bool          negative  = false;
        std::uint64_t magnitude = 0;
        if (isSigned) {
            std::int64_t value = 0;
            switch (spec.length) {
                case 'H': value = static_cast<signed char>(va_arg(args, int)); break;
                case 'h': value = static_cast<short>(va_arg(args, int)); break;
                case 'l': value = va_arg(args, long); break;
                case 'L': value = va_arg(args, long long); break;
                case 'z': value = va_arg(args, std::ptrdiff_t); break;
                default: value = va_arg(args, int); break;
            }
            negative  = value < 0;
            magnitude = negative ? 0 - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
        } else {
            switch (spec.length) {
                case 'H': magnitude = static_cast<unsigned char>(va_arg(args, int)); break;
                case 'h': magnitude = static_cast<unsigned short>(va_arg(args, int)); break;
                case 'l': magnitude = va_arg(args, unsigned long); break;
                case 'L': magnitude = va_arg(args, unsigned long long); break;
                case 'z': magnitude = va_arg(args, std::size_t); break;
                default: magnitude = va_arg(args, unsigned int); break;
            }
        }

        const std::uint32_t base   = spec.conversion == 'x' || spec.conversion == 'X' ? 16 : 10;
        const char*         symbol = spec.conversion == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
        char                reversed[24];
        std::int32_t        digits = 0;
        for (; magnitude != 0; magnitude /= base)
            reversed[digits++] = symbol[magnitude % base];
        const std::int32_t minimum = spec.precision >= 0 ? spec.precision : 1;
        char               body[96];
        std::int32_t       length = 0;
        for (std::int32_t i = digits; i < minimum && length < 64; ++i)
            body[length++] = '0';
        while (digits > 0)
            body[length++] = reversed[--digits];

        const char* sign = negative ? "-" : isSigned && spec.plus ? "+" : isSigned && spec.space ? " " : "";
        Pad(out, spec, sign, body, length, spec.precision < 0);
    }

    template <typename Char>
    void Formatter::Floating(Output<Char>& out, const Spec& spec, double value) noexcept
    {
        const bool         upper     = spec.conversion == 'F' || spec.conversion == 'E' || spec.conversion == 'G';
        const std::int32_t precision = spec.precision < 0 ? 6 : spec.precision < Decimal::MaxPrecision ? spec.precision : Decimal::MaxPrecision;
        const auto         bits      = std::bit_cast<std::uint64_t>(value);
        const bool         negative  = (bits >> 63) != 0;
        const bool         special   = ((bits >> 52) & 0x7FF) == 0x7FF;
        const char*        sign      = negative ? "-" : spec.plus ? "+" : spec.space ? " " : "";

        if (special) {
            const bool   isNan  = (bits & ((std::uint64_t{1} << 52) - 1)) != 0;
            const char*  text   = !isNan ? (upper ? "INF" : "inf") : negative ? (upper ? "NAN(IND)" : "nan(ind)") : (upper ? "NAN" : "nan");
            std::int32_t length = 0;
            while (text[length] != 0)
                ++length;
            Pad(out, spec, sign, text, length, false);
            return;
        }

        const Decimal decimal(negative ? -value : value);
        char          body[Decimal::BodySize];
        std::int32_t  length = 0;
        switch (spec.conversion) {
            case 'f':
            case 'F': length = FormatFixed(decimal, precision, body); break;
            case 'e':
            case 'E': length = FormatExponent(decimal, precision, upper, body); break;
            default: length = FormatGeneral(decimal, precision, upper, body); break;
        }
        Pad(out, spec, sign, body, length, true);
    }

    template <typename Char, typename Source>
    void Formatter::String(Output<Char>& out, const Spec& spec, const Source* text) noexcept
    {
        if (text == nullptr) {
            String(out, spec, "(null)");
            return;
        }
        // the length in characters of the output, at most the precision; a converted code point is not cut
        std::int32_t length = 0;
        if constexpr (std::is_same_v<Char, Source>) {
            while (text[length] != 0 && (spec.precision < 0 || length < spec.precision))
                ++length;
        } else {
            for (const Source* p = text; *p != 0;) {
                Char               units[4];
                const std::int32_t n = Encode(NextCodePoint(p), units);
                if (spec.precision >= 0 && length + n > spec.precision)
                    break;
                length += n;
            }
        }

        const std::int32_t fill = spec.width > length ? spec.width - length : 0;
        if (!spec.left)
            out.Repeat(' ', fill);
        if constexpr (std::is_same_v<Char, Source>) {
            for (std::int32_t i = 0; i < length; ++i)
                out.Put(text[i]);
        } else {
            std::int32_t written = 0;
            for (const Source* p = text; *p != 0 && written < length;) {
                Char               units[4];
                const std::int32_t n = Encode(NextCodePoint(p), units);
                for (std::int32_t i = 0; i < n; ++i)
                    out.Put(units[i]);
                written += n;
            }
        }
        if (spec.left)
            out.Repeat(' ', fill);
    }

    // %f: the value rounded to precision digits after the point.
    std::int32_t Formatter::FormatFixed(const Decimal& decimal, std::int32_t precision, char* body) noexcept
    {
        std::uint8_t       rounded[Decimal::RoundSize];
        const std::int32_t keep = decimal.IsZero() ? -1 : decimal.IntDigits() + precision;
        const std::int32_t kept = keep > 0 ? keep : 0;
        if (keep >= 0)
            decimal.Round(kept, rounded);
        else
            rounded[0] = 0;
        // rounded[0 .. kept] is an integer whose last precision digits are the fraction
        const std::int32_t total   = kept + 1;
        std::int32_t       length  = 0;
        std::int32_t       integer = total - precision; // digits of the integer part in rounded
        bool               leading = true;
        for (std::int32_t i = 0; i < integer; ++i) {
            if (leading && rounded[i] == 0 && i < integer - 1)
                continue;
            leading        = false;
            body[length++] = static_cast<char>('0' + rounded[i]);
        }
        if (integer <= 0)
            body[length++] = '0';
        if (precision > 0) {
            body[length++] = '.';
            for (std::int32_t j = 0; j < precision; ++j) {
                const std::int32_t i = integer + j;
                body[length++]       = static_cast<char>('0' + (i >= 0 ? rounded[i] : 0));
            }
        }
        return length;
    }

    // %e: one digit before the point, precision digits after it, the exponent with at least two digits.
    std::int32_t Formatter::FormatExponent(const Decimal& decimal, std::int32_t precision, bool upper, char* body) noexcept
    {
        std::uint8_t        rounded[Decimal::RoundSize];
        const std::int32_t  significant = precision + 1;
        std::int32_t        exponent    = 0;
        const std::uint8_t* digits      = rounded + 1;
        if (decimal.IsZero()) {
            for (std::int32_t i = 0; i <= significant; ++i)
                rounded[i] = 0;
        } else {
            exponent = decimal.IntDigits() - 1;
            if (decimal.Round(significant, rounded)) {
                digits = rounded; // 9.99 became 10.0: one more digit before the point
                ++exponent;
            }
        }
        std::int32_t length = 0;
        body[length++]      = static_cast<char>('0' + digits[0]);
        if (precision > 0) {
            body[length++] = '.';
            for (std::int32_t i = 1; i < significant; ++i)
                body[length++] = static_cast<char>('0' + digits[i]);
        }
        body[length++]         = upper ? 'E' : 'e';
        body[length++]         = exponent < 0 ? '-' : '+';
        std::int32_t magnitude = exponent < 0 ? -exponent : exponent;
        char         reversed[8];
        std::int32_t count = 0;
        do {
            reversed[count++] = static_cast<char>('0' + magnitude % 10);
            magnitude /= 10;
        } while (magnitude != 0);
        if (count < 2)
            reversed[count++] = '0';
        while (count > 0)
            body[length++] = reversed[--count];
        return length;
    }

    // %g: %e or %f by the exponent after rounding to precision significant digits, without trailing zeros.
    std::int32_t Formatter::FormatGeneral(const Decimal& decimal, std::int32_t precision, bool upper, char* body) noexcept
    {
        const std::int32_t significant = precision == 0 ? 1 : precision;
        std::int32_t       exponent    = 0;
        if (!decimal.IsZero()) {
            std::uint8_t rounded[Decimal::RoundSize];
            exponent = decimal.IntDigits() - 1 + (decimal.Round(significant, rounded) ? 1 : 0);
        }
        std::int32_t length = 0;
        if (exponent < significant && exponent >= -4)
            length = FormatFixed(decimal, significant - 1 - exponent, body);
        else
            length = FormatExponent(decimal, significant - 1, upper, body);

        // trailing zeros of the fraction and a point without digits go
        std::int32_t mantissaEnd = 0;
        while (mantissaEnd < length && body[mantissaEnd] != 'e' && body[mantissaEnd] != 'E')
            ++mantissaEnd;
        std::int32_t point = 0;
        while (point < mantissaEnd && body[point] != '.')
            ++point;
        if (point == mantissaEnd)
            return length;
        std::int32_t end = mantissaEnd;
        while (end > point + 1 && body[end - 1] == '0')
            --end;
        if (end == point + 1)
            end = point;
        const std::int32_t removed = mantissaEnd - end;
        for (std::int32_t i = mantissaEnd; i < length; ++i)
            body[i - removed] = body[i];
        return length - removed;
    }

    // UTF-8; an invalid byte is U+FFFD.
    char32_t Formatter::NextCodePoint(const char*& text) noexcept
    {
        const auto   lead = static_cast<std::uint8_t>(*text++);
        std::int32_t more = 0;
        char32_t     code = 0;
        if (lead < 0x80)
            return lead;
        if ((lead & 0xE0) == 0xC0) {
            more = 1;
            code = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            more = 2;
            code = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            more = 3;
            code = lead & 0x07;
        } else {
            return 0xFFFD;
        }
        for (std::int32_t i = 0; i < more; ++i) {
            const auto next = static_cast<std::uint8_t>(*text);
            if ((next & 0xC0) != 0x80)
                return 0xFFFD;
            code = code << 6 | (next & 0x3F);
            ++text;
        }
        return code;
    }

    // UTF-16; a lone surrogate stays as it is.
    char32_t Formatter::NextCodePoint(const wchar_t*& text) noexcept
    {
        const char32_t first = static_cast<char16_t>(*text++);
        if (first >= 0xD800 && first < 0xDC00 && *text >= 0xDC00 && *text < 0xE000) {
            const char32_t second = static_cast<char16_t>(*text++);
            return 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
        }
        return first;
    }

    std::int32_t Formatter::Encode(char32_t codePoint, char* out) noexcept
    {
        if (codePoint < 0x80) {
            out[0] = static_cast<char>(codePoint);
            return 1;
        }
        if (codePoint < 0x800) {
            out[0] = static_cast<char>(0xC0 | codePoint >> 6);
            out[1] = static_cast<char>(0x80 | (codePoint & 0x3F));
            return 2;
        }
        if (codePoint < 0x10000) {
            out[0] = static_cast<char>(0xE0 | codePoint >> 12);
            out[1] = static_cast<char>(0x80 | (codePoint >> 6 & 0x3F));
            out[2] = static_cast<char>(0x80 | (codePoint & 0x3F));
            return 3;
        }
        out[0] = static_cast<char>(0xF0 | codePoint >> 18);
        out[1] = static_cast<char>(0x80 | (codePoint >> 12 & 0x3F));
        out[2] = static_cast<char>(0x80 | (codePoint >> 6 & 0x3F));
        out[3] = static_cast<char>(0x80 | (codePoint & 0x3F));
        return 4;
    }

    std::int32_t Formatter::Encode(char32_t codePoint, wchar_t* out) noexcept
    {
        if (codePoint < 0x10000) {
            out[0] = static_cast<wchar_t>(codePoint);
            return 1;
        }
        codePoint -= 0x10000;
        out[0] = static_cast<wchar_t>(0xD800 + (codePoint >> 10));
        out[1] = static_cast<wchar_t>(0xDC00 + (codePoint & 0x3FF));
        return 2;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Shared
