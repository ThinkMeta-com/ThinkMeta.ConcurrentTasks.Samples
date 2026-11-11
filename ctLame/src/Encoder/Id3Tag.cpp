// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Id3Tag.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Memory.h"
#include "Version.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    namespace {

        using Shared::Heap;

        // NOTE: The spelling of these genre names is identical to those found in Winamp and mp3info.
        constexpr const char* GenreNames[] = {"Blues",
                                              "Classic Rock",
                                              "Country",
                                              "Dance",
                                              "Disco",
                                              "Funk",
                                              "Grunge",
                                              "Hip-Hop",
                                              "Jazz",
                                              "Metal",
                                              "New Age",
                                              "Oldies",
                                              "Other",
                                              "Pop",
                                              "R&B",
                                              "Rap",
                                              "Reggae",
                                              "Rock",
                                              "Techno",
                                              "Industrial",
                                              "Alternative",
                                              "Ska",
                                              "Death Metal",
                                              "Pranks",
                                              "Soundtrack",
                                              "Euro-Techno",
                                              "Ambient",
                                              "Trip-Hop",
                                              "Vocal",
                                              "Jazz+Funk",
                                              "Fusion",
                                              "Trance",
                                              "Classical",
                                              "Instrumental",
                                              "Acid",
                                              "House",
                                              "Game",
                                              "Sound Clip",
                                              "Gospel",
                                              "Noise",
                                              "Alternative Rock",
                                              "Bass",
                                              "Soul",
                                              "Punk",
                                              "Space",
                                              "Meditative",
                                              "Instrumental Pop",
                                              "Instrumental Rock",
                                              "Ethnic",
                                              "Gothic",
                                              "Darkwave",
                                              "Techno-Industrial",
                                              "Electronic",
                                              "Pop-Folk",
                                              "Eurodance",
                                              "Dream",
                                              "Southern Rock",
                                              "Comedy",
                                              "Cult",
                                              "Gangsta",
                                              "Top 40",
                                              "Christian Rap",
                                              "Pop/Funk",
                                              "Jungle",
                                              "Native US",
                                              "Cabaret",
                                              "New Wave",
                                              "Psychedelic",
                                              "Rave",
                                              "Showtunes",
                                              "Trailer",
                                              "Lo-Fi",
                                              "Tribal",
                                              "Acid Punk",
                                              "Acid Jazz",
                                              "Polka",
                                              "Retro",
                                              "Musical",
                                              "Rock & Roll",
                                              "Hard Rock",
                                              "Folk",
                                              "Folk-Rock",
                                              "National Folk",
                                              "Swing",
                                              "Fast Fusion",
                                              "Bebob",
                                              "Latin",
                                              "Revival",
                                              "Celtic",
                                              "Bluegrass",
                                              "Avantgarde",
                                              "Gothic Rock",
                                              "Progressive Rock",
                                              "Psychedelic Rock",
                                              "Symphonic Rock",
                                              "Slow Rock",
                                              "Big Band",
                                              "Chorus",
                                              "Easy Listening",
                                              "Acoustic",
                                              "Humour",
                                              "Speech",
                                              "Chanson",
                                              "Opera",
                                              "Chamber Music",
                                              "Sonata",
                                              "Symphony",
                                              "Booty Bass",
                                              "Primus",
                                              "Porn Groove",
                                              "Satire",
                                              "Slow Jam",
                                              "Club",
                                              "Tango",
                                              "Samba",
                                              "Folklore",
                                              "Ballad",
                                              "Power Ballad",
                                              "Rhythmic Soul",
                                              "Freestyle",
                                              "Duet",
                                              "Punk Rock",
                                              "Drum Solo",
                                              "A Cappella",
                                              "Euro-House",
                                              "Dance Hall",
                                              "Goa",
                                              "Drum & Bass",
                                              "Club-House",
                                              "Hardcore",
                                              "Terror",
                                              "Indie",
                                              "BritPop",
                                              "Negerpunk",
                                              "Polsk Punk",
                                              "Beat",
                                              "Christian Gangsta",
                                              "Heavy Metal",
                                              "Black Metal",
                                              "Crossover",
                                              "Contemporary Christian",
                                              "Christian Rock",
                                              "Merengue",
                                              "Salsa",
                                              "Thrash Metal",
                                              "Anime",
                                              "JPop",
                                              "SynthPop"};

        constexpr std::int32_t GenreNameCount = static_cast<std::int32_t>(sizeof(GenreNames) / sizeof(GenreNames[0]));

        // the genre numbers in alphabetical order of their names
        constexpr std::uint8_t GenreAlphaMap[] = {
            123, 34,  74, 73,  99,  20,  40,  26,  145, 90, 116, 41,  135, 85,  96,  138, 89,  0,   107, 132, 65,  88,  104, 102, 97,  136, 61,  141, 32, 1,
            112, 128, 57, 140, 2,   139, 58,  3,   125, 50, 22,  4,   55,  127, 122, 120, 98,  52,  48,  54,  124, 25,  84,  80,  115, 81,  119, 5,   30, 36,
            59,  126, 38, 49,  91,  6,   129, 79,  137, 7,  35,  100, 131, 19,  33,  46,  47,  8,   29,  146, 63,  86,  71,  45,  142, 9,   77,  82,  64, 133,
            10,  66,  39, 11,  103, 12,  75,  134, 13,  53, 62,  109, 117, 23,  108, 92,  67,  93,  43,  121, 15,  68,  14,  16,  76,  87,  118, 17,  78, 143,
            114, 110, 69, 21,  111, 95,  105, 42,  37,  24, 56,  44,  101, 83,  94,  106, 147, 113, 18,  51,  130, 144, 60,  70,  31,  72,  27,  28};

        constexpr std::int32_t GenreAlphaCount = static_cast<std::int32_t>(sizeof(GenreAlphaMap));
        constexpr std::int32_t GenreIndexOther = 12;
        constexpr std::int32_t GenreNumUnknown = 255;

        static_assert(GenreNameCount == 148 && GenreAlphaCount == 148);

        // id3tag.h: tag_spec.flags
        constexpr std::uint32_t CHANGED_FLAG = 1u << 0;
        constexpr std::uint32_t ADD_V2_FLAG  = 1u << 1;
        constexpr std::uint32_t V1_ONLY_FLAG = 1u << 2;
        constexpr std::uint32_t V2_ONLY_FLAG = 1u << 3;
        constexpr std::uint32_t PAD_V2_FLAG  = 1u << 5;

        // id3tag.h: albumart_mimetype
        constexpr std::int32_t MIMETYPE_NONE = 0;
        constexpr std::int32_t MIMETYPE_JPEG = 1;
        constexpr std::int32_t MIMETYPE_PNG  = 2;
        constexpr std::int32_t MIMETYPE_GIF  = 3;

        constexpr std::uint32_t FrameId(char a, char b, char c, char d) noexcept
        {
            return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) << 24) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 16)
                | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 8) | static_cast<std::uint32_t>(static_cast<std::uint8_t>(d));
        }

        constexpr std::uint32_t ID_TITLE      = FrameId('T', 'I', 'T', '2');
        constexpr std::uint32_t ID_ARTIST     = FrameId('T', 'P', 'E', '1');
        constexpr std::uint32_t ID_ALBUM      = FrameId('T', 'A', 'L', 'B');
        constexpr std::uint32_t ID_GENRE      = FrameId('T', 'C', 'O', 'N');
        constexpr std::uint32_t ID_ENCODER    = FrameId('T', 'S', 'S', 'E');
        constexpr std::uint32_t ID_PLAYLENGTH = FrameId('T', 'L', 'E', 'N');
        constexpr std::uint32_t ID_COMMENT    = FrameId('C', 'O', 'M', 'M');
        constexpr std::uint32_t ID_TRACK      = FrameId('T', 'R', 'C', 'K');
        constexpr std::uint32_t ID_YEAR       = FrameId('T', 'Y', 'E', 'R');
        constexpr std::uint32_t ID_TXXX       = FrameId('T', 'X', 'X', 'X');
        constexpr std::uint32_t ID_WXXX       = FrameId('W', 'X', 'X', 'X');
        constexpr std::uint32_t ID_SYLT       = FrameId('S', 'Y', 'L', 'T');
        constexpr std::uint32_t ID_APIC       = FrameId('A', 'P', 'I', 'C');
        constexpr std::uint32_t ID_GEOB       = FrameId('G', 'E', 'O', 'B');
        constexpr std::uint32_t ID_PCNT       = FrameId('P', 'C', 'N', 'T');
        constexpr std::uint32_t ID_AENC       = FrameId('A', 'E', 'N', 'C');
        constexpr std::uint32_t ID_LINK       = FrameId('L', 'I', 'N', 'K');
        constexpr std::uint32_t ID_ENCR       = FrameId('E', 'N', 'C', 'R');
        constexpr std::uint32_t ID_GRID       = FrameId('G', 'R', 'I', 'D');
        constexpr std::uint32_t ID_PRIV       = FrameId('P', 'R', 'I', 'V');
        constexpr std::uint32_t ID_USER       = FrameId('U', 'S', 'E', 'R');
        constexpr std::uint32_t ID_PCST       = FrameId('P', 'C', 'S', 'T'); // iTunes Podcast indicator, only presence important
        constexpr std::uint32_t ID_WFED       = FrameId('W', 'F', 'E', 'D'); // iTunes Podcast URL as TEXT FRAME !!! violates standard

        // LAME: frame_id_matches() and isFrameIdMatching(); a byte of the id that is 0 counts as a match
        bool IsFrameIdMatching(std::uint32_t id, std::uint32_t mask) noexcept
        {
            std::uint32_t result = 0;
            std::uint32_t window = 0xff;
            for (std::int32_t i = 0; i < 4; ++i, window <<= 8) {
                const std::uint32_t mw = mask & window;
                const std::uint32_t iw = id & window;
                if (mw != 0 && mw != iw)
                    result |= iw;
            }
            return result == 0;
        }

        // LAME: isMultiFrame(); frames that may occur more than once (told apart by language and description)
        bool IsMultiFrame(std::uint32_t frameId) noexcept
        {
            switch (frameId) {
                case ID_TXXX:
                case ID_WXXX:
                case ID_COMMENT:
                case ID_SYLT:
                case ID_APIC:
                case ID_GEOB:
                case ID_PCNT:
                case ID_AENC:
                case ID_LINK:
                case ID_ENCR:
                case ID_GRID:
                case ID_PRIV: return true;
                default: return false;
            }
        }

        // tolower/toupper of the "C" locale
        char ToLower(char c) noexcept
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        char ToUpper(char c) noexcept
        {
            return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        }

        bool IsSpace(char c) noexcept
        {
            return c == ' ' || (c >= '\t' && c <= '\r');
        }

        // strtol(text, &end, 10), clamped to the int range
        std::int32_t StrToInt(const char* text, const char** end) noexcept
        {
            const char* p = text;
            while (IsSpace(*p))
                ++p;
            const bool negative = *p == '-';
            if (*p == '-' || *p == '+')
                ++p;
            if (*p < '0' || *p > '9') {
                *end = text;
                return 0;
            }
            std::int64_t value = 0;
            while (*p >= '0' && *p <= '9') {
                if (value <= 0x7fffffffll)
                    value = value * 10 + (*p - '0');
                ++p;
            }
            *end = p;
            if (negative)
                value = -value;
            if (value > 0x7fffffffll)
                return 0x7fffffff;
            if (value < -0x80000000ll)
                return static_cast<std::int32_t>(-0x7fffffff - 1);
            return static_cast<std::int32_t>(value);
        }

        // atoi(text)
        std::int32_t AToI(const char* text) noexcept
        {
            const char* end = nullptr;
            return StrToInt(text, &end);
        }

        // sprintf(buffer, "%u", value)
        void UnsignedToText(char (&buffer)[16], std::uint32_t value) noexcept
        {
            char         digits[16];
            std::int32_t n = 0;
            do {
                digits[n++] = static_cast<char>('0' + value % 10);
                value /= 10;
            } while (value != 0);
            std::int32_t i = 0;
            while (n > 0)
                buffer[i++] = digits[--n];
            buffer[i] = 0;
        }

        std::size_t Length(const char* s) noexcept
        {
            return s != nullptr ? std::strlen(s) : 0;
        }

        // LAME: local_ucs2_strlen()
        std::size_t Length(const char16_t* s) noexcept
        {
            std::size_t n = 0;
            if (s != nullptr) {
                while (*s++ != 0)
                    ++n;
            }
            return n;
        }

        bool HasUcs2ByteOrderMarker(char16_t bom) noexcept
        {
            return bom == 0xFFFEu || bom == 0xFEFFu;
        }

        char16_t SwapBytes(char16_t w) noexcept
        {
            return static_cast<char16_t>((0xff00u & (w << 8)) | (0x00ffu & (w >> 8)));
        }

        char16_t ToLittleEndian(char16_t bom, char16_t c) noexcept
        {
            return bom == 0xFFFEu ? SwapBytes(c) : c;
        }

        char16_t FromLatin1Char(const char16_t* s, char16_t c) noexcept
        {
            return s[0] == 0xFFFEu ? SwapBytes(c) : c;
        }

        // LAME: local_ucs2_pos() and local_char_pos()
        template <typename C>
        std::int32_t Position(const C* str, C c) noexcept
        {
            for (std::int32_t i = 0; str != nullptr && str[i] != 0; ++i) {
                if (str[i] == c)
                    return i;
            }
            return -1;
        }

        // LAME: maybeLatin1(); note that 0xff does not count
        bool MaybeLatin1(const char16_t* text) noexcept
        {
            if (text != nullptr) {
                const char16_t bom = *text++;
                while (*text != 0) {
                    if (ToLittleEndian(bom, *text++) > 0x00fe)
                        return false;
                }
            }
            return true;
        }

        // A string on the heap that frees itself; nullptr if empty (local_strdup and local_ucs2_strdup).
        template <typename C>
        struct HeapString {
            C*          ptr = nullptr;
            std::size_t dim = 0;

            HeapString() = default;

            HeapString(const HeapString&)            = delete;
            HeapString& operator=(const HeapString&) = delete;

            ~HeapString() { Heap::Free(ptr); }
        };

        // LAME: local_ucs2_substr(); keeps a leading byte order mark
        void Substring(HeapString<char16_t>& dst, const char16_t* src, std::size_t start, std::size_t end) noexcept
        {
            const std::size_t len = 1 + 1 + (start < end ? end - start : 0);
            dst.ptr               = static_cast<char16_t*>(Heap::Allocate(len * sizeof(char16_t), true));
            if (dst.ptr == nullptr || src == nullptr)
                return;
            std::size_t n = 0;
            if (HasUcs2ByteOrderMarker(src[0])) {
                dst.ptr[n++] = src[0];
                if (start == 0)
                    ++start;
            }
            while (start < end)
                dst.ptr[n++] = src[start++];
            dst.ptr[n] = 0;
            dst.dim    = n;
        }

        // LAME: setLang(); a code shorter than three characters is padded with blanks (fix of LAME 4.1)
        void SetLang(char* dst, const char* src) noexcept
        {
            if (src == nullptr || src[0] == 0) {
                dst[0] = 'e';
                dst[1] = 'n';
                dst[2] = 'g';
            } else {
                std::int32_t i = 0;
                for (; i < 3 && src[i] != 0; ++i)
                    dst[i] = src[i];
                for (; i < 3; ++i)
                    dst[i] = ' ';
            }
        }

        // LAME: isSameLang()
        bool IsSameLang(const char* l1, const char* l2) noexcept
        {
            char d[3];
            SetLang(d, l2);
            for (std::int32_t i = 0; i < 3; ++i) {
                char a = ToLower(l1[i]);
                char b = ToLower(d[i]);
                if (a < ' ')
                    a = ' ';
                if (b < ' ')
                    b = ' ';
                if (a != b)
                    return false;
            }
            return true;
        }

        // LAME: nextUpperAlpha()
        const char* NextUpperAlpha(const char* p, char x) noexcept
        {
            for (char c = ToUpper(*p); *p != 0; c = ToUpper(*++p)) {
                if ('A' <= c && c <= 'Z' && c != x)
                    return p;
            }
            return p;
        }

        // LAME: sloppyCompared(); letters only, case-insensitive, "Bros." matches "Brothers"
        bool SloppyCompared(const char* p, const char* q) noexcept
        {
            p       = NextUpperAlpha(p, 0);
            q       = NextUpperAlpha(q, 0);
            char cp = ToUpper(*p);
            char cq = ToUpper(*q);
            while (cp == cq) {
                if (cp == 0)
                    return true;
                if (p[1] == '.') { // some abbrevation
                    while (*q != 0 && *q++ != ' ') {
                    }
                }
                p  = NextUpperAlpha(p, cp);
                q  = NextUpperAlpha(q, cq);
                cp = ToUpper(*p);
                cq = ToUpper(*q);
            }
            return false;
        }

        // LAME: local_strcasecmp() == 0
        bool EqualsIgnoreCase(const char* s1, const char* s2) noexcept
        {
            char c1 = 0;
            char c2 = 0;
            do {
                c1 = ToLower(*s1);
                c2 = ToLower(*s2);
                if (c1 == 0)
                    break;
                ++s1;
                ++s2;
            } while (c1 == c2);
            return c1 == c2;
        }

        // LAME: lookupGenre(); the genre number, -1 for an unknown number, -2 for an unknown name
        std::int32_t LookupGenre(const char* genre) noexcept
        {
            const char*  end = nullptr;
            std::int32_t num = StrToInt(genre, &end);
            if (*end != 0) { // a name
                num = GenreNameCount;
                for (std::int32_t i = 0; i < GenreNameCount && num == GenreNameCount; ++i) {
                    if (EqualsIgnoreCase(genre, GenreNames[i]))
                        num = i;
                }
                for (std::int32_t i = 0; i < GenreNameCount && num == GenreNameCount; ++i) {
                    if (SloppyCompared(genre, GenreNames[i]))
                        num = i;
                }
                if (num == GenreNameCount)
                    return -2; // no common genre text found
            } else if (num < 0 || num >= GenreNameCount) {
                return -1; // number unknown
            }
            return num;
        }

        // LAME: toID3v2TagId(); an id of fewer than four characters is invalid (fix of LAME 4.1)
        std::uint32_t ToId3v2TagId(const char* s) noexcept
        {
            if (s == nullptr)
                return 0;
            std::uint32_t x = 0;
            std::int32_t  i = 0;
            for (; i < 4 && s[i] != 0; ++i) {
                const char c = s[i];
                x            = (x << 8) | static_cast<std::uint8_t>(c);
                if ((c < 'A' || 'Z' < c) && (c < '0' || '9' < c))
                    return 0;
            }
            return i < 4 ? 0 : x;
        }

        // LAME: toID3v2TagId_ucs2(); an id of fewer than four characters is invalid (fix of LAME 4.1)
        std::uint32_t ToId3v2TagIdUcs2(const char16_t* s) noexcept
        {
            if (s == nullptr)
                return 0;
            const char16_t bom = s[0];
            if (HasUcs2ByteOrderMarker(bom))
                ++s;
            std::uint32_t x = 0;
            std::int32_t  i = 0;
            for (; i < 4 && s[i] != 0; ++i) {
                const char16_t c = ToLittleEndian(bom, s[i]);
                if ((c < 'A' || 'Z' < c) && (c < '0' || '9' < c))
                    return 0;
                x = (x << 8) | c;
            }
            return i < 4 ? 0 : x;
        }

        // --- writing the frames

        std::uint8_t* Set4ByteValue(std::uint8_t* bytes, std::uint32_t value) noexcept
        {
            for (std::int32_t i = 3; i >= 0; --i) {
                bytes[i] = static_cast<std::uint8_t>(value & 0xffu);
                value >>= 8;
            }
            return bytes + 4;
        }

        std::uint8_t* WriteChars(std::uint8_t* frame, const char* str, std::size_t n) noexcept
        {
            while (n-- > 0)
                *frame++ = static_cast<std::uint8_t>(*str++);
            return frame;
        }

        std::uint8_t* WriteUcs2s(std::uint8_t* frame, const char16_t* str, std::size_t n) noexcept
        {
            if (n > 0) {
                const char16_t bom = *str;
                while (n-- > 0) {
                    const char16_t c = ToLittleEndian(bom, *str++);
                    *frame++         = static_cast<std::uint8_t>(0x00ffu & c);
                    *frame++         = static_cast<std::uint8_t>(0x00ffu & (c >> 8));
                }
            }
            return frame;
        }

        // LAME: writeLoBytes(); UCS-2 to Latin-1, characters outside Latin-1 become blanks
        std::uint8_t* WriteLoBytes(std::uint8_t* frame, const char16_t* str, std::size_t n) noexcept
        {
            if (n > 0) {
                const char16_t bom = *str;
                if (HasUcs2ByteOrderMarker(bom)) { // skip BOM
                    ++str;
                    --n;
                }
                while (n-- > 0) {
                    const char16_t c = ToLittleEndian(bom, *str++);
                    *frame++         = c < 0x0020u || 0x00ffu < c ? std::uint8_t{0x20} : static_cast<std::uint8_t>(c);
                }
            }
            return frame;
        }

        // LAME: set_text_field(); the text cut or padded to size bytes
        std::uint8_t* SetTextField(std::uint8_t* field, const char* text, std::size_t size) noexcept
        {
            while (size-- > 0) {
                if (text != nullptr && *text != 0)
                    *field++ = static_cast<std::uint8_t>(*text++);
                else
                    *field++ = 0;
            }
            return field;
        }

    } // namespace

    Id3Tag::Id3Tag() noexcept
    {
        genreId3v1_  = GenreNumUnknown;
        paddingSize_ = 128;

        // id3v2AddLameVersion
        const char* const parts[] = {"LAME ", GetLameOsBitness(), " version ", GetLameVersion(), " (", GetLameUrl(), ")"};
        char              buffer[1024];
        std::size_t       n = 0;
        for (const char* part : parts) {
            const std::size_t len = std::strlen(part);
            std::memcpy(buffer + n, part, len);
            n += len;
        }
        buffer[n] = 0;
        CopyV1ToV2(ID_ENCODER, buffer);
    }

    Id3Tag::~Id3Tag()
    {
        for (std::int32_t i = 0; i < frameCount_; ++i) {
            Heap::Free(frames_[i].dsc.ptr);
            Heap::Free(frames_[i].txt.ptr);
        }
        Heap::Free(frames_);
        Heap::Free(title_.ptr);
        Heap::Free(artist_.ptr);
        Heap::Free(album_.ptr);
        Heap::Free(comment_.ptr);
        Heap::Free(albumArt_);
    }

    void Id3Tag::AddV2() noexcept
    {
        flags_ &= ~V1_ONLY_FLAG;
        flags_ |= ADD_V2_FLAG;
    }

    void Id3Tag::V1Only() noexcept
    {
        flags_ &= ~(ADD_V2_FLAG | V2_ONLY_FLAG);
        flags_ |= V1_ONLY_FLAG;
    }

    void Id3Tag::V2Only() noexcept
    {
        flags_ &= ~V1_ONLY_FLAG;
        flags_ |= V2_ONLY_FLAG;
    }

    void Id3Tag::SetPad(std::size_t n) noexcept
    {
        flags_ &= ~V1_ONLY_FLAG;
        flags_ |= PAD_V2_FLAG | ADD_V2_FLAG;
        paddingSize_ = static_cast<std::uint32_t>(n < MaxPadding ? n : MaxPadding); // capped since LAME 4.1
    }

    namespace {

        // LAME: local_strdup() into a field of the tag; empty texts are not stored
        template <typename C>
        std::size_t Duplicate(void*& dst, const C* src) noexcept
        {
            Heap::Free(dst);
            dst                 = nullptr;
            const std::size_t n = Length(src);
            if (n > 0) {
                C* copy = static_cast<C*>(Heap::Allocate((n + 1) * sizeof(C)));
                if (copy != nullptr) {
                    std::memcpy(copy, src, n * sizeof(C));
                    copy[n] = 0;
                    dst     = copy;
                    return n;
                }
            }
            return 0;
        }

    } // namespace

    void Id3Tag::SetTitle(const char* title) noexcept
    {
        if (title != nullptr && *title != 0) {
            title_.dim = Duplicate(title_.ptr, title);
            flags_ |= CHANGED_FLAG;
            CopyV1ToV2(ID_TITLE, title);
        }
    }

    void Id3Tag::SetArtist(const char* artist) noexcept
    {
        if (artist != nullptr && *artist != 0) {
            artist_.dim = Duplicate(artist_.ptr, artist);
            flags_ |= CHANGED_FLAG;
            CopyV1ToV2(ID_ARTIST, artist);
        }
    }

    void Id3Tag::SetAlbum(const char* album) noexcept
    {
        if (album != nullptr && *album != 0) {
            album_.dim = Duplicate(album_.ptr, album);
            flags_ |= CHANGED_FLAG;
            CopyV1ToV2(ID_ALBUM, album);
        }
    }

    void Id3Tag::SetYear(const char* year) noexcept
    {
        if (year != nullptr && *year != 0) {
            std::int32_t num = AToI(year);
            if (num < 0)
                num = 0;
            if (num > 9999) // limit a year to 4 digits so it fits in a version 1 tag
                num = 9999;
            if (num != 0) {
                year_ = num;
                flags_ |= CHANGED_FLAG;
            }
            CopyV1ToV2(ID_YEAR, year);
        }
    }

    void Id3Tag::SetComment(const char* comment) noexcept
    {
        if (comment != nullptr && *comment != 0) {
            comment_.dim = Duplicate(comment_.ptr, comment);
            flags_ |= CHANGED_FLAG;
            const std::uint32_t flags = flags_;
            Add<char>(ID_COMMENT, nullptr, "", comment);
            flags_ = flags;
        }
    }

    std::int32_t Id3Tag::SetTrack(const char* track) noexcept
    {
        std::int32_t ret = 0;
        if (track != nullptr && *track != 0) {
            std::int32_t num = AToI(track);
            if (num < 1 || num > 255) { // track number out of ID3v1 range, ignored for ID3v1
                num = 0;
                ret = -1;
                flags_ |= CHANGED_FLAG | ADD_V2_FLAG;
            }
            if (num != 0) {
                trackId3v1_ = num;
                flags_ |= CHANGED_FLAG;
            }
            if (std::strchr(track, '/') != nullptr) // a total track count after a "/" needs version 2
                flags_ |= CHANGED_FLAG | ADD_V2_FLAG;
            CopyV1ToV2(ID_TRACK, track);
        }
        return ret;
    }

    std::int32_t Id3Tag::SetGenre(const char* genre) noexcept
    {
        if (genre != nullptr && *genre != 0) {
            const std::int32_t num = LookupGenre(genre);
            if (num == -1)
                return num;
            flags_ |= CHANGED_FLAG;
            if (num >= 0) {
                genreId3v1_ = num;
                genre       = GenreNames[num];
            } else {
                genreId3v1_ = GenreIndexOther;
                flags_ |= ADD_V2_FLAG;
            }
            CopyV1ToV2(ID_GENRE, genre);
        }
        return 0;
    }

    std::int32_t Id3Tag::SetFieldValue(const char* fieldValue) noexcept
    {
        if (fieldValue != nullptr && *fieldValue != 0) {
            if (std::strlen(fieldValue) < 5 || fieldValue[4] != '=')
                return -1;
            return SetTextInfoLatin1(fieldValue, &fieldValue[5]);
        }
        return 0;
    }

    std::int32_t Id3Tag::SetFieldValueUtf16(const char16_t* fieldValue) noexcept
    {
        if (fieldValue != nullptr && *fieldValue != 0) {
            const std::size_t   dx        = HasUcs2ByteOrderMarker(fieldValue[0]) ? 1 : 0;
            const char16_t      separator = FromLatin1Char(fieldValue, u'=');
            const std::uint32_t frameId   = ToId3v2TagIdUcs2(fieldValue);
            const std::size_t   length    = Length(fieldValue);
            if (length < 5 + dx || fieldValue[4 + dx] != separator)
                return -1;
            if (frameId != 0) {
                const char           fid[5] = {static_cast<char>((frameId >> 24) & 0xff), static_cast<char>((frameId >> 16) & 0xff),
                                               static_cast<char>((frameId >> 8) & 0xff), static_cast<char>(frameId & 0xff), 0};
                HeapString<char16_t> text;
                Substring(text, fieldValue, dx + 5, length);
                return SetTextInfoUtf16(fid, text.ptr);
            }
        }
        return -1;
    }

    std::int32_t Id3Tag::SetTextInfoLatin1(const char* id, const char* text) noexcept
    {
        const std::uint32_t frameId = ToId3v2TagId(id);
        if (frameId == 0)
            return -1;
        if (text == nullptr)
            return 0;
        if (frameId == ID_TXXX || frameId == ID_WXXX || frameId == ID_COMMENT)
            return SetUserInfoLatin1(frameId, text);
        if (frameId == ID_GENRE)
            return SetGenre(text);
        if (frameId == ID_PCST)
            return Add<char>(frameId, nullptr, nullptr, text);
        if (frameId == ID_USER || frameId == ID_WFED) // iTunes expects WFED to be a text frame
            return Add<char>(frameId, nullptr, text, nullptr);
        if (IsFrameIdMatching(frameId, FrameId('T', 0, 0, 0)) || IsFrameIdMatching(frameId, FrameId('W', 0, 0, 0)))
            return Add<char>(frameId, nullptr, nullptr, text);
        return -255; // not supported by now
    }

    std::int32_t Id3Tag::SetTextInfoUtf16(const char* id, const char16_t* text) noexcept
    {
        const std::uint32_t frameId = ToId3v2TagId(id);
        if (frameId == 0)
            return -1;
        if (text == nullptr)
            return 0;
        if (!HasUcs2ByteOrderMarker(text[0]))
            return -3; // BOM missing
        if (frameId == ID_TXXX || frameId == ID_WXXX || frameId == ID_COMMENT)
            return SetUserInfoUcs2(frameId, text);
        if (frameId == ID_GENRE)
            return SetGenreUtf16(text);
        if (frameId == ID_PCST)
            return Add<char16_t>(frameId, nullptr, nullptr, text);
        if (frameId == ID_USER || frameId == ID_WFED) // iTunes expects WFED to be a text frame
            return Add<char16_t>(frameId, nullptr, text, nullptr);
        if (IsFrameIdMatching(frameId, FrameId('T', 0, 0, 0)) || IsFrameIdMatching(frameId, FrameId('W', 0, 0, 0)))
            return Add<char16_t>(frameId, nullptr, nullptr, text);
        return -255; // not supported by now
    }

    std::int32_t Id3Tag::SetCommentUtf16(const char* lang, const char16_t* desc, const char16_t* text) noexcept
    {
        return Add<char16_t>(ID_COMMENT, lang, desc, text);
    }

    std::int32_t Id3Tag::SetAlbumArt(const std::byte* image, std::size_t size) noexcept
    {
        std::int32_t mimeType = MIMETYPE_NONE;
        if (image != nullptr) {
            // determine MIME type from the actual image data
            const auto* data = reinterpret_cast<const std::uint8_t*>(image);
            if (2 < size && data[0] == 0xFF && data[1] == 0xD8)
                mimeType = MIMETYPE_JPEG;
            else if (4 < size && data[0] == 0x89 && std::memcmp(&data[1], "PNG", 3) == 0)
                mimeType = MIMETYPE_PNG;
            else if (4 < size && std::memcmp(data, "GIF8", 4) == 0)
                mimeType = MIMETYPE_GIF;
            else
                return -1;
        }
        Heap::Free(albumArt_);
        albumArt_         = nullptr;
        albumArtSize_     = 0;
        albumArtMimeType_ = MIMETYPE_NONE;
        if (size < 1 || mimeType == MIMETYPE_NONE)
            return 0;
        albumArt_ = static_cast<std::byte*>(Heap::Allocate(size));
        if (albumArt_ != nullptr) {
            std::memcpy(albumArt_, image, size);
            albumArtSize_     = static_cast<std::uint32_t>(size);
            albumArtMimeType_ = mimeType;
            flags_ |= CHANGED_FLAG;
            AddV2();
        }
        return 0;
    }

    namespace {

        // LAME: sizeOfNode(); a text frame
        template <typename Frame>
        std::size_t SizeOfNode(const Frame& node) noexcept
        {
            std::size_t n = 10; // header size
            n += 1;             // text encoding flag
            if (node.txt.enc != 1) {
                if (node.dsc.dim > 0)
                    n += node.dsc.dim + 1;
                n += node.txt.dim;
            } else {
                if (node.dsc.dim > 0)
                    n += (node.dsc.dim + 1) * 2;
                n += node.txt.dim * 2;
            }
            return n;
        }

        // LAME: sizeOfCommentNode(); COMM and USER
        template <typename Frame>
        std::size_t SizeOfCommentNode(const Frame& node) noexcept
        {
            std::size_t n = 10; // header size
            n += 1;             // text encoding flag
            n += 3;             // language
            n += node.dsc.enc != 1 ? 1 + node.dsc.dim : 2 + node.dsc.dim * 2;
            n += node.txt.enc != 1 ? node.txt.dim : node.txt.dim * 2;
            return n;
        }

        // LAME: sizeOfWxxxNode(); URL frames, the URL always in Latin-1
        template <typename Frame>
        std::size_t SizeOfWxxxNode(const Frame& node) noexcept
        {
            std::size_t n = 10; // header size
            if (node.dsc.dim > 0) {
                n += 1; // text encoding flag
                n += node.dsc.enc != 1 ? 1 + node.dsc.dim : 2 + node.dsc.dim * 2;
            }
            if (node.txt.dim > 0)
                n += node.txt.enc != 1 ? node.txt.dim : node.txt.dim - 1; // UCS2 -> Latin1, skip BOM
            return n;
        }

        // the descriptor with its zero byte(s)
        template <typename Text>
        std::uint8_t* WriteDescriptor(std::uint8_t* frame, const Text& dsc) noexcept
        {
            if (dsc.enc != 1) {
                frame    = WriteChars(frame, dsc.Latin1(), dsc.dim);
                *frame++ = 0;
            } else {
                frame    = WriteUcs2s(frame, dsc.Ucs2(), dsc.dim);
                *frame++ = 0;
                *frame++ = 0;
            }
            return frame;
        }

        template <typename Text>
        std::uint8_t* WriteText(std::uint8_t* frame, const Text& txt) noexcept
        {
            return txt.enc != 1 ? WriteChars(frame, txt.Latin1(), txt.dim) : WriteUcs2s(frame, txt.Ucs2(), txt.dim);
        }

        // LAME: set_frame_comment()
        template <typename Frame>
        std::uint8_t* SetFrameComment(std::uint8_t* frame, const Frame& node) noexcept
        {
            const std::size_t n = SizeOfCommentNode(node);
            if (n > 10) {
                frame    = Set4ByteValue(frame, node.fid);
                frame    = Set4ByteValue(frame, static_cast<std::uint32_t>(n - 10));
                *frame++ = 0; // clear 2-byte header flags
                *frame++ = 0;
                *frame++ = static_cast<std::uint8_t>(node.txt.enc); // the encoding descriptor byte
                *frame++ = static_cast<std::uint8_t>(node.lng[0]);  // 3 bytes language
                *frame++ = static_cast<std::uint8_t>(node.lng[1]);
                *frame++ = static_cast<std::uint8_t>(node.lng[2]);
                frame    = WriteDescriptor(frame, node.dsc);
                frame    = WriteText(frame, node.txt); // comment full text
            }
            return frame;
        }

        // LAME: set_frame_custom2()
        template <typename Frame>
        std::uint8_t* SetFrameCustom2(std::uint8_t* frame, const Frame& node) noexcept
        {
            const std::size_t n = SizeOfNode(node);
            if (n > 10) {
                frame    = Set4ByteValue(frame, node.fid);
                frame    = Set4ByteValue(frame, static_cast<std::uint32_t>(n - 10));
                *frame++ = 0; // clear 2-byte header flags
                *frame++ = 0;
                *frame++ = static_cast<std::uint8_t>(node.txt.enc); // the encoding descriptor byte
                if (node.dsc.dim > 0)
                    frame = WriteDescriptor(frame, node.dsc);
                frame = WriteText(frame, node.txt);
            }
            return frame;
        }

        // LAME: set_frame_wxxx()
        template <typename Frame>
        std::uint8_t* SetFrameWxxx(std::uint8_t* frame, const Frame& node) noexcept
        {
            const std::size_t n = SizeOfWxxxNode(node);
            if (n > 10) {
                frame    = Set4ByteValue(frame, node.fid);
                frame    = Set4ByteValue(frame, static_cast<std::uint32_t>(n - 10));
                *frame++ = 0; // clear 2-byte header flags
                *frame++ = 0;
                if (node.dsc.dim > 0) {
                    *frame++ = static_cast<std::uint8_t>(node.dsc.enc); // the encoding descriptor byte
                    frame    = WriteDescriptor(frame, node.dsc);
                }
                frame = node.txt.enc != 1 ? WriteChars(frame, node.txt.Latin1(), node.txt.dim) : WriteLoBytes(frame, node.txt.Ucs2(), node.txt.dim);
            }
            return frame;
        }

        // LAME: set_frame_apic(); ID3v2.3 APIC: encoding, MIME type, picture type 0, empty description, data
        std::uint8_t* SetFrameApic(std::uint8_t* frame, const char* mimeType, const std::byte* data, std::size_t size) noexcept
        {
            if (mimeType != nullptr && data != nullptr && size > 0) {
                frame    = Set4ByteValue(frame, ID_APIC);
                frame    = Set4ByteValue(frame, static_cast<std::uint32_t>(4 + std::strlen(mimeType) + size));
                *frame++ = 0; // clear 2-byte header flags
                *frame++ = 0;
                *frame++ = 0; // ISO-8859-1
                while (*mimeType != 0)
                    *frame++ = static_cast<std::uint8_t>(*mimeType++);
                *frame++ = 0;
                *frame++ = 0; // picture type 0
                *frame++ = 0; // empty description field
                std::memcpy(frame, data, size);
                frame += size;
            }
            return frame;
        }

    } // namespace

    std::size_t Id3Tag::GetV2Tag(std::byte* buffer, std::size_t size, std::uint32_t numSamples, std::int32_t samplerateIn) noexcept
    {
        if ((flags_ & V1_ONLY_FLAG) != 0)
            return 0;

        bool usev2 = (flags_ & (ADD_V2_FLAG | V2_ONLY_FLAG)) != 0;
        // write tag if explicitly requested or if fields overflow
        if (title_.dim > 30 || artist_.dim > 30 || album_.dim > 30 || comment_.dim > 30 || (trackId3v1_ != 0 && comment_.dim > 28))
            usev2 = true;
        if (!usev2)
            return 0;

        if (numSamples != 0xFFFFFFFFu) { // id3v2AddAudioDuration
            double ms = static_cast<double>(numSamples);
            ms *= 1000;
            ms /= samplerateIn;
            const std::uint32_t playlengthMs = ms > 4294967295.0 ? 0xFFFFFFFFu : ms < 0 ? 0 : static_cast<std::uint32_t>(ms);
            char                text[16];
            UnsignedToText(text, playlengthMs);
            CopyV1ToV2(ID_PLAYLENGTH, text);
        }

        // size of tag starting with 10-byte tag header
        std::size_t tagSize = 10;
        const char* artMime = nullptr;
        if (albumArt_ != nullptr && albumArtSize_ != 0) {
            switch (albumArtMimeType_) {
                case MIMETYPE_JPEG: artMime = "image/jpeg"; break;
                case MIMETYPE_PNG: artMime = "image/png"; break;
                case MIMETYPE_GIF: artMime = "image/gif"; break;
                default: break;
            }
            if (artMime != nullptr)
                tagSize += 10 + 4 + std::strlen(artMime) + albumArtSize_;
        }
        for (std::int32_t i = 0; i < frameCount_; ++i) {
            const Frame& node = frames_[i];
            if (node.fid == ID_COMMENT || node.fid == ID_USER)
                tagSize += SizeOfCommentNode(node);
            else if (IsFrameIdMatching(node.fid, FrameId('W', 0, 0, 0)))
                tagSize += SizeOfWxxxNode(node);
            else
                tagSize += SizeOfNode(node);
        }
        if ((flags_ & PAD_V2_FLAG) != 0)
            tagSize += paddingSize_; // add some bytes of padding
        // LAME 4.1: a tag whose size does not fit into the 28-bit field is left out rather than written corrupt
        if (tagSize - 10 > 0x0FFFFFFFu)
            return 0;
        if (size < tagSize)
            return tagSize;
        if (buffer == nullptr)
            return 0;

        auto* p = reinterpret_cast<std::uint8_t*>(buffer);
        // tag header: file identifier, version 2.3.0, flags, size without the header as 4 x 7 bits
        *p++                       = 'I';
        *p++                       = 'D';
        *p++                       = '3';
        *p++                       = 3;
        *p++                       = 0;
        *p++                       = 0;
        const std::size_t adjusted = tagSize - 10;
        *p++                       = static_cast<std::uint8_t>((adjusted >> 21) & 0x7fu);
        *p++                       = static_cast<std::uint8_t>((adjusted >> 14) & 0x7fu);
        *p++                       = static_cast<std::uint8_t>((adjusted >> 7) & 0x7fu);
        *p++                       = static_cast<std::uint8_t>(adjusted & 0x7fu);

        // NOTE: The frames are not "unsynchronized"; players skip the tag, and false sync patterns are unlikely in text.
        for (std::int32_t i = 0; i < frameCount_; ++i) {
            const Frame& node = frames_[i];
            if (node.fid == ID_COMMENT || node.fid == ID_USER)
                p = SetFrameComment(p, node);
            else if (IsFrameIdMatching(node.fid, FrameId('W', 0, 0, 0)))
                p = SetFrameWxxx(p, node);
            else
                p = SetFrameCustom2(p, node);
        }
        if (artMime != nullptr)
            p = SetFrameApic(p, artMime, albumArt_, albumArtSize_);
        // clear any padding bytes
        std::memset(p, 0, tagSize - static_cast<std::size_t>(p - reinterpret_cast<std::uint8_t*>(buffer)));
        return tagSize;
    }

    std::size_t Id3Tag::GetV1Tag(std::byte* buffer, std::size_t size) const noexcept
    {
        constexpr std::size_t TagSize = 128;
        if (size < TagSize)
            return TagSize;
        if (buffer == nullptr || (flags_ & V2_ONLY_FLAG) != 0 || (flags_ & CHANGED_FLAG) == 0)
            return 0;

        auto* p = reinterpret_cast<std::uint8_t*>(buffer);
        *p++    = 'T';
        *p++    = 'A';
        *p++    = 'G';
        p       = SetTextField(p, title_.Latin1(), 30);
        p       = SetTextField(p, artist_.Latin1(), 30);
        p       = SetTextField(p, album_.Latin1(), 30);
        char year[16];
        UnsignedToText(year, static_cast<std::uint32_t>(year_));
        p = SetTextField(p, year_ != 0 ? year : nullptr, 4);
        // limit comment field to 28 bytes if a track is specified
        p = SetTextField(p, comment_.Latin1(), trackId3v1_ != 0 ? 28 : 30);
        if (trackId3v1_ != 0) {
            *p++ = 0; // a version 1.1 tag
            *p++ = static_cast<std::uint8_t>(trackId3v1_);
        }
        *p = static_cast<std::uint8_t>(genreId3v1_);
        return TagSize;
    }

    void Id3Tag::GenreList(void (*handler)(std::int32_t number, const char* name, void* cookie), void* cookie) noexcept
    {
        if (handler != nullptr) {
            for (std::int32_t i = 0; i < GenreAlphaCount; ++i)
                handler(GenreAlphaMap[i], GenreNames[GenreAlphaMap[i]], cookie);
        }
    }

    // --- private

    std::int32_t Id3Tag::FindFrame(std::uint32_t frameId, std::int32_t start) const noexcept
    {
        for (std::int32_t i = start; i < frameCount_; ++i) {
            if (frames_[i].fid == frameId)
                return i;
        }
        return -1;
    }

    std::int32_t Id3Tag::NewFrame() noexcept
    {
        // LAME keeps the frames in a list without a limit
        if (frameCount_ == frameCapacity_) {
            const std::int32_t capacity = frameCapacity_ == 0 ? 16 : 2 * frameCapacity_;
            void* const        grown    = frames_ == nullptr
                ? Heap::Allocate(static_cast<std::size_t>(capacity) * sizeof(Frame))
                : Heap::Reallocate(frames_, static_cast<std::size_t>(frameCapacity_) * sizeof(Frame), static_cast<std::size_t>(capacity) * sizeof(Frame));
            if (grown == nullptr)
                return -1;
            frames_        = static_cast<Frame*>(grown);
            frameCapacity_ = capacity;
        }
        frames_[frameCount_] = Frame{};
        return frameCount_++;
    }

    // id3v2_add_latin1 (char) and id3v2_add_ucs2 (char16_t)
    template <typename C>
    std::int32_t Id3Tag::Add(std::uint32_t frameId, const char* lng, const C* desc, const C* text) noexcept
    {
        constexpr std::int32_t enc = sizeof(C) == 1 ? 0 : 1;

        char lang[4] = {};
        SetLang(lang, lng);
        std::int32_t index = FindFrame(frameId, 0);
        if (IsMultiFrame(frameId)) {
            // isSameDescriptor and isSameDescriptorUcs2 of LAME 4.1: the whole description, so that one which only
            // begins another names a different frame; no description is the empty one
            auto sameDescriptor = [desc](const Frame& node) {
                if (node.dsc.enc != enc && node.dsc.dim > 0)
                    return false;
                if (desc == nullptr)
                    return node.dsc.dim == 0;
                const C*    dsc = static_cast<const C*>(node.dsc.ptr);
                std::size_t i   = 0;
                for (; i < node.dsc.dim; ++i) {
                    if (dsc[i] != desc[i])
                        return false;
                }
                return desc[i] == 0;
            };
            while (index >= 0 && !(IsSameLang(frames_[index].lng, lang) && sameDescriptor(frames_[index])))
                index = FindFrame(frameId, index + 1);
        }
        if (index < 0) {
            index = NewFrame();
            if (index < 0)
                return -254; // memory problem
        }
        Frame& node = frames_[index];
        node.fid    = frameId;
        SetLang(node.lng, lang);
        node.dsc.dim = Duplicate(node.dsc.ptr, desc);
        node.dsc.enc = enc;
        node.txt.dim = Duplicate(node.txt.ptr, text);
        node.txt.enc = enc;
        flags_ |= CHANGED_FLAG | ADD_V2_FLAG;
        return 0;
    }

    void Id3Tag::CopyV1ToV2(std::uint32_t frameId, const char* text) noexcept
    {
        // the frame alone does not ask for a version 2 tag
        const std::uint32_t flags = flags_;
        Add<char>(frameId, nullptr, nullptr, text);
        flags_ = flags;
    }

    std::int32_t Id3Tag::SetUserInfoLatin1(std::uint32_t frameId, const char* fieldValue) noexcept
    {
        const std::int32_t a = Position(fieldValue, '=');
        if (a < 0)
            return -7;
        HeapString<char> dup;
        dup.dim = Length(fieldValue);
        dup.ptr = static_cast<char*>(Heap::Allocate(dup.dim + 1));
        if (dup.ptr == nullptr)
            return -254; // memory problem
        std::memcpy(dup.ptr, fieldValue, dup.dim + 1);
        dup.ptr[a] = 0;
        return Add<char>(frameId, nullptr, dup.ptr, dup.ptr + a + 1);
    }

    std::int32_t Id3Tag::SetUserInfoUcs2(std::uint32_t frameId, const char16_t* fieldValue) noexcept
    {
        const char16_t     separator = FromLatin1Char(fieldValue, u'=');
        const std::size_t  b         = Length(fieldValue);
        const std::int32_t a         = Position(fieldValue, separator);
        if (a < 0)
            return -7;
        HeapString<char16_t> dsc;
        HeapString<char16_t> val;
        Substring(dsc, fieldValue, 0, static_cast<std::size_t>(a));
        Substring(val, fieldValue, static_cast<std::size_t>(a) + 1, b);
        return Add<char16_t>(frameId, nullptr, dsc.ptr, val.ptr);
    }

    std::int32_t Id3Tag::SetGenreUtf16(const char16_t* text) noexcept
    {
        if (text == nullptr || !HasUcs2ByteOrderMarker(text[0]))
            return -3;
        if (MaybeLatin1(text)) {
            // local_strdup_utf16_to_latin1
            const std::size_t len = Length(text);
            HeapString<char>  latin1;
            latin1.ptr = static_cast<char*>(Heap::Allocate(len + 1, true));
            if (latin1.ptr == nullptr)
                return -254;
            WriteLoBytes(reinterpret_cast<std::uint8_t*>(latin1.ptr), text, len);
            const std::int32_t num = LookupGenre(latin1.ptr);
            if (num == -1)
                return -1;  // number out of range
            if (num >= 0) { // common genre found
                flags_ |= CHANGED_FLAG;
                genreId3v1_ = num;
                CopyV1ToV2(ID_GENRE, GenreNames[num]);
                return 0;
            }
        }
        const std::int32_t ret = Add<char16_t>(ID_GENRE, nullptr, nullptr, text);
        if (ret == 0) {
            flags_ |= CHANGED_FLAG;
            genreId3v1_ = GenreIndexOther;
        }
        return ret;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
