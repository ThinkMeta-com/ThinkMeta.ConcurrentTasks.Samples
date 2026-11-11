// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "CommandLine.h"

#if defined(_WIN32)
#    include <Windows.h>
#else
#    include <clocale>
#    include <iconv.h>
#    include <langinfo.h>
#    include <sys/resource.h>
#endif

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "Console.h"
#include "File.h"
#include "Memory.h"
#include "Tables.h"
#include "Text.h"
#include "Version.h"
#include "VersionInfo.h"
#include "ctLameVersion.h"

// Port of frontend/parse.c. The help texts, messages and the parsing quirks (argument
// skipping, sscanf for -Z, the kHz/Hz heuristics) are kept exactly, so that ctLame accepts and
// rejects the same command lines as lame.exe and prints the same text. LAME works with UTF-8
// arguments, ctLame with the UTF-16 ones: for ASCII both behave the same; paths with other
// characters are printed as they are and found by FindFirstFileW.

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    using Shared::Console;
    using Shared::Stream;
    using Shared::Text;

    Shared::ParseResult CommandLine::Parse(std::int32_t argc, Shared::Char** argv, Options& options)
    {
        // the program name in the help texts without ".exe" (ctLame.exe --help: "usage: ctLame ...")
        if (argc > 0) {
            const std::size_t length = Text::Length(argv[0]);
            if (length > 4 && Text::EqualIgnoreCase(argv[0] + length - 4, CT_TEXT(".exe")))
                argv[0][length - 4] = '\0';
        }
        if (argc <= 1) {
            PrintUsage(Stream::Error, argv[0]); // no command-line args, print usage, exit
            return Shared::ParseResult::Error;
        }

        // LAMEOPT arguments go between the program name and the command line arguments
        constexpr std::int32_t             MaxArgs = 512;
        std::array<Shared::Char*, MaxArgs> strArgv;
        Shared::Char*                      lameopt = nullptr;

#if defined(_WIN32)
        const std::uint32_t size = GetEnvironmentVariableW(CT_TEXT("LAMEOPT"), nullptr, 0);
        if (size > 0) {
            lameopt = static_cast<Shared::Char*>(Shared::Heap::Allocate(size * sizeof(Shared::Char)));
            if (lameopt != nullptr && GetEnvironmentVariableW(CT_TEXT("LAMEOPT"), lameopt, size) >= size) {
                Shared::Heap::Free(lameopt);
                lameopt = nullptr;
            }
        }
#else
        // a copy: StringToArgv splits it in place
        if (const char* value = std::getenv("LAMEOPT"); value != nullptr) {
            const std::size_t size = Text::Length(value) + 1;
            lameopt                = static_cast<Shared::Char*>(Shared::Heap::Allocate(size));
            if (lameopt != nullptr)
                Text::Copy(lameopt, size, value);
        }
#endif

        std::int32_t strArgc = StringToArgv(lameopt, strArgv.data(), MaxArgs);
        strArgc              = MergeArgv(argc, argv, strArgc, strArgv.data(), MaxArgs);
        if (strArgc > MaxArgs)
            strArgc = MaxArgs;

        const std::int32_t ret = ParseArgsInternal(options, strArgc, strArgv.data());
        Shared::Heap::Free(lameopt);
        // parse_args: -1 after an error (exit code 1), -2 after the help or version text (exit code 0)
        return ret == -2 ? Shared::ParseResult::Exit : ret < 0 ? Shared::ParseResult::Error : Shared::ParseResult::Run;
    }

    std::int32_t CommandLine::PrintUsage(Stream fp, const Shared::Char* programName)
    {
        VersionPrint(fp);
        Console::Print(fp,
                       CT_TEXT("usage: %s [options] <infile> [outfile]\n") CT_TEXT("\n")
                           CT_TEXT("    <infile> must be a WAVE file. Reading from stdin and writing to stdout\n") CT_TEXT("    are not supported.\n")
                               CT_TEXT("\n") CT_TEXT("Try:\n") CT_TEXT("     \"%s --help\"           for general usage information\n") CT_TEXT(" or:\n")
                                   CT_TEXT("     \"%s --preset help\"    for information on suggested predefined settings\n") CT_TEXT(" or:\n")
                                       CT_TEXT("     \"%s --longhelp\"\n") CT_TEXT("  or \"%s -?\"              for a complete options list\n\n"),
                       programName, programName, programName, programName, programName);
        return 0;
    }

    std::int32_t CommandLine::DisplayBitrates(Stream fp)
    {
        Console::Print(fp, CT_TEXT("\nMPEG-1   layer III sample frequencies (kHz):  32  48  44.1\n") CT_TEXT("bitrates (kbps):"));
        for (std::int32_t i = 1; i <= 14; i++)
            Console::Print(fp, CT_TEXT(" %2i"), BitrateTable[i]);
        Console::Print(fp, CT_TEXT("\n"));
        Console::Print(fp, CT_TEXT("\n"));
        return 0;
    }

    std::int32_t CommandLine::EvaluateArgument(const Shared::Char* token, const Shared::Char* arg, const Shared::Char* endPtr)
    {
        if (arg != nullptr && arg != endPtr)
            return 1;
        Console::Print(Stream::Error, CT_TEXT("WARNING: argument missing for '%s'\n"), token);
        return 0;
    }

    std::int32_t CommandLine::GetDoubleValue(const Shared::Char* token, const Shared::Char* arg, double* ptr)
    {
        const Shared::Char* endPtr = nullptr;
        const double        d      = Text::ToDouble(arg, &endPtr);
        if (ptr != nullptr)
            *ptr = d;
        return EvaluateArgument(token, arg, endPtr);
    }

    std::int32_t CommandLine::GetIntValue(const Shared::Char* token, const Shared::Char* arg, std::int32_t* ptr)
    {
        const Shared::Char* endPtr = nullptr;
        const auto          d      = Text::ToInt32(arg, &endPtr);
        if (ptr != nullptr)
            *ptr = d;
        return EvaluateArgument(token, arg, endPtr);
    }

    // the banner (lame_version_print with ctLame's name and copyright)
    std::int32_t CommandLine::VersionPrint(Stream fp)
    {
        Console::Print(fp, CT_TEXT("ctLame %hs %hs, based on LAME %hs (%hs)\n"), CTLAME_VERSION_STRING, GetLameOsBitness(), GetLameVersion(), GetLameUrl());
        Console::Print(fp, CT_TEXT("%hs\n\n"), CT_SAMPLES_COPYRIGHT);
        return 0;
    }

    std::int32_t CommandLine::PrintLicense(Stream fp)
    {
        VersionPrint(fp);
        Console::Print(fp,
                       CT_TEXT("ctLame is a parallel port of LAME 4.0 (VBR) to ThinkMeta ConcurrentTasks;\n") "its output is bit-identical to LAME's. LAME:\n"
                                                                                                              "\n");
        Console::Print(fp,
                       CT_TEXT("Copyright (c) 1999-2011 by The LAME Project\n") "Copyright (c) 1999,2000,2001 by Mark Taylor\n"
                                                                                "Copyright (c) 1998 by Michael Cheng\n"
                                                                                "Copyright (c) 1995,1996,1997 by Michael Hipp: mpglib\n"
                                                                                "\n");
        Console::Print(
            fp,
            CT_TEXT("This library is free software; you can redistribute it and/or\n") "modify it under the terms of the GNU Library General Public\n"
                                                                                       "License as published by the Free Software Foundation; either\n"
                                                                                       "version 2 of the License, or (at your option) any later version.\n"
                                                                                       "\n");
        Console::Print(
            fp,
            CT_TEXT("This library is distributed in the hope that it will be useful,\n") "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
                                                                                         "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU\n"
                                                                                         "Library General Public License for more details.\n"
                                                                                         "\n");
        Console::Print(fp,
                       CT_TEXT("You should have received a copy of the GNU Library General Public\n") "License along with this program. If not, see\n"
                                                                                                      "<http://www.gnu.org/licenses/>.\n");
        return 0;
    }

    std::int32_t CommandLine::ShortHelp(const EncoderOptions& gfp, Stream fp, const Shared::Char* programName)
    {
        VersionPrint(fp);
        Console::Print(fp,
                       CT_TEXT("usage: %s [options] <infile> [outfile]\n") "\n"
                                                                           "    <infile> must be a WAVE file with 32, 44.1 or 48 kHz. Reading from\n"
                                                                           "    stdin and writing to stdout are not supported. ctLame encodes MPEG-1\n"
                                                                           "    Layer III VBR (the \"new\" VBR routine) only.\n"
                                                                           "\n"
                                                                           "RECOMMENDED:\n"
                                                                           "    ctlame -V2 input.wav output.mp3\n"
                                                                           "\n",
                       programName);
        Console::Print(fp,
                       CT_TEXT("OPTIONS:\n") "    -V n            quality setting for VBR.  default n=%i\n"
                                             "                    0=high quality,bigger files. 9.999=smaller files\n"
                                             "    -b bitrate      minimum allowed bitrate, default 32 kbps\n"
                                             "    -B bitrate      maximum allowed bitrate, default 320 kbps\n"
                                             "    -h              higher quality, but a little slower.\n"
                                             "    -f              fast mode (lower quality)\n",
                       gfp.vbrQ);
        Console::Print(
            fp,
            CT_TEXT(
                "    --preset type   type must be \"medium\", \"standard\" or \"extreme\"\n") "                    \"--preset help\" gives more info on these\n"
                                                                                              "\n");
        Console::Print(
            fp,
            CT_TEXT("    --priority type  sets the process priority\n") "                     0,1 = Low priority\n"
                                                                        "                     2   = normal priority\n"
                                                                        "                     3,4 = High priority\n"
                                                                        "\n"
                                                                        "    --threads n     encode with n threads (default: all logical processors)\n"
                                                                        "\n"
                                                                        "    --help id3      ID3 tagging related options\n"
                                                                        "\n"
                                                                        "    --longhelp      full list of options\n"
                                                                        "\n"
                                                                        "    --license       print License information\n\n");
        return 0;
    }

    void CommandLine::WaitFor(Stream fp, std::int32_t lessmode)
    {
        if (lessmode) {
            (void)std::getchar();
        } else {
            Console::Print(fp, CT_TEXT("\n"));
        }
        Console::Print(fp, CT_TEXT("\n"));
    }

    std::int32_t CommandLine::LongHelp(const EncoderOptions& gfp, Stream fp, const Shared::Char* programName, std::int32_t lessmode)
    {
        VersionPrint(fp);
        Console::Print(fp,
                       CT_TEXT("usage: %s [options] <infile> [outfile]\n") "\n"
                                                                           "    <infile> must be a WAVE file with 32, 44.1 or 48 kHz. Reading from\n"
                                                                           "    stdin and writing to stdout are not supported. ctLame encodes MPEG-1\n"
                                                                           "    Layer III VBR (the \"new\" VBR routine) only.\n"
                                                                           "\n"
                                                                           "RECOMMENDED:\n"
                                                                           "    ctlame -V2 input.wav output.mp3\n"
                                                                           "\n",
                       programName);
        Console::Print(fp,
                       CT_TEXT("OPTIONS:\n") "  Input options:\n"
                                             "    --scale <arg>   scale input (multiply PCM data) by <arg>\n"
                                             "    --scale-l <arg> scale channel 0 (left) input (multiply PCM data) by <arg>\n"
                                             "    --scale-r <arg> scale channel 1 (right) input (multiply PCM data) by <arg>\n"
                                             "    --swap-channel  swap L/R channels\n"
                                             "    --ignorelength  ignore file length in WAV header\n"
                                             "    --gain <arg>    apply Gain adjustment in decibels, range -20.0 to +12.0\n"
                                             "    -a              downmix from stereo to mono file for mono encoding\n"
                                             "\n");

        WaitFor(fp, lessmode);
        Console::Print(fp,
                       CT_TEXT("  Operational options:\n") "    -m <mode>       (j)oint, (s)imple, (f)orce, (d)ual-mono, (m)ono (l)eft (r)ight\n"
                                                           "                    default is (j)\n"
                                                           "                    joint  = Uses the best possible of MS and LR stereo\n"
                                                           "                    simple = force LR stereo on all frames\n"
                                                           "                    force  = force MS stereo on all frames.\n");
        Console::Print(
            fp,
            CT_TEXT(
                "    --preset type   type must be \"medium\", \"standard\" or \"extreme\"\n") "                    \"--preset help\" gives more info on these\n"
                                                                                              "    --r3mix         the tuning of the former r3mix preset, like "
                                                                                              "-V 3\n"
                                                                                              "    --replaygain-fast   compute RG fast but slightly "
                                                                                              "inaccurately (default)\n"
                                                                                              "    --noreplaygain  disable ReplayGain analysis\n"
                                                                                              "    --flush         flush output stream as soon as possible\n");

        WaitFor(fp, lessmode);
        Console::Print(fp,
                       CT_TEXT("  Verbosity:\n") "    -S              don't print the LAME tag message\n"
                                                 "    --quiet         don't print anything on screen\n"
                                                 "    --silent        don't print anything on screen, but fatal errors\n"
                                                 "    --brief         print more useful information\n"
                                                 "    --verbose       print a lot of useful information\n"
                                                 "\n");
        Console::Print(
            fp,
            CT_TEXT("  Noise shaping & psycho acoustic algorithms:\n") "    -q <arg>        <arg> = 0...9.  Default  -q 3 \n"
                                                                       "                    -q 0:  Highest quality, very slow \n"
                                                                       "                    -q 9:  Poor quality, but fast \n"
                                                                       "    -h              Same as -q 2.   \n"
                                                                       "    -f              Same as -q 7.   Fast, ok quality\n"
                                                                       "    --athaa-sensitivity x  activation offset in -/+ dB for ATH auto-adjustment\n"
                                                                       "    -Z [n]          always do calculate short block maskings\n");

        WaitFor(fp, lessmode);
        Console::Print(fp,
                       CT_TEXT("  VBR options:\n") "    -V n            quality setting for VBR.  default n=%i\n"
                                                   "                    0=high quality,bigger files. 9.999=smaller files\n"
                                                   "    -Y              lets ctLame ignore noise in sfb21\n"
                                                   "                    (Default for V3 to V9.999)\n",
                       gfp.vbrQ);
        Console::Print(
            fp,
            CT_TEXT("    -b <bitrate>    specify minimum allowed bitrate, default  32 kbps\n") "    -B <bitrate>    specify maximum allowed bitrate, default "
                                                                                               "320 kbps\n"
                                                                                               "    -F              strictly enforce the -b option, for use "
                                                                                               "with players that\n"
                                                                                               "                    do not support low bitrate mp3\n"
                                                                                               "    -t              disable writing LAME Tag\n"
                                                                                               "    -T              enable and force writing LAME Tag\n");

        WaitFor(fp, lessmode);
        Console::Print(fp,
                       CT_TEXT("  MP3 header/stream options:\n") "    -e <emp>        de-emphasis n/5/c  (obsolete)\n"
                                                                 "    -c              mark as copyright\n"
                                                                 "    -o              mark as non-original\n"
                                                                 "    -p              error protection.  adds 16 bit checksum to every frame\n"
                                                                 "                    (the checksum is computed correctly)\n"
                                                                 "    --nores         disable the bit reservoir\n"
                                                                 "    --strictly-enforce-ISO   comply as much as possible to ISO MPEG spec\n");
        Console::Print(
            fp,
            CT_TEXT("    --buffer-constraint <constraint> available values for constraint:\n") "                                     default, strict, maximum\n"
                                                                                               "\n");
        Console::Print(fp,
                       CT_TEXT("  Filter options:\n") "  --lowpass <freq>        frequency(kHz), lowpass filter cutoff above freq\n"
                                                      "  --lowpass-width <freq>  frequency(kHz) - default 15%% of lowpass freq\n"
                                                      "  --highpass <freq>       frequency(kHz), highpass filter cutoff below freq\n"
                                                      "  --highpass-width <freq> frequency(kHz) - default 15%% of highpass freq\n");

        WaitFor(fp, lessmode);
        HelpId3Tag(fp);
        Console::Print(
            fp,
            CT_TEXT("  ctLame options (parallel encoding, SIMD):\n") "    --threads <n>   compute threads, 1 to 256 (default: all logical processors)\n"
                                                                     "    --sequential    the whole encoder in one task, without parallel stages\n"
                                                                     "                    (the reference path, slower)\n"
                                                                     "    --simd <level>  the highest SIMD level to use: scalar, sse4.1 or avx2\n"
                                                                     "                    (default: the highest the CPU supports; all levels give\n"
                                                                     "                    the same bits)\n"
                                                                     "    --features      print the CPU features and the SIMD level in use\n"
                                                                     "    --stats         print the stage and task times after encoding\n"
                                                                     "                    (also: environment variable CTLAME_PROFILE)\n");
        Console::Print(fp,
                       CT_TEXT("\n\nMS-Windows-specific options:\n") "    --priority <type>  sets the process priority:\n"
                                                                     "                         0,1 = Low priority (IDLE_PRIORITY_CLASS)\n"
                                                                     "                         2 = normal priority (NORMAL_PRIORITY_CLASS, default)\n"
                                                                     "                         3,4 = High priority (HIGH_PRIORITY_CLASS))\n"
                                                                     "    Note: Calling '--priority' without a parameter will select priority 0.\n"
                                                                     "\nMisc:\n    --license       print License information\n\n");

        DisplayBitrates(fp);

        return 0;
    }

    void CommandLine::PresetsLongInfo(Stream msgfp)
    {
        Console::Print(msgfp,
                       CT_TEXT("\n") "The --preset switches are aliases over ctLame settings.\n"
                                     "\n"
                                     "\n");
        Console::Print(
            msgfp,
            CT_TEXT(
                "     --preset medium      (-V 4) This preset should provide near transparency\n") "                          to most people on most music.\n"
                                                                                                   "\n"
                                                                                                   "     --preset standard    (-V 2) This preset should "
                                                                                                   "generally be transparent\n"
                                                                                                   "                          to most people on most music and "
                                                                                                   "is already quite\n"
                                                                                                   "                          high in quality.\n"
                                                                                                   "\n");
        Console::Print(msgfp,
                       CT_TEXT("     --preset extreme     (-V 0) If you have extremely good hearing and similar\n") "                          equipment, this "
                                                                                                                    "preset will generally provide\n"
                                                                                                                    "                          slightly higher "
                                                                                                                    "quality than the \"standard\" mode.\n"
                                                                                                                    "\n");
        Console::Print(msgfp,
                       CT_TEXT("    For example:\n") "\n"
                                                     "    --preset standard <input file> <output file>\n"
                                                     " or --preset extreme <input file> <output file>\n"
                                                     "\n");
    }

    std::int32_t CommandLine::PresetsSet(EncoderOptions& gfp, const Shared::Char* presetName, const Shared::Char* programName)
    {
        if (Text::Equal(presetName, CT_TEXT("help"))) {
            VersionPrint(Stream::Output);
            PresetsLongInfo(Stream::Output);
            return -1;
        }
        if (Text::Equal(presetName, CT_TEXT("medium"))) {
            gfp.SetVbrQ(4);
            return 0;
        }
        if (Text::Equal(presetName, CT_TEXT("standard"))) {
            gfp.SetVbrQ(2);
            return 0;
        } else if (Text::Equal(presetName, CT_TEXT("extreme"))) {
            gfp.SetVbrQ(0);
            return 0;
        }

        VersionPrint(Stream::Error);
        Console::Print(
            Stream::Error,
            CT_TEXT("Error: You did not enter a valid profile and/or options with --preset\n") "\n"
                                                                                               "Available profiles are:\n"
                                                                                               "\n"
                                                                                               "                 medium\n"
                                                                                               "                 standard\n"
                                                                                               "                 extreme\n"
                                                                                               "\n"
                                                                                               "For further information try: \"%ls --preset help\"\n",
            programName);
        return -1;
    }

    // tolower/isspace of the "C" locale: ASCII only
    Shared::Char CommandLine::ToLower(Shared::Char c)
    {
        return c >= 'A' && c <= 'Z' ? static_cast<Shared::Char>(c - 'A' + 'a') : c;
    }

    std::int32_t CommandLine::IsSpace(Shared::Char c)
    {
        return c == ' ' || (c >= '\t' && c <= '\r');
    }

    // would use real "strcasecmp" but it isn't portable
    std::int32_t CommandLine::LocalStrcasecmp(const Shared::Char* s1, const Shared::Char* s2)
    {
        Shared::Char c1;
        Shared::Char c2;
        do {
            c1 = ToLower(*s1);
            c2 = ToLower(*s2);
            if (!c1)
                break;
            ++s1;
            ++s2;
        } while (c1 == c2);
        return c1 - c2;
    }

    std::size_t CommandLine::ScanPath(const Shared::Char* s, const Shared::Char** a, const Shared::Char** b)
    {
        const Shared::Char* s1 = s;
        const Shared::Char* s2 = s;
        if (s != nullptr) {
            for (; *s; ++s) {
                if (*s == Slash || *s == Colon)
                    s2 = s;
            }
            if (*s2 == Colon)
                ++s2;
        }
        if (a)
            *a = s1;
        if (b)
            *b = s2;
        return static_cast<std::size_t>(s2 - s1);
    }

    std::size_t CommandLine::ScanBasename(const Shared::Char* s, const Shared::Char** a, const Shared::Char** b)
    {
        const Shared::Char* s1 = s;
        const Shared::Char* s2 = s;
        if (s != nullptr) {
            for (; *s; ++s) {
                if (*s == Slash || *s == Colon)
                    s1 = s2 = s;
                else if (*s == '.')
                    s2 = s;
            }
            if (s2 == s1)
                s2 = s;
            if (*s1 == Slash || *s1 == Colon)
                ++s1;
        }
        if (a != nullptr)
            *a = s1;
        if (b != nullptr)
            *b = s2;
        return static_cast<std::size_t>(s2 - s1);
    }

    std::int32_t CommandLine::IsCommonSuffix(const Shared::Char* ext)
    {
        const auto suffixes = std::to_array<const Shared::Char*>(
            {CT_TEXT(".WAV"), CT_TEXT(".RAW"), CT_TEXT(".MP1"), CT_TEXT(".MP2"),  CT_TEXT(".MP3"), CT_TEXT(".MPG"), CT_TEXT(".MPA"),
             CT_TEXT(".CDA"), CT_TEXT(".OGG"), CT_TEXT(".AIF"), CT_TEXT(".AIFF"), CT_TEXT(".AU"),  CT_TEXT(".SND"), CT_TEXT(".FLAC"),
             CT_TEXT(".WV"),  CT_TEXT(".OFR"), CT_TEXT(".TAK"), CT_TEXT(".MP4"),  CT_TEXT(".M4A"), CT_TEXT(".PCM"), CT_TEXT(".W64")});
        for (const Shared::Char* suffix : suffixes) {
            if (LocalStrcasecmp(ext, suffix) == 0)
                return 1;
        }
        return 0;
    }

    // generateOutPath with an empty output directory: the input name with its common
    // suffix replaced by ext.
    std::int32_t CommandLine::GenerateOutPath(const Shared::Char* inPath, const Shared::Char* ext, Shared::Char* out)
    {
        const std::size_t maxPath = PathMax;
        std::size_t       i       = 0;

        {
            const Shared::Char* pa;
            const Shared::Char* pb;
            std::size_t         n = ScanPath(inPath, &pa, &pb);
            if (i + n >= maxPath)
                goto error;
            for (std::size_t j = 0; j < n; ++j)
                out[i++] = pa[j];
            if (n > 0) {
                out[i++] = Slash;
                if (i >= maxPath)
                    goto error;
            }
            out[i] = 0;
        }
        {
            std::int32_t        replaceSuffix = 0;
            const Shared::Char* na;
            const Shared::Char* nb;
            std::size_t         n = ScanBasename(inPath, &na, &nb);
            if (i + n >= maxPath)
                goto error;
            for (std::size_t j = 0; j < n; ++j)
                out[i++] = na[j];
            out[i] = 0;
            if (IsCommonSuffix(nb) == 1) {
                replaceSuffix = 1;
                if (LocalStrcasecmp(nb, ext) == 0)
                    replaceSuffix = 0;
            }
            if (replaceSuffix == 0) {
                while (*nb) {
                    out[i++] = *nb++;
                    if (i >= maxPath)
                        goto error;
                }
                out[i] = 0;
            }
        }
        if (i + 5 >= maxPath)
            goto error;
        while (*ext)
            out[i++] = *ext++;
        out[i] = 0;
        return 0;

    error:
        Console::Print(Stream::Error, CT_TEXT("error: output file name too long\n"));
        return 1;
    }

    // strncpy: copies at most n characters and pads with zeros up to n
    void CommandLine::CopyPadded(Shared::Char* dst, const Shared::Char* src, std::size_t n)
    {
        std::size_t k = 0;
        for (; k < n && src[k] != '\0'; ++k)
            dst[k] = src[k];
        for (; k < n; ++k)
            dst[k] = '\0';
    }

    // dosToLongFileName: replaces the file name part by the name the file system reports.
    void CommandLine::DosToLongFileName([[maybe_unused]] Shared::Char* filename)
    {
#if defined(_WIN32)
        const std::size_t msize = PathMax + 1 - 4; // we wanna add ".mp3" later
        WIN32_FIND_DATAW  findData;
        HANDLE            h = FindFirstFileW(filename, &findData);
        if (h == INVALID_HANDLE_VALUE)
            return;

        FindClose(h);
        std::size_t a;
        for (a = 0; a < msize && a < MAX_PATH; a++) {
            if ('\0' == findData.cFileName[a])
                break;
        }
        if (a >= msize || a == 0)
            return;

        Shared::Char* q = Text::FindLast(filename, '\\');
        Shared::Char* p = Text::FindLast(filename, '/');
        if (p - q > 0)
            q = p;
        if (q == nullptr)
            q = Text::FindLast(filename, ':');

        if (q == nullptr) {
            CopyPadded(filename, findData.cFileName, a);
        } else {
            a += static_cast<std::size_t>(q - filename + 1);
            if (a >= msize)
                return;
            CopyPadded(++q, findData.cFileName, msize - a);
        }
#endif // LAME changes the name only on Windows
    }

    // string_to_argv for LAMEOPT: splits str in place into blank separated arguments, double
    // quotes group an argument. argv[0] is a placeholder for the program name.
    std::int32_t CommandLine::StringToArgv(Shared::Char* str, Shared::Char** argv, std::int32_t n)
    {
        std::int32_t argc = 0;
        if (str == nullptr)
            return argc;
        argv[argc++] = const_cast<Shared::Char*>(CT_TEXT("lhama"));
        for (;;) {
            std::int32_t quoted = 0;
            while (IsSpace(*str)) // skip blanks
                ++str;
            if (*str == '\"') { // is quoted argument ?
                quoted = 1;
                ++str;
            }
            if (*str == '\0') // end of string reached
                break;
            // found beginning of some argument
            if (argc < n)
                argv[argc++] = str;
            // look out for end of argument, either end of string, blank or quote
            for (; *str != '\0'; ++str) {
                if (quoted) {
                    if (*str == '\"') { // end of quotation reached
                        *str++ = '\0';
                        break;
                    }
                } else {
                    if (IsSpace(*str)) { // parameter separator reached
                        *str++ = '\0';
                        break;
                    }
                }
            }
        }
        return argc;
    }

    std::int32_t CommandLine::MergeArgv(std::int32_t argc, Shared::Char** argv, std::int32_t strArgc, Shared::Char** strArgv, std::int32_t n)
    {
        if (argc > 0) {
            strArgv[0] = argv[0];
            if (strArgc < 1)
                strArgc = 1;
        }
        for (std::int32_t i = 1; i < argc; ++i) {
            const std::int32_t j = strArgc + i - 1;
            if (j < n)
                strArgv[j] = argv[i];
        }
        return argc + strArgc - 1;
    }

    std::int32_t CommandLine::ParseArgsInternal(Options& options, std::int32_t argc, Shared::Char** argv)
    {
        EncoderOptions&           gfp             = options.encoder;
        Shared::Char* const       inPath          = options.inPath.data();
        Shared::Char* const       outPath         = options.outPath.data();
        std::int32_t              inputFile       = 0; // set to 1 if we parse an input file name
        std::int32_t              autoconvert     = 0;
        std::int32_t              noreplaygain    = 0; // is RG explicitly disabled by the user
        const Shared::Char* const programName     = argv[0];
        Id3Tag&                   tag             = options.id3;
        TextEncoding              id3Encoding     = TextEncoding::Utf16; // ID3TAGS_EXTENDED: the default of LAME on Windows and Linux
        std::int32_t              ignoreTagErrors = 0;                   // ignore errors in values passed for tags

        // ID3TAG_MODE: what the error messages of --tn and --tg depend on
        enum class Id3TagMode { Default, V1Only, V2Only };
        Id3TagMode id3TagMode = Id3TagMode::Default;

#if !defined(_WIN32)
        // HAVE_LANGINFO_H: the tag texts are converted from the character set of the locale
        std::setlocale(LC_CTYPE, "");
#endif
        inPath[0]  = '\0';
        outPath[0] = '\0';
        // turn on display options. user settings may turn them off below
        options.silent = 0;

        for (std::int32_t i = 0; ++i < argc;) {
            const Shared::Char* token = argv[i];
            std::int32_t        argUsed;

            if (*token++ == '-') {
                const Shared::Char* nextArg = i + 1 < argc ? argv[i + 1] : CT_TEXT("");
                argUsed                     = 0;
                if (!*token) { // "-" would mean stdin or stdout
                    Console::Print(Stream::Error, CT_TEXT("Reading from stdin and writing to stdout are not supported.\n"));
                    return -1;
                }
                if (*token == '-') { // GNU style
                    double       doubleValue = 0;
                    std::int32_t intValue    = 0;
                    token++;

                    auto is = [&](const Shared::Char* name) { return 0 == LocalStrcasecmp(token, name); };

                    if (is(CT_TEXT("r3mix"))) {
                        gfp.SetPreset(R3Mix);
                    } else if (is(CT_TEXT("flush"))) {
                        options.flushWrite = 1;
                    } else if (is(CT_TEXT("nores"))) {
                        gfp.SetDisableReservoir(1);
                    } else if (is(CT_TEXT("strictly-enforce-ISO"))) {
                        gfp.SetStrictIso(MdbStrictIso);
                    } else if (is(CT_TEXT("buffer-constraint"))) {
                        argUsed = 1;
                        if (Text::Equal(nextArg, CT_TEXT("default")))
                            (void)gfp.SetStrictIso(MdbDefault);
                        else if (Text::Equal(nextArg, CT_TEXT("strict")))
                            (void)gfp.SetStrictIso(MdbStrictIso);
                        else if (Text::Equal(nextArg, CT_TEXT("maximum")))
                            (void)gfp.SetStrictIso(MdbMaximum);
                        else {
                            Console::Print(Stream::Error, CT_TEXT("unknown buffer constraint '%s'\n"), nextArg);
                            return -1;
                        }
                    } else if (is(CT_TEXT("scale"))) {
                        argUsed = GetDoubleValue(token, nextArg, &doubleValue);
                        if (argUsed)
                            (void)gfp.SetScale(static_cast<float>(doubleValue));
                    } else if (is(CT_TEXT("scale-l"))) {
                        argUsed = GetDoubleValue(token, nextArg, &doubleValue);
                        if (argUsed)
                            (void)gfp.SetScaleLeft(static_cast<float>(doubleValue));
                    } else if (is(CT_TEXT("scale-r"))) {
                        argUsed = GetDoubleValue(token, nextArg, &doubleValue);
                        if (argUsed)
                            (void)gfp.SetScaleRight(static_cast<float>(doubleValue));
                    } else if (is(CT_TEXT("gain"))) {
                        argUsed = GetDoubleValue(token, nextArg, &doubleValue);
                        if (argUsed) {
                            double gain = doubleValue;
                            gain        = gain > -20.f ? gain : -20.f;
                            gain        = gain < 12.f ? gain : 12.f;
                            gain        = std::pow(10.f, gain * 0.05);
                            (void)gfp.SetScale(static_cast<float>(gain));
                        }
                    } else if (is(CT_TEXT("priority"))) {
                        argUsed = GetIntValue(token, nextArg, &intValue);
                        if (argUsed)
                            SetProcessPriority(intValue);
                    } else if (is(CT_TEXT("lowpass"))) {
                        argUsed = GetDoubleValue(token, nextArg, &doubleValue);
                        if (argUsed) {
                            if (doubleValue < 0) {
                                gfp.SetLowpassFreq(-1);
                            } else {
                                // useful are 0.001 kHz...50 kHz, 50 Hz...50000 Hz
                                if (doubleValue < 0.001 || doubleValue > 50000.) {
                                    Console::Print(Stream::Error, CT_TEXT("Must specify lowpass with --lowpass freq, freq >= 0.001 kHz\n"));
                                    return -1;
                                }
                                gfp.SetLowpassFreq(static_cast<std::int32_t>(doubleValue * (doubleValue < 50. ? 1.e3 : 1.e0) + 0.5));
                            }
                        }
                    } else if (is(CT_TEXT("lowpass-width"))) {
                        argUsed = GetDoubleValue(token, nextArg, &doubleValue);
                        if (argUsed) {
                            // useful are 0.001 kHz...16 kHz, 16 Hz...50000 Hz
                            if (doubleValue < 0.001 || doubleValue > 50000.) {
                                Console::Print(Stream::Error, CT_TEXT("Must specify lowpass width with --lowpass-width freq, freq >= 0.001 kHz\n"));
                                return -1;
                            }
                            gfp.SetLowpassWidth(static_cast<std::int32_t>(doubleValue * (doubleValue < 16. ? 1.e3 : 1.e0) + 0.5));
                        }
                    } else if (is(CT_TEXT("highpass"))) {
                        argUsed = GetDoubleValue(token, nextArg, &doubleValue);
                        if (argUsed) {
                            if (doubleValue < 0.0) {
                                gfp.SetHighpassFreq(-1);
                            } else {
                                // useful are 0.001 kHz...16 kHz, 16 Hz...50000 Hz
                                if (doubleValue < 0.001 || doubleValue > 50000.) {
                                    Console::Print(Stream::Error, CT_TEXT("Must specify highpass with --highpass freq, freq >= 0.001 kHz\n"));
                                    return -1;
                                }
                                gfp.SetHighpassFreq(static_cast<std::int32_t>(doubleValue * (doubleValue < 16. ? 1.e3 : 1.e0) + 0.5));
                            }
                        }
                    } else if (is(CT_TEXT("highpass-width"))) {
                        argUsed = GetDoubleValue(token, nextArg, &doubleValue);
                        if (argUsed) {
                            // useful are 0.001 kHz...16 kHz, 16 Hz...50000 Hz
                            if (doubleValue < 0.001 || doubleValue > 50000.) {
                                Console::Print(Stream::Error, CT_TEXT("Must specify highpass width with --highpass-width freq, freq >= 0.001 kHz\n"));
                                return -1;
                            }
                            // LAME takes the value as Hz here, without the kHz heuristic
                            gfp.SetHighpassWidth(static_cast<std::int32_t>(doubleValue));
                        }
                    } else if (is(CT_TEXT("quiet"))) {
                        options.silent = 10; // on a scale from 1 to 10 be very silent
                    } else if (is(CT_TEXT("silent"))) {
                        options.silent = 9;
                    } else if (is(CT_TEXT("brief"))) {
                        options.silent = -5; // print few info on screen
                    } else if (is(CT_TEXT("verbose"))) {
                        options.silent = -10; // print a lot on screen
                    } else if (is(CT_TEXT("version")) || is(CT_TEXT("license"))) {
                        PrintLicense(Stream::Output);
                        return -2;
                    } else if (is(CT_TEXT("help")) || is(CT_TEXT("usage"))) {
                        // local_strncasecmp(nextArg, "id3", 3)
                        const Shared::Char* const id3 = CT_TEXT("id3");
                        std::int32_t              k   = 0;
                        while (k < 3 && ToLower(nextArg[k]) == id3[k])
                            ++k;
                        if (k == 3)
                            HelpId3Tag(Stream::Output);
                        else
                            ShortHelp(gfp, Stream::Output, programName);
                        return -2;
                    } else if (is(CT_TEXT("longhelp"))) {
                        LongHelp(gfp, Stream::Output, programName, 0);
                        return -2;
                    } else if (is(CT_TEXT("?"))) {
                        LongHelp(gfp, Stream::Output, programName, 1);
                        return -2;
                    } else if (is(CT_TEXT("preset"))) {
                        argUsed = 1;
                        if (PresetsSet(gfp, nextArg, programName) < 0)
                            return -1;
                    } else if (is(CT_TEXT("threads"))) {
                        // ctLame: threads of the compute scheduler
                        argUsed = GetIntValue(token, nextArg, &intValue);
                        if (argUsed) {
                            if (intValue < 1 || intValue > 256) {
                                Console::Print(Stream::Error, CT_TEXT("%s: --threads must be from 1 to 256, not %d\n"), programName, intValue);
                                return -1;
                            }
                            options.threads = intValue;
                        }
                    } else if (is(CT_TEXT("sequential"))) {
                        // ctLame: the encoder as one task (reference path)
                        options.sequential = 1;
                    } else if (is(CT_TEXT("simd"))) {
                        // ctLame: the highest bit-identical SIMD level (scalar, sse4.1, avx2)
                        argUsed = 1;
                        if (!ParseSimdLevel(nextArg, options.simd)) {
                            Console::Print(Stream::Error, CT_TEXT("%s: --simd must be scalar, sse4.1 or avx2, not '%s'\n"), programName, nextArg);
                            return -1;
                        }
                    } else if (is(CT_TEXT("features"))) {
                        // ctLame: print the CPU features and the SIMD level
                        options.features = 1;
                    } else if (is(CT_TEXT("stats"))) {
                        // ctLame: the stage and task times after encoding
                        options.stats = 1;
                    } else if (is(CT_TEXT("swap-channel"))) {
                        options.swapChannel = 1;
                    } else if (is(CT_TEXT("ignorelength"))) {
                        options.ignoreWavLength = 1;
                    } else if (is(CT_TEXT("replaygain-fast"))) {
                        (void)gfp.SetFindReplayGain(1);
                    } else if (is(CT_TEXT("noreplaygain"))) {
                        noreplaygain = 1;
                        (void)gfp.SetFindReplayGain(0);
                    } else if (is(CT_TEXT("athaa-sensitivity"))) {
                        argUsed = GetDoubleValue(token, nextArg, &doubleValue);
                        if (argUsed)
                            gfp.SetAthaaSensitivity(static_cast<float>(doubleValue));

                        // options for ID3 tag
                    } else if (is(CT_TEXT("id3v2-utf16"))) {
                        id3Encoding = TextEncoding::Utf16;
                        tag.AddV2();
                    } else if (is(CT_TEXT("id3v2-latin1"))) {
                        id3Encoding = TextEncoding::Latin1;
                        tag.AddV2();
                    } else if (is(CT_TEXT("tt"))) {
                        argUsed = 1;
                        Id3TagOption(tag, 't', id3Encoding, nextArg, options.silent);
                    } else if (is(CT_TEXT("ta"))) {
                        argUsed = 1;
                        Id3TagOption(tag, 'a', id3Encoding, nextArg, options.silent);
                    } else if (is(CT_TEXT("tl"))) {
                        argUsed = 1;
                        Id3TagOption(tag, 'l', id3Encoding, nextArg, options.silent);
                    } else if (is(CT_TEXT("ty"))) {
                        argUsed = 1;
                        Id3TagOption(tag, 'y', id3Encoding, nextArg, options.silent);
                    } else if (is(CT_TEXT("tc"))) {
                        argUsed = 1;
                        Id3TagOption(tag, 'c', id3Encoding, nextArg, options.silent);
                    } else if (is(CT_TEXT("tn"))) {
                        const std::int32_t ret = Id3TagOption(tag, 'n', id3Encoding, nextArg, options.silent);
                        argUsed                = 1;
                        if (ret != 0 && ignoreTagErrors == 0) {
                            if (id3TagMode == Id3TagMode::V1Only) {
                                if (options.silent < 9)
                                    Console::Print(Stream::Error, CT_TEXT("The track number has to be between 1 and 255 for ID3v1.\n"));
                                return -1;
                            }
                            // V2Only: the track is stored as-is in ID3v2, so no problem here
                            if (id3TagMode == Id3TagMode::Default && options.silent < 9)
                                Console::Print(Stream::Error, CT_TEXT("The track number has to be between 1 and 255 for ID3v1, ignored for ID3v1.\n"));
                        }
                    } else if (is(CT_TEXT("tg"))) {
                        std::int32_t ret = 0;
                        argUsed          = 1;
                        if (nextArg[0] != '\0')
                            ret = Id3TagOption(tag, 'g', id3Encoding, nextArg, options.silent);
                        if (ret != 0 && ignoreTagErrors == 0) {
                            if (ret == -1) {
                                Console::Print(Stream::Error, CT_TEXT("Unknown ID3v1 genre number: '%s'.\n"), nextArg);
                                return -1;
                            }
                            if (ret == -2) {
                                if (id3TagMode == Id3TagMode::V1Only) {
                                    Console::Print(Stream::Error, CT_TEXT("Unknown ID3v1 genre: '%s'.\n"), nextArg);
                                    return -1;
                                }
                                // V2Only: the genre is stored as-is in ID3v2, so no problem here
                                if (id3TagMode == Id3TagMode::Default && options.silent < 9)
                                    Console::Print(Stream::Error, CT_TEXT("Unknown ID3v1 genre: '%s'.  Setting ID3v1 genre to 'Other'\n"), nextArg);
                            } else {
                                if (options.silent < 10)
                                    Console::Print(Stream::Error, CT_TEXT("Internal error.\n"));
                                return -1;
                            }
                        }
                    } else if (is(CT_TEXT("tv"))) {
                        argUsed = 1;
                        if (Id3TagOption(tag, 'v', id3Encoding, nextArg, options.silent) != 0 && options.silent < 9)
                            Console::Print(Stream::Error, CT_TEXT("Invalid field value: '%s'. Ignored\n"), nextArg);
                    } else if (is(CT_TEXT("ti"))) {
                        argUsed = 1;
                        if (SetId3AlbumArt(tag, nextArg) != 0 && ignoreTagErrors == 0)
                            return -1;
                    } else if (is(CT_TEXT("ignore-tag-errors"))) {
                        ignoreTagErrors = 1;
                    } else if (is(CT_TEXT("add-id3v2"))) {
                        tag.AddV2();
                    } else if (is(CT_TEXT("id3v1-only"))) {
                        tag.V1Only();
                        id3TagMode = Id3TagMode::V1Only;
                    } else if (is(CT_TEXT("id3v2-only"))) {
                        tag.V2Only();
                        id3TagMode = Id3TagMode::V2Only;
                    } else if (is(CT_TEXT("pad-id3v2-size"))) {
                        argUsed = GetIntValue(token, nextArg, &intValue);
                        if (argUsed) {
                            intValue = intValue <= 128000 ? intValue : 128000;
                            intValue = intValue >= 0 ? intValue : 0;
                            tag.SetPad(static_cast<std::size_t>(intValue));
                        }
                    } else if (is(CT_TEXT("genre-list"))) {
                        Id3Tag::GenreList(
                            [](std::int32_t number, const char* name, void*) { Console::Print(Stream::Output, CT_TEXT("%3d %hs\n"), number, name); }, nullptr);
                        return -2;
                    } else {
                        Console::Print(Stream::Error, CT_TEXT("%s: unrecognized option --%s\n"), programName, token);
                        return -1;
                    }
                    i += argUsed;
                } else {
                    Shared::Char c;
                    while ((c = *token++) != '\0') {
                        double              doubleValue = 0;
                        std::int32_t        intValue    = 0;
                        const Shared::Char* arg         = *token ? token : nextArg;
                        switch (c) {
                            case 'm':
                                argUsed = 1;
                                switch (*arg) {
                                    case 's': (void)gfp.SetMode(Stereo); break;
                                    case 'd': (void)gfp.SetMode(DualChannel); break;
                                    case 'f':
                                        gfp.SetForceMs(1);
                                        (void)gfp.SetMode(JointStereo);
                                        break;
                                    case 'j':
                                        gfp.SetForceMs(0);
                                        (void)gfp.SetMode(JointStereo);
                                        break;
                                    case 'm': (void)gfp.SetMode(Mono); break;
                                    case 'l':
                                        (void)gfp.SetMode(Mono);
                                        (void)gfp.SetScaleLeft(2);
                                        (void)gfp.SetScaleRight(0);
                                        break;
                                    case 'r':
                                        (void)gfp.SetMode(Mono);
                                        (void)gfp.SetScaleLeft(0);
                                        (void)gfp.SetScaleRight(2);
                                        break;
                                    case 'a': // same as 'j' ???
                                        gfp.SetForceMs(0);
                                        (void)gfp.SetMode(JointStereo);
                                        break;
                                    default: Console::Print(Stream::Error, CT_TEXT("%s: -m mode must be s/d/f/j/m/l/r not %s\n"), programName, arg); return -1;
                                }
                                break;

                            case 'V':
                                argUsed = GetDoubleValue(CT_TEXT("V"), arg, &doubleValue);
                                if (argUsed)
                                    gfp.SetVbrQuality(static_cast<float>(doubleValue));
                                break;

                            case 'q':
                                argUsed = GetIntValue(CT_TEXT("q"), arg, &intValue);
                                if (argUsed)
                                    (void)gfp.SetQuality(intValue);
                                break;
                            case 'f': (void)gfp.SetQuality(7); break;
                            case 'h': (void)gfp.SetQuality(2); break;

                            case 'b':
                                argUsed = GetIntValue(CT_TEXT("b"), arg, &intValue);
                                if (argUsed) {
                                    // the unmodified LAME also set this as CBR bitrate, and its
                                    // lame_set_brate() turned the bit reservoir off above 320 kbps
                                    if (intValue > 320)
                                        gfp.SetDisableReservoir(1);
                                    gfp.SetVbrMinBitrateKbps(intValue);
                                }
                                break;
                            case 'B':
                                argUsed = GetIntValue(CT_TEXT("B"), arg, &intValue);
                                if (argUsed)
                                    gfp.SetVbrMaxBitrateKbps(intValue);
                                break;
                            case 'F': gfp.SetVbrHardMin(1); break;
                            case 't': // dont write VBR tag
                                (void)gfp.SetWriteVbrTag(0);
                                break;
                            case 'T': // do write VBR tag
                                (void)gfp.SetWriteVbrTag(1);
                                break;
                            case 'p': // error_protection: add crc16 information to stream
                                gfp.SetErrorProtection(1);
                                break;
                            case 'a': // autoconvert input file from stereo to mono - for mono mp3 encoding
                                autoconvert = 1;
                                (void)gfp.SetMode(Mono);
                                break;
                            case 'S': options.silent = 5; break;
                            case 'Y': gfp.SetExperimentalY(1); break;
                            case 'Z': {
                                // sscanf returns EOF (-1) for an empty argument, which LAME
                                // counts as a used argument
                                std::int32_t n = 1;
#if defined(_WIN32)
                                argUsed = swscanf_s(arg, L"%d", &n); // same as sscanf for %d
#else
                                argUsed = std::sscanf(arg, "%d", &n);
#endif
                                gfp.SetExperimentalZ(n);
                            } break;
                            case 'e':
                                argUsed = 1;
                                switch (*arg) {
                                    case 'n': gfp.SetEmphasis(0); break;
                                    case '5': gfp.SetEmphasis(1); break;
                                    case 'c': gfp.SetEmphasis(3); break;
                                    default: Console::Print(Stream::Error, CT_TEXT("%s: -e emp must be n/5/c not %s\n"), programName, arg); return -1;
                                }
                                break;
                            case 'c': gfp.SetCopyright(1); break;
                            case 'o': gfp.SetOriginal(0); break;

                            case '?': LongHelp(gfp, Stream::Output, programName, 0); return -1;

                            default: Console::Print(Stream::Error, CT_TEXT("%s: unrecognized option -%lc\n"), programName, c); return -1;
                        }
                        if (argUsed) {
                            if (arg == token)
                                token = CT_TEXT(""); // no more from token
                            else
                                ++i; // skip arg we used
                            argUsed = 0;
                        }
                    }
                }
            } else {
                // normal options: inputfile [outputfile]
                if (inPath[0] == '\0') {
                    CopyPadded(inPath, argv[i], PathMax + 1);
                    inputFile = 1;
                } else {
                    if (outPath[0] == '\0')
                        CopyPadded(outPath, argv[i], PathMax + 1);
                    else {
                        Console::Print(Stream::Error, CT_TEXT("%s: excess arg %s\n"), programName, argv[i]);
                        return -1;
                    }
                }
            }
        }

        if (!inputFile) {
            PrintUsage(Stream::Error, programName);
            return -1;
        }

        inPath[PathMax] = '\0'; // strncpy leaves a PATH_MAX + 1 long name unterminated
        DosToLongFileName(inPath);

        if (outPath[0] == '\0') { // no explicit output file
            if (GenerateOutPath(inPath, CT_TEXT(".mp3"), outPath) != 0)
                return -1;
        }

        // RG is enabled by default
        if (noreplaygain == 0)
            (void)gfp.SetFindReplayGain(1);

        // default guess for number of channels
        if (autoconvert)
            (void)gfp.SetNumChannels(2);
        else if (Mono == gfp.mode)
            (void)gfp.SetNumChannels(1);
        else
            (void)gfp.SetNumChannels(2);

        return 0;
    }

    void CommandLine::HelpId3Tag(Stream fp)
    {
        Console::Print(fp,
                       CT_TEXT("  ID3 tag options:\n") "    --tt <title>    audio/song title (max 30 chars for version 1 tag)\n"
                                                       "    --ta <artist>   audio/song artist (max 30 chars for version 1 tag)\n"
                                                       "    --tl <album>    audio/song album (max 30 chars for version 1 tag)\n"
                                                       "    --ty <year>     audio/song year of issue (1 to 9999)\n"
                                                       "    --tc <comment>  user-defined text (max 30 chars for v1 tag, 28 for v1.1)\n");
        Console::Print(fp,
                       CT_TEXT("    --tn <track[/total]>   audio/song track number and (optionally) the total\n") "                           number of tracks "
                                                                                                                  "on the original recording. (track\n"
                                                                                                                  "                           and total each 1 "
                                                                                                                  "to 255. just the track number\n"
                                                                                                                  "                           creates v1.1 "
                                                                                                                  "tag, providing a total forces v2.0).\n");
        Console::Print(
            fp,
            CT_TEXT("    --tg <genre>    audio/song genre (name or number in list)\n") "    --ti <file>     audio/song albumArt (jpeg/png/gif file, v2.3 tag)\n"
                                                                                       "    --tv <id=value> user-defined frame specified by id and value (v2.3 "
                                                                                       "tag)\n"
                                                                                       "                    syntax: --tv \"TXXX=description=content\"\n");
        Console::Print(
            fp,
            CT_TEXT("    --add-id3v2     force addition of version 2 tag\n") "    --id3v1-only    add only a version 1 tag\n"
                                                                             "    --id3v2-only    add only a version 2 tag\n"
                                                                             "    --id3v2-utf16   add following options in unicode UTF-16 text encoding\n"
                                                                             "    --id3v2-latin1  add following options in latin-1 text encoding\n"
                                                                             "    --pad-id3v2-size <value> adds version 2 tag, pad with extra <value> bytes\n"
                                                                             "    --genre-list    print alphabetically sorted ID3 genre list and exit\n"
                                                                             "    --ignore-tag-errors  ignore errors in values passed for tags\n"
                                                                             "\n");
        Console::Print(fp,
                       CT_TEXT("    Note: A version 2 tag will NOT be added unless one of the input fields\n") "    won't fit in a version 1 tag (e.g. the "
                                                                                                               "title string is longer than 30\n"
                                                                                                               "    characters), or the '--add-id3v2' or "
                                                                                                               "'--id3v2-only' options are used.\n"
                                                                                                               "    Unicode text (the default, see "
                                                                                                               "'--id3v2-utf16') also adds it.\n");
    }

    std::int32_t CommandLine::Id3TagOption(Id3Tag& tag, char type, TextEncoding encoding, const Shared::Char* text, std::int32_t silent)
    {
        if (encoding == TextEncoding::Utf16 && type != 'v')
            Id3TagOption(tag, type, TextEncoding::Latin1, text, silent); // for ID3v1
        std::int32_t result = 0;
        if (encoding == TextEncoding::Latin1) {
            char* const latin1 = ToLatin1(text, silent);
            result             = SetId3Tag(tag, type, latin1);
            Shared::Heap::Free(latin1);
        } else {
            char16_t* const utf16 = ToUtf16(text, silent);
            result                = SetId3v2Tag(tag, type, utf16);
            Shared::Heap::Free(utf16);
        }
        return result;
    }

    std::int32_t CommandLine::SetId3Tag(Id3Tag& tag, char type, const char* text)
    {
        switch (type) {
            case 'a': tag.SetArtist(text); return 0;
            case 't': tag.SetTitle(text); return 0;
            case 'l': tag.SetAlbum(text); return 0;
            case 'g': return tag.SetGenre(text);
            case 'c': tag.SetComment(text); return 0;
            case 'n': return tag.SetTrack(text);
            case 'y': tag.SetYear(text); return 0;
            case 'v': return tag.SetFieldValue(text);
            default: return 0;
        }
    }

    std::int32_t CommandLine::SetId3v2Tag(Id3Tag& tag, char type, const char16_t* text)
    {
        switch (type) {
            case 'a': return tag.SetTextInfoUtf16("TPE1", text);
            case 't': return tag.SetTextInfoUtf16("TIT2", text);
            case 'l': return tag.SetTextInfoUtf16("TALB", text);
            case 'g': return tag.SetTextInfoUtf16("TCON", text);
            case 'c': return tag.SetCommentUtf16(nullptr, nullptr, text);
            case 'n': return tag.SetTextInfoUtf16("TRCK", text);
            case 'y': return tag.SetTextInfoUtf16("TYER", text);
            case 'v': return tag.SetFieldValueUtf16(text);
            default: return -3;
        }
    }

    std::int32_t CommandLine::SetId3AlbumArt(Id3Tag& tag, const Shared::Char* fileName)
    {
        if (fileName == nullptr)
            return 0;
        std::int32_t  ret  = 0;
        std::uint64_t size = 0;
        if (!Shared::File::Size(fileName, size)) {
            ret = 1;
        } else {
            void* const albumArt = Shared::Heap::Allocate(static_cast<std::size_t>(size));
            if (albumArt == nullptr)
                ret = 2;
            else {
                if (!Shared::File::Read(fileName, albumArt, static_cast<std::size_t>(size)))
                    ret = 3;
                else
                    ret = tag.SetAlbumArt(static_cast<const std::byte*>(albumArt), static_cast<std::size_t>(size)) != 0 ? 4 : 0;
                Shared::Heap::Free(albumArt);
            }
        }
        switch (ret) {
            case 1: Console::Print(Stream::Error, CT_TEXT("Could not find: '%s'.\n"), fileName); break;
            case 2: Console::Print(Stream::Error, CT_TEXT("Insufficient memory for reading the albumart.\n")); break;
            case 3: Console::Print(Stream::Error, CT_TEXT("Read error: '%s'.\n"), fileName); break;
            case 4: Console::Print(Stream::Error, CT_TEXT("Unsupported image: '%s'.\nSpecify JPEG/PNG/GIF image\n"), fileName); break;
            default: break;
        }
        return ret;
    }

