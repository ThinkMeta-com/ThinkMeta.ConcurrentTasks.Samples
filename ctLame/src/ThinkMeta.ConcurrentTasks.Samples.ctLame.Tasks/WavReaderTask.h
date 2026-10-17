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

#include "IoPriority.h"
#include "Profiling.h"

// headers with imports last
#include "BlockPipe.h"

import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    /// <summary>
    /// Node Read of the encoder graph (EncodeGraph.h): reads the sample data of a
    /// WAV file into a pipe with several overlapped reads in flight. Runs on the I/O scheduler
    /// and sets its own priority by the fill level of the pipe.
    /// </summary>
    class WavReaderTask : public Tasks::Task
    {
    public:
        static constexpr std::uint32_t MaxReadsInFlight = 16;

        struct Settings {
            const wchar_t*  path          = nullptr;
            std::uint64_t   dataOffset    = 0;     // first byte of the sample data
            std::uint64_t   byteLimit     = ~0ull; // bytes to read at most (the data chunk size)
            std::uint32_t   readsInFlight = 4;     // at most MaxReadsInFlight, less than the pipe's blocks
            std::uint32_t   firstReadSize = 0;     // 0: every read fills a block; else the first read, doubled per read up to the block size
            FillLevelPolicy policy;
            TaskProfile*    profile = nullptr; // CTLAME_PROFILE: busy and idle time
        };

        WavReaderTask(BlockPipe& pipe, const Settings& settings) noexcept;

        void Run() noexcept override;

        /// <summary>ERROR_SUCCESS or the error that ended the reading.</summary>
        std::uint32_t Error() const noexcept { return error_; }

        /// <summary>Set once the last block is in the pipe (or reading failed).</summary>
        const volatile long* FinishedFlag() const noexcept { return &finished_; }

        const IoStats& Stats() const noexcept { return stats_; }

    private:
        BlockPipe&    pipe_;
        Settings      settings_;
        std::uint32_t error_    = ERROR_SUCCESS;
        volatile long finished_ = 0;
        IoStats       stats_;

        std::array<AsyncIo::WaitableAsyncFileIoRequest_s, MaxReadsInFlight> requests_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
