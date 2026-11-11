// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "Console.h"
#include "EncoderOptions.h"
#include "Id3Tag.h"
#include "ParseResult.h"
#include "SimdFeatures.h"
#include "Text.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>PATH_MAX of the LAME frontend; path buffers hold PATH_MAX + 1 characters.</summary>
    inline constexpr std::int32_t PathMax = 1024;

    /// <summary>
    /// The settings of the command line: the encoder options (lame_global_flags), the paths and the frontend settings
    /// (ReaderConfig, WriterConfig and UiConfig of frontend/main.h).
    /// </summary>
    struct Options {
        EncoderOptions                        encoder;
        Id3Tag                                id3; // the ID3 tags (id3tag_spec), written in front of and behind the MP3 frames
        std::array<Shared::Char, PathMax + 1> inPath  = {};
        std::array<Shared::Char, PathMax + 1> outPath = {};

        std::int32_t swapChannel     = 0;
        std::int32_t ignoreWavLength = 0;
        std::int32_t flushWrite      = 0;
        std::int32_t silent          = 0; // verbosity: 10 --quiet, 9 --silent, 5 -S, -5 --brief, -10 --verbose

        // ctLame options, not in LAME (and not in its help texts)
        std::int32_t threads    = 0;               // --threads n: threads of the compute scheduler, 0 = logical processors
        std::int32_t sequential = 0;               // --sequential: the encoder as one task, the reference path
        SimdLevel    simd       = SimdLevel::Avx2; // --simd level: the highest SIMD level to use
        std::int32_t features   = 0;               // --features: print the CPU features and the SIMD level
        std::int32_t stats      = 0;               // --stats (or CTLAME_PROFILE): the stage and task times after encoding
    };

    /// <summary>
    /// The command line of ctLame, a port of parse_args (frontend/parse.c) with LAME's options, messages and quirks.
    /// The same interface in every sample.
    /// </summary>
    class CommandLine
    {
    public:
        /// <summary>
        /// Reads the command line into options: LAMEOPT goes between the program name and the arguments. argv[0] is the
        /// program name, shown without ".exe". Prints the help, version or error texts; without arguments the usage.
        /// </summary>
        static Shared::ParseResult Parse(std::int32_t argc, Shared::Char** argv, Options& options);

        /// <summary>Port of usage: the short usage message.</summary>
        static std::int32_t PrintUsage(Shared::Stream fp, const Shared::Char* programName);

        /// <summary>Port of display_bitrates.</summary>
        static std::int32_t DisplayBitrates(Shared::Stream fp);

    private:
        static constexpr Shared::Char Slash = '\\';
        static constexpr Shared::Char Colon = ':';

        static std::int32_t EvaluateArgument(const Shared::Char* token, const Shared::Char* arg, const Shared::Char* endPtr);
        static std::int32_t GetDoubleValue(const Shared::Char* token, const Shared::Char* arg, double* ptr);
        static std::int32_t GetIntValue(const Shared::Char* token, const Shared::Char* arg, std::int32_t* ptr);
        static std::int32_t VersionPrint(Shared::Stream fp);
        static std::int32_t PrintLicense(Shared::Stream fp);
        static std::int32_t ShortHelp(const EncoderOptions& gfp, Shared::Stream fp, const Shared::Char* programName);
        static void         WaitFor(Shared::Stream fp, std::int32_t lessmode);
        static std::int32_t LongHelp(const EncoderOptions& gfp, Shared::Stream fp, const Shared::Char* programName, std::int32_t lessmode);
        static void         PresetsLongInfo(Shared::Stream msgfp);
        static std::int32_t PresetsSet(EncoderOptions& gfp, const Shared::Char* presetName, const Shared::Char* programName);
        static Shared::Char ToLower(Shared::Char c);
        static std::int32_t IsSpace(Shared::Char c);
        static std::int32_t LocalStrcasecmp(const Shared::Char* s1, const Shared::Char* s2);
        static std::size_t  ScanPath(const Shared::Char* s, const Shared::Char** a, const Shared::Char** b);
        static std::size_t  ScanBasename(const Shared::Char* s, const Shared::Char** a, const Shared::Char** b);
        static std::int32_t IsCommonSuffix(const Shared::Char* ext);
        static std::int32_t GenerateOutPath(const Shared::Char* inPath, const Shared::Char* ext, Shared::Char* out);
        static void         CopyPadded(Shared::Char* dst, const Shared::Char* src, std::size_t n);
        static void         DosToLongFileName(Shared::Char* filename);
        static std::int32_t StringToArgv(Shared::Char* str, Shared::Char** argv, std::int32_t n);
        static std::int32_t MergeArgv(std::int32_t argc, Shared::Char** argv, std::int32_t strArgc, Shared::Char** strArgv, std::int32_t n);
        static std::int32_t ParseArgsInternal(Options& options, std::int32_t argc, Shared::Char** argv);

        /// <summary>The text encodings of the ID3 options (TextEncoding of parse.c, without UTF-8).</summary>
        enum class TextEncoding { Latin1, Utf16 };

        /// <summary>Port of help_id3tag.</summary>
        static void HelpId3Tag(Shared::Stream fp);

        /// <summary>Port of id3_tag: one ID3 option (type: t, a, l, y, c, n, g or v); in UTF-16 mode the text goes in Latin-1 into the ID3v1 fields
        /// first.</summary>
        static std::int32_t Id3TagOption(Id3Tag& tag, char type, TextEncoding encoding, const Shared::Char* text, std::int32_t silent);

        /// <summary>Port of set_id3tag: a Latin-1 text.</summary>
        static std::int32_t SetId3Tag(Id3Tag& tag, char type, const char* text);

        /// <summary>Port of set_id3v2tag: a UTF-16 text with byte order mark.</summary>
        static std::int32_t SetId3v2Tag(Id3Tag& tag, char type, const char16_t* text);

        /// <summary>Port of set_id3_albumart: 0, or 1 to 4 after an error (printed).</summary>
        static std::int32_t SetId3AlbumArt(Id3Tag& tag, const Shared::Char* fileName);

        /// <summary>toLatin1 of the LAME frontend: the argument as Latin-1, on the heap (nullptr if it cannot be converted).</summary>
        static char* ToLatin1(const Shared::Char* text, std::int32_t silent);

        /// <summary>toUtf16 of the LAME frontend: the argument as UTF-16 after a byte order mark, on the heap.</summary>
        static char16_t* ToUtf16(const Shared::Char* text, std::int32_t silent);

#if !defined(_WIN32)
        /// <summary>
        /// Port of convert_text (parse.c of LAME 4.1): iconv of srcBytes of src from the character set of the locale into a
        /// zeroed buffer of (room + 4) * 4 bytes, from byte lead on; reports text that cannot be converted (silent < 9).
        /// </summary>
        static char* ConvertText(const char* to, const char* src, std::size_t srcBytes, std::size_t room, std::size_t lead, std::int32_t silent);
#endif

        /// <summary>The priorities of setProcessPriority.</summary>
        enum class Priority { Low, Normal, High };

        static void SetPriority(Priority priority);

        /// <summary>Port of setProcessPriority (Windows; a nice value on Linux).</summary>
        static void SetProcessPriority(std::int32_t priority);
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
