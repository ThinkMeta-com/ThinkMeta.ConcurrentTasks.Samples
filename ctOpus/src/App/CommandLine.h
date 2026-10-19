// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>
#include <ctime>

#include "HeapArray.h"
#include "OpusDefines.h"
#include "OpusHeaders.h"
#include "ParseResult.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    /// <summary>The settings of the command line (the variables of opusenc.c main).</summary>
    struct Options {
        // the arguments as UTF-8 like opusenc (WIN_UNICODE), the originals for the file names
        HeapArray<char*> argvUtf8;
        HeapArray<char>  argvStorage; // the strings of argvUtf8
        wchar_t**        argvWide = nullptr;
        std::int32_t     inIndex  = 0; // the file arguments
        std::int32_t     outIndex = 0;

        bool         quiet          = false;
        std::int32_t bitrate        = -1;
        std::int32_t frameSize      = 960;
        std::int32_t opusFrameParam = OPUS_FRAMESIZE_20_MS;
        bool         withHardCbr    = false;
        bool         withCvbr       = false;
        std::int32_t signalType     = OPUS_AUTO;
        std::int32_t expectLoss     = 0;
        std::int32_t complexity     = 10;
        std::int32_t downmix        = 0;
        bool         noPhaseInv     = false;
        std::int32_t maxOggDelay    = 48000;
        std::int32_t commentPadding = 512;
        bool         ignoreLength   = false;
        bool         rawMode        = false;
        std::int32_t threads        = 0;
        bool         sequential     = false;
        bool         stats          = false; // --stats: times, thread count and peak memory after encoding
        std::int32_t optCtls        = 0;
        std::int32_t optCtlValues[3 * 64];
        std::int32_t serialNo    = 0;
        std::time_t  startTime   = 0;
        const char*  opusVersion = nullptr;
        OpusTags     comments; // with ENCODER, ENCODER_OPTIONS and the metadata options
    };

    /// <summary>
    /// The command line of ctOpus: opusenc's (opus-tools 0.2 opusenc.c) with getopt_long semantics (permutation, prefixes,
    /// "--opt=value", "--"), the same messages and exit codes, plus --threads, --sequential and --stats. Not supported:
    /// --picture, --save-range and raw input. The same interface in every sample.
    /// </summary>
    class CommandLine
    {
    public:
        /// <summary>
        /// Reads the command line into options; prints the help, version or error texts. An invalid option value ends the
        /// process with exit code 1, like opusenc's fatal().
        /// </summary>
        static Shared::ParseResult Parse(int argc, wchar_t** argv, Options& options);

        /// <summary>The help text, to stdout.</summary>
        static void PrintUsage();

    private:
        // The strings opusenc and libopusenc write into the stream (OpusTags); kept for bit identity.
        static constexpr const char* PackageName       = "opus-tools";
        static constexpr const char* PackageVersion    = "0.2";
        static constexpr const char* LibopusencVersion = "libopusenc 0.2.1";

        static constexpr std::int32_t EncoderStringSize = 1024;

        struct LongOption {
            const char* name;
            bool        hasArgument;
        };
        static const LongOption LongOptions[];

        struct ParsedOption;
        class OptionParser;

        static void PrintVersion(const char* opusVersion);
        static bool IsValidCtl(std::int32_t request) noexcept;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
