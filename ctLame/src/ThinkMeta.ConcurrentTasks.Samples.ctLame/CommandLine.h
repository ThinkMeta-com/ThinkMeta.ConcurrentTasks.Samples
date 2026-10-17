// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <cstdint>

#include "Console.h"
#include "EncoderOptions.h"
#include "SimdFeatures.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    /// <summary>PATH_MAX of the LAME frontend; path buffers hold PATH_MAX + 1 characters.</summary>
    inline constexpr std::int32_t PathMax = 1024;

    /// <summary>Frontend settings (ReaderConfig, WriterConfig and UiConfig of frontend/main.h).</summary>
    struct FrontendOptions {
        std::int32_t swapChannel     = 0;
        std::int32_t ignoreWavLength = 0;
        std::int32_t flushWrite      = 0;
        std::int32_t silent          = 0; // verbosity: 10 --quiet, 9 --silent, 5 -S, -5 --brief, -10 --verbose

        // ctLame options, not in LAME (and not in its help texts)
        std::int32_t threads    = 0;               // --threads n: threads of the compute scheduler, 0 = logical processors
        std::int32_t sequential = 0;               // --sequential: the encoder as one task, the reference path
        SimdLevel    simd       = SimdLevel::Avx2; // --simd level: the highest SIMD level to use
        std::int32_t features   = 0;               // --features: print the CPU features and the SIMD level
    };

    /// <summary>
    /// Port of parse_args (frontend/parse.c): merges LAMEOPT into the arguments and sets the
    /// encoder and frontend options. argv[0] is the program name. inPath and outPath are buffers
    /// of PathMax + 1 characters. Returns 0, -1 on an error (exit code 1) or -2 after help or
    /// version output (exit code 0).
    /// </summary>
    std::int32_t ParseArgs(EncoderOptions& options, FrontendOptions& frontend, std::int32_t argc, wchar_t** argv, wchar_t* inPath, wchar_t* outPath);

    /// <summary>Port of usage: the short usage message.</summary>
    std::int32_t Usage(Stream fp, const wchar_t* programName);

    /// <summary>Port of display_bitrates.</summary>
    std::int32_t DisplayBitrates(Stream fp);

    /// <summary>Port of setProcessPriority (Windows).</summary>
    void SetProcessPriority(std::int32_t priority);

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
