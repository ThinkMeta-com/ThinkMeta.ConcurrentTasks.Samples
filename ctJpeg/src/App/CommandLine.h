// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstddef>
#include <cstdint>

#include "EncoderOptions.h"
#include "ParseResult.h"
#include "SimdFeatures.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    /// <summary>The settings of the command line: cjpegli's encoder options, the paths and the ctJpeg options that cjpegli does not have.</summary>
    struct Options {
        EncoderOptions encoder;
        const wchar_t* inPath     = nullptr;
        const wchar_t* outPath    = nullptr;
        bool           quiet      = false;
        std::int32_t   threads    = 0;               // --threads n: threads of the scheduler, 0 = logical processors
        bool           sequential = false;           // --sequential: the reference path in one task
        SimdLevel      simd       = SimdLevel::Avx2; // --simd level: the feature set to use
        bool           simdGiven  = false;           // --simd was given: no fallback, the levels differ in output
        const wchar_t* dumpPath   = nullptr;         // --dump-coefficients path: write the quantized coefficients instead (check tool)
        std::int32_t   bandRows   = 0;               // --band-rows n: iMCU rows per pixel-phase band, 0 = automatic (check tool)
        std::int32_t   scanBands  = 0;               // --scan-bands n: bands per AC scan, 0 = automatic, 1 = whole scans (check tool)
        std::int32_t   stripWidth = 0;               // --strip-width n: pixel columns per pixel-phase strip, 0 = automatic (check tool)
        bool           stats      = false;           // --stats (or --times): print the phase times
        bool           features   = false;           // --features: print the CPU features and the feature set in use
    };

    /// <summary>
    /// The command line of ctJpeg: cjpegli syntax (INPUT OUTPUT, -q/--quality, -d/--distance, --chroma_subsampling,
    /// -p/--progressive_level, values as "--opt v" or "--opt=v") plus the options of every sample (--threads,
    /// --sequential, --simd, --features, --stats, --version) and --quiet.
    /// Options outside the ported scope are rejected with a message instead of being ignored. The same interface in
    /// every sample.
    /// </summary>
    class CommandLine
    {
    public:
        /// <summary>Reads the command line into options; prints the help text or the error.</summary>
        static Shared::ParseResult Parse(std::int32_t argc, wchar_t** argv, Options& options);

        /// <summary>The help text, to stdout.</summary>
        static void PrintUsage(const wchar_t* programName);

        /// <summary>The version and the copyright, to stdout.</summary>
        static void PrintVersion();

    private:
        /// <summary>Splits "--name=value" into name and value; value is nullptr without '='.</summary>
        static void SplitOption(const wchar_t* arg, wchar_t* name, std::size_t nameSize, const wchar_t*& value);

        static bool ParseInt(const wchar_t* text, std::int32_t& value);
        static bool ParseFloat(const wchar_t* text, float& value);
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
