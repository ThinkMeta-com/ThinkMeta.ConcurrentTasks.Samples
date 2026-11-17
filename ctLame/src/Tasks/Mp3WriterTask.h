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

#include <cstddef>
#include <cstdint>

#include "NodeProfile.h"

// headers with imports last
#include "Mp3File.h"
#include "Text.h"

import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>
    /// Node Write of the encoder graph (EncodeGraph.h): writes the MP3 file front to back, section after section as
    /// Mp3File hands them over. One write at a time; a write suspends the task, not its thread. Runs on the I/O
    /// scheduler.
    /// </summary>
    class Mp3WriterTask final : public Tasks::Task
    {
    public:
        struct Settings {
            const Shared::Char* path    = nullptr; // the file is created, or emptied
            TaskProfile*        profile = nullptr; // CTLAME_PROFILE: busy and idle time
        };

        Mp3WriterTask(Mp3File& mp3, const Settings& settings) noexcept;

        void Run() noexcept override;

        /// <summary>0 (success) or the error that ended the writing.</summary>
        std::uint32_t Error() const noexcept { return error_; }

    private:
        /// <summary>
        /// Writes the bytes at position and moves position behind them. After an error it writes nothing more; the first
        /// error stays.
        /// </summary>
        void Write(AsyncIo::AsyncFile& output, std::uint64_t& position, Mp3File::Bytes bytes) noexcept;

        Mp3File&      mp3_;
        Settings      settings_;
        std::uint32_t error_ = 0;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
