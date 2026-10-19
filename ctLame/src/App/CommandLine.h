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
#include "ParseResult.h"
#include "SimdFeatures.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>PATH_MAX of the LAME frontend; path buffers hold PATH_MAX + 1 characters.</summary>
    inline constexpr std::int32_t PathMax = 1024;

    /// <summary>
    /// The settings of the command line: the encoder options (lame_global_flags), the paths and the frontend settings
    /// (ReaderConfig, WriterConfig and UiConfig of frontend/main.h).
    /// </summary>
    struct Options {
        EncoderOptions                   encoder;
        std::array<wchar_t, PathMax + 1> inPath  = {};
        std::array<wchar_t, PathMax + 1> outPath = {};

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
        static Shared::ParseResult Parse(std::int32_t argc, wchar_t** argv, Options& options);

        /// <summary>Port of usage: the short usage message.</summary>
        static std::int32_t PrintUsage(Shared::Stream fp, const wchar_t* programName);

        /// <summary>Port of display_bitrates.</summary>
        static std::int32_t DisplayBitrates(Shared::Stream fp);

    private:
        static constexpr wchar_t Slash = L'\\';
        static constexpr wchar_t Colon = L':';

        static std::int32_t EvaluateArgument(const wchar_t* token, const wchar_t* arg, const wchar_t* endPtr);
        static std::int32_t GetDoubleValue(const wchar_t* token, const wchar_t* arg, double* ptr);
        static std::int32_t GetIntValue(const wchar_t* token, const wchar_t* arg, std::int32_t* ptr);
        static std::int32_t VersionPrint(Shared::Stream fp);
        static std::int32_t PrintLicense(Shared::Stream fp);
        static std::int32_t ShortHelp(const EncoderOptions& gfp, Shared::Stream fp, const wchar_t* programName);
        static void         WaitFor(Shared::Stream fp, std::int32_t lessmode);
        static std::int32_t LongHelp(const EncoderOptions& gfp, Shared::Stream fp, const wchar_t* programName, std::int32_t lessmode);
        static void         PresetsLongInfo(Shared::Stream msgfp);
        static std::int32_t PresetsSet(EncoderOptions& gfp, const wchar_t* presetName, const wchar_t* programName);
        static wchar_t      ToLower(wchar_t c);
        static std::int32_t IsSpace(wchar_t c);
        static std::int32_t LocalStrcasecmp(const wchar_t* s1, const wchar_t* s2);
        static std::size_t  ScanPath(const wchar_t* s, const wchar_t** a, const wchar_t** b);
        static std::size_t  ScanBasename(const wchar_t* s, const wchar_t** a, const wchar_t** b);
        static std::int32_t IsCommonSuffix(const wchar_t* ext);
        static std::int32_t GenerateOutPath(const wchar_t* inPath, const wchar_t* ext, wchar_t* out);
        static void         CopyPadded(wchar_t* dst, const wchar_t* src, std::size_t n);
        static void         DosToLongFileName(wchar_t* filename);
        static std::int32_t StringToArgv(wchar_t* str, wchar_t** argv, std::int32_t n);
        static std::int32_t MergeArgv(std::int32_t argc, wchar_t** argv, std::int32_t strArgc, wchar_t** strArgv, std::int32_t n);
        static std::int32_t ParseArgsInternal(Options& options, std::int32_t argc, wchar_t** argv);

        /// <summary>Port of setProcessPriority (Windows).</summary>
        static void SetProcessPriority(std::int32_t priority);
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