#if defined(_WIN32)

    // LAME passes its arguments as UTF-8 and converts them with utf8ToLatin1 and utf8ToUtf16 (main.c); ctLame has
    // them as UTF-16 already, the result is the same.

    char* CommandLine::ToLatin1(const Shared::Char* text, [[maybe_unused]] std::int32_t silent)
    {
        // Latin-1 is code page 28591; characters outside it get the best fit or '?'
        constexpr std::uint32_t Latin1 = 28591;
        const std::int32_t      n      = 1 + WideCharToMultiByte(Latin1, 0, text, -1, nullptr, 0, nullptr, nullptr);
        auto* const             latin1 = static_cast<char*>(Shared::Heap::Allocate(static_cast<std::size_t>(n)));
        if (latin1 != nullptr && WideCharToMultiByte(Latin1, 0, text, -1, latin1, n, nullptr, nullptr) == 0) {
            Shared::Heap::Free(latin1);
            return nullptr;
        }
        return latin1;
    }

    char16_t* CommandLine::ToUtf16(const Shared::Char* text, [[maybe_unused]] std::int32_t silent)
    {
        const std::size_t length = Text::Length(text);
        auto* const       utf16  = static_cast<char16_t*>(Shared::Heap::Allocate((length + 2) * sizeof(char16_t)));
        if (utf16 != nullptr) {
            utf16[0] = 0xFEFF; // BOM
            for (std::size_t i = 0; i <= length; ++i)
                utf16[i + 1] = static_cast<char16_t>(text[i]);
        }
        return utf16;
    }

