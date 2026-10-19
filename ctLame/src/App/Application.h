// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <array>
#include <cstdint>

#include "CommandLine.h"
#include "EncoderOptions.h"
#include "RunStats.h"

// headers with imports last
#include "EncodeGraph.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    class Encoder;
    struct SessionConfig;
    class WavFile;

    /// <summary>
    /// The program ctLame (frontend/main.c and frontend/lame_main.c): the command line (ParseArgs), then either the
    /// encoding (Execute) or the end after an error or the help text (ParseError). The same three steps in every
    /// sample.
    /// </summary>
    class Application
    {
    public:
        /// <summary>Debug builds: CRT reports to stderr (DebugReports).</summary>
        Application();

        /// <summary>Reads the command line (CommandLine, parse_args). False on an error or after the help or version text.</summary>
        bool ParseArgs(std::int32_t argc, wchar_t** argv);

        /// <summary>Encodes the file (lame_encoder); returns the exit code.</summary>
        std::int32_t Execute();

        /// <summary>After a failed ParseArgs, which has printed the message or the help text: returns the exit code.</summary>
        std::int32_t ParseError();

    private:
        static constexpr std::int32_t ConsoleWidth = 80; // Console_IO.disp_width of LAME

        struct EncodeProfile;

        static HANDLE        InitOutfile(const wchar_t* outPath);
        static void          EncoderProgressBegin(const SessionConfig& config, const Options& frontend, const wchar_t* inPath, const wchar_t* outPath);
        void                 PrintProfile(const EncodeProfile& profile) const;
        static std::uint64_t MaxInputSamples(const WavFile& wavFile, const wchar_t* inPath, std::int32_t channels, std::uint64_t byteLimit);
        static bool RunEncodeGraph(Encoder& encoder, const SessionConfig& config, const Options& frontend, const WavFile& wavFile, const wchar_t* inPath,
                                   const wchar_t* outPath, EncodeGraph::Result& result, EncodeProfile* profile, Shared::RunStats& stats,
                                   const wchar_t*& failure);

        Options             options_;
        Shared::ParseResult result_ = Shared::ParseResult::Error;
        Shared::RunStats    stats_; // --stats
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
