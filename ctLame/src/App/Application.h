// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#if defined(_WIN32)
#    include <Windows.h>
#endif

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <array>
#include <cstdint>

#include "CommandLine.h"
#include "EncoderOptions.h"
#include "RunStats.h"

// headers with imports last
#include "EncodeGraph.h"
#include "Text.h"

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
        bool ParseArgs(std::int32_t argc, Shared::Char** argv);

        /// <summary>Encodes the file (lame_encoder); returns the exit code.</summary>
        std::int32_t Execute();

        /// <summary>After a failed ParseArgs, which has printed the message or the help text: returns the exit code.</summary>
        std::int32_t ParseError();

    private:
        static constexpr std::int32_t ConsoleWidth = 80; // Console_IO.disp_width of LAME

        struct EncodeProfile;

        static bool InitOutfile(const Shared::Char* outPath);
        static void EncoderProgressBegin(const SessionConfig& config, const Options& frontend, const Shared::Char* inPath, const Shared::Char* outPath);
        void        PrintProfile(const EncodeProfile& profile) const;
        static std::uint64_t MaxInputSamples(const WavFile& wavFile, const Shared::Char* inPath, std::int32_t channels, std::uint64_t byteLimit);
        static bool          RunEncodeGraph(Encoder& encoder, const SessionConfig& config, const Options& frontend, const Id3Bytes& id3, const WavFile& wavFile,
                                            const Shared::Char* inPath, const Shared::Char* outPath, EncodeGraph::Result& result, EncodeProfile* profile,
                                            Shared::RunStats& stats, const Shared::Char*& failure);

        Options             options_;
        Shared::ParseResult result_ = Shared::ParseResult::Error;
        Shared::RunStats    stats_; // --stats
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