#else

    // HAVE_ICONV: iconv from the character set of the locale, as in parse.c of LAME 4.1 (toLatin1, toUtf16 and
    // convert_text), which reports text it cannot convert; 4.0 left it out silently. Characters outside the target
    // set are transliterated ("//TRANSLIT").

    char* CommandLine::ToLatin1(const Shared::Char* text, std::int32_t silent)
    {
        if (text == nullptr)
            return nullptr;
        const std::size_t l = Text::Length(text);
        return ConvertText("ISO_8859-1", text, l, l * 4, 0, silent);
    }

    char16_t* CommandLine::ToUtf16(const Shared::Char* text, std::int32_t silent)
    {
        if (text == nullptr)
            return nullptr;
        // two bytes at the front for the byte order mark, written afterwards so that it survives a conversion that
        // could not start at all
        const std::size_t l   = Text::Length(text);
        char* const       dst = ConvertText("UTF-16LE", text, l, (l + 1) * 4, 2, silent);
        if (dst != nullptr) {
            dst[0] = '\xff';
            dst[1] = '\xfe';
        }
        return reinterpret_cast<char16_t*>(dst);
    }

    char* CommandLine::ConvertText(const char* to, const char* src, std::size_t srcBytes, std::size_t room, std::size_t lead, std::int32_t silent)
    {
        auto* const dst = static_cast<char*>(Shared::Heap::Allocate((room + 4) * 4, true));
        if (dst == nullptr)
            return nullptr;
        char toTranslit[32];
        Text::Copy(toTranslit, sizeof(toTranslit), to);
        Text::Copy(toTranslit + Text::Length(to), sizeof(toTranslit) - Text::Length(to), "//TRANSLIT");
        const char* const from   = nl_langinfo(CODESET);
        const iconv_t     xiconv = iconv_open(toTranslit, from);
        if (xiconv == reinterpret_cast<iconv_t>(-1)) {
            if (silent < 9)
                Console::Print(Stream::Error, CT_TEXT("Cannot convert text from %s to %s on this system; the value is left out of the tag.\n"), from, to);
            return dst;
        }
        char*             iPtr  = const_cast<char*>(src);
        char*             oPtr  = dst + lead;
        std::size_t       srcln = srcBytes;
        std::size_t       avail = room;
        const std::size_t rc    = iconv(xiconv, &iPtr, &srcln, &oPtr, &avail);
        iconv_close(xiconv);
        // the value is echoed back because a run may set several tag fields
        if (rc == static_cast<std::size_t>(-1) && silent < 9)
            Console::Print(Stream::Error, CT_TEXT("Only part of '%s' could be written as %s; %zu byte(s) of it were dropped.\n"), src, to, srcln);
        return dst;
    }

