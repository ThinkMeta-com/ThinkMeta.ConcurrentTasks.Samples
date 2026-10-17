// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <cstdint>

#include "EncoderOptions.h"
#include "SimdFeatures.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg {

    /// <summary>Frontend settings: paths and the ctJpeg options that cjpegli does not have.</summary>
    struct FrontendOptions {
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
        bool           times      = false;           // --times: print the phase times
    };

    /// <summary>
    /// Parses the command line: cjpegli syntax (INPUT OUTPUT, -q/--quality, -d/--distance,
    /// --chroma_subsampling, -p/--progressive_level, values as "--opt v" or "--opt=v") plus --simd,
    /// --threads, --sequential and --quiet. Options outside the ported scope are rejected with a message
    /// instead of being ignored. Returns 0, 1 on an error or 2 after the help text.
    /// </summary>
    std::int32_t ParseCommandLine(std::int32_t argc, wchar_t** argv, EncoderOptions& options, FrontendOptions& frontend);

    /// <summary>The help text.</summary>
    void PrintUsage(const wchar_t* programName);

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctJpeg
