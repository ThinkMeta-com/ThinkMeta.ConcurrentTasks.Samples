// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <cstddef>
#include <cstdint>

#include "CommandLine.h"
#include "EncoderOptions.h"
#include "RunStats.h"
#include "SimdFeatures.h"

import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg {

    class AsyncInput;
    class ByteBuffer;
    class Image;
    struct PhaseTimes;

    /// <summary>
    /// The program ctJpeg: the command line (ParseArgs), then either the encoding (Execute) or the end after an error
    /// or the help text (ParseError). The same three steps in every sample.
    /// </summary>
    class Application
    {
    public:
        /// <summary>Debug builds: CRT reports to stderr (DebugReports).</summary>
        Application();

        /// <summary>Reads the command line (CommandLine). False on an error or after the help text.</summary>
        bool ParseArgs(std::int32_t argc, wchar_t** argv);

        /// <summary>Encodes the image; returns the exit code.</summary>
        std::int32_t Execute();

        /// <summary>After a failed ParseArgs, which has printed the message or the help text: returns the exit code.</summary>
        std::int32_t ParseError();

    private:
        /// <summary>The input arrives in steps of 1 MB: a Pixel tile wakes when the step holding its last row is in.</summary>
        static constexpr std::size_t ReadStep = std::size_t{1} << 20;

        static int  DumpCoefficients(const Image& image, const EncoderOptions& options, SimdLevel simd, const wchar_t* path);
        static bool StartRead(AsyncInput& input, ITaskScheduler& io, Synchronization::TaskGroup_s& ioGroup, const Options& frontend, Image& image,
                              wchar_t* error, std::int32_t errorSize);
        static void PrintTimes(const PhaseTimes& times, double header, double schedulerStart, double encode, double endOfIo);

        Shared::RunStats    stats_; // --stats
        Shared::ParseResult result_ = Shared::ParseResult::Error;
        Options             options_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Jpeg