#endif

    void CommandLine::SetProcessPriority(std::int32_t priority)
    {
        switch (priority) {
            case 0:
            case 1:
                SetPriority(Priority::Low);
                Console::Print(Stream::Error, CT_TEXT("==> Priority set to Low.\n"));
                break;
            default:
            case 2:
                SetPriority(Priority::Normal);
                Console::Print(Stream::Error, CT_TEXT("==> Priority set to Normal.\n"));
                break;
            case 3:
            case 4:
                SetPriority(Priority::High);
                Console::Print(Stream::Error, CT_TEXT("==> Priority set to High.\n"));
                break;
        }
    }

    // the process priority of setProcessPriority: a priority class on Windows, a nice value on Linux (raising it needs
    // the right to; without it the priority stays, as LAME ignores the result)
    void CommandLine::SetPriority(Priority priority)
    {
#if defined(_WIN32)
        SetPriorityClass(GetCurrentProcess(),
                         priority == Priority::Low        ? IDLE_PRIORITY_CLASS
                             : priority == Priority::High ? HIGH_PRIORITY_CLASS
                                                          : NORMAL_PRIORITY_CLASS);
#else
        setpriority(PRIO_PROCESS, 0, priority == Priority::Low ? 10 : priority == Priority::High ? -10 : 0);
#endif
    }
} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
