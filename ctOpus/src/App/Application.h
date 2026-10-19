// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdarg>
#include <cstdint>
#include <ctime>

#include "CommandLine.h"
#include "RunStats.h"

// headers with imports last
#include "EncodeGraph.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    class OggOpusEncoder;
    class Shared::SchedulerHandle;
    class WavInput;

    /// <summary>
    /// Port of opusenc's main() (opus-tools 0.2 opusenc.c) for WAV input: the command line (ParseArgs), then either the
    /// encoding (Execute) or the end after an error or the help text (ParseError). The same three steps in every sample.
    /// Same options, messages, exit codes and output as opusenc. Not supported: --picture, --save-range and
    /// raw/AIFF/FLAC input.
    /// </summary>
    class Application
    {
    public:
        /// <summary>Debug builds: CRT reports to stderr (DebugReports).</summary>
        Application();

        /// <summary>Reads the command line (CommandLine). False on an error or after the help or version text.</summary>
        bool ParseArgs(int argc, wchar_t** argv);

        /// <summary>Encodes the file; returns the exit code. A failure ends the process with exit code 1 (fatal()).</summary>
        int Execute();

        /// <summary>After a failed ParseArgs, which has printed the message or the help text: returns the exit code.</summary>
        int ParseError();

    private:
        struct EncData;
        struct Progress;

        static void         ReportToStderr(const wchar_t* format, std::va_list args);
        static void         PrintTime(double seconds);
        static void         PrintOpenError(const char* name, unsigned long error);
        static bool         WriteCallback(void* user, const std::uint8_t* data, std::int32_t length);
        static bool         CloseCallback(void* user);
        static void         PacketCallback(void* user, const std::uint8_t* packet, std::int32_t length);
        static void         ShowProgress(Progress& p);
        static void         ShowProgressCallback(void* user);
        static std::int32_t RunEncodeGraph(WavInput& wav, OggOpusEncoder& encoder, const EncodeGraph::Settings& settings, bool sequential, float* buffer,
                                           std::int32_t frameSize, ITaskScheduler& io);

        Options             options_;
        Shared::ParseResult result_ = Shared::ParseResult::Error;
        Shared::RunStats    stats_; // --stats
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
