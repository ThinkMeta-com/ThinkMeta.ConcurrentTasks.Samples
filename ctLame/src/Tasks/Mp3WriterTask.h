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
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "IoPriority.h"
#include "NodeProfile.h"

// headers with imports last
#include "BlockPipe.h"

import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFile;

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    /// <summary>
    /// Node Write of the encoder graph (EncodeGraph.h): writes the blocks of a pipe
    /// to a file with several overlapped writes in flight. At the end
    /// it can write a final header at offset 0, e.g. the LAME tag frame, whose content is only
    /// known after encoding. Runs on the I/O scheduler and sets its own priority by the fill
    /// level of the pipe.
    /// </summary>
    class Mp3WriterTask : public Tasks::Task
    {
    public:
        static constexpr std::uint32_t MaxWritesInFlight = 8;

        struct Settings {
            const wchar_t*           path                = nullptr;
            std::uint32_t            creationDisposition = CREATE_ALWAYS;
            std::uint32_t            writesInFlight      = 4;       // at most MaxWritesInFlight, less than the pipe's blocks
            const std::atomic<bool>* producerFinished    = nullptr; // optional: drain with High priority once set
            FillLevelPolicy          policy;
            TaskProfile*             profile = nullptr; // CTLAME_PROFILE: busy and idle time
        };

        Mp3WriterTask(BlockPipe& pipe, const Settings& settings) noexcept;

        /// <summary>
        /// Data to write at offset 0 after the last block. The producer sets it before it ends the
        /// stream and keeps the memory alive until the task has finished.
        /// </summary>
        void SetFinalHeader(const std::byte* data, std::uint32_t size) noexcept
        {
            header_     = data;
            headerSize_ = size;
        }

        void Run() noexcept override;

        /// <summary>ERROR_SUCCESS or the error that ended the writing.</summary>
        std::uint32_t Error() const noexcept { return error_; }

        const IoStats& Stats() const noexcept { return stats_; }

    private:
        BlockPipe&       pipe_;
        Settings         settings_;
        const std::byte* header_     = nullptr;
        std::uint32_t    headerSize_ = 0;
        std::uint32_t    error_      = ERROR_SUCCESS;
        IoStats          stats_;

        std::array<AsyncIo::WaitableAsyncFileIoRequest_s, MaxWritesInFlight> requests_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
