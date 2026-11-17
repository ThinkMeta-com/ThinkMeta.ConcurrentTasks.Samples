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

#include <cstdint>

#include "NodeProfile.h"

// headers with imports last
#include "BlockPipe.h"
#include "Text.h"

import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>
    /// Node Read of the encoder graph (EncodeGraph.h): reads the sample data of a WAV file into a pipe, block after
    /// block. One read at a time; a read suspends the task, not its thread. Runs on the I/O scheduler.
    /// </summary>
    class WavReaderTask final : public Tasks::Task
    {
    public:
        struct Settings {
            const Shared::Char* path       = nullptr;
            std::uint64_t       dataOffset = 0;       // first byte of the sample data
            std::uint64_t       byteLimit  = ~0ull;   // bytes to read at most (the data chunk size)
            TaskProfile*        profile    = nullptr; // CTLAME_PROFILE: busy and idle time
        };

        WavReaderTask(BlockPipe& pcm, const Settings& settings) noexcept;

        void Run() noexcept override;

        /// <summary>0 (success) or the error that ended the reading.</summary>
        std::uint32_t Error() const noexcept { return error_; }

    private:
        BlockPipe&    pcm_;
        Settings      settings_;
        std::uint32_t error_ = 0;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
