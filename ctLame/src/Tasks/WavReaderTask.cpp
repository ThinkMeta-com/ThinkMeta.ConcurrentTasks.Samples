// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "WavReaderTask.h"

#include <cstddef>
#include <cstdint>

import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    using Windows::Error;

    WavReaderTask::WavReaderTask(BlockPipe& pipe, const Settings& settings) noexcept : pipe_(pipe), settings_(settings)
    {
        if (settings_.readsInFlight < 1)
            settings_.readsInFlight = 1;
        if (settings_.readsInFlight > MaxReadsInFlight)
            settings_.readsInFlight = MaxReadsInFlight;
        if (settings_.readsInFlight >= pipe_.BlockCount())
            settings_.readsInFlight = pipe_.BlockCount() > 1 ? pipe_.BlockCount() - 1 : 1;
    }

    void WavReaderTask::Run() noexcept
    {
        TaskProfile* const profile = settings_.profile;
        const ProfiledRun  run(profile);

        AsyncIo::AsyncFile file;
        std::uint32_t      error = file.Create(settings_.path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN);

        std::uint64_t total = 0;
        if (Error::Succeeded(error)) {
            AsyncIo::AsyncFile::FileSize size = 0;
            error                             = file.GetSize(size);
            total                             = size > settings_.dataOffset ? size - settings_.dataOffset : 0;
            if (total > settings_.byteLimit)
                total = settings_.byteLimit;
        }

        const std::uint32_t blockSize = pipe_.BlockSize();
        const std::uint32_t inFlight  = settings_.readsInFlight;
        const std::uint32_t readSize  = blockSize;
        std::uint64_t       position  = 0; // the file offset of the next read, from the start of the data
        std::uint64_t       issued    = 0;
        std::uint64_t       completed = 0;
        TaskPriority        priority  = TaskPriority::Normal;
        if (!Error::Succeeded(error))
            total = 0;

        ApplyPriority(priority, settings_.policy.ForProducer(pipe_.FilledBlocks(), pipe_.BlockCount(), priority), stats_);

        while (completed < issued || (Error::Succeeded(error) && position < total)) {
            // issue reads while blocks are free; wait for space only with nothing in flight,
            // since the consumer may need the blocks this task still holds
            while (Error::Succeeded(error) && position < total && issued - completed < inFlight) {
                std::byte* buffer;
                if (issued == completed) {
                    buffer = WhileIdle(profile, [&] { return pipe_.AcquireWriteBlock(); });
                } else {
                    buffer = pipe_.TryAcquireWriteBlock();
                }
                if (buffer == nullptr)
                    break;

                const std::uint32_t bytes   = static_cast<std::uint32_t>(total - position < readSize ? total - position : readSize);
                auto&               request = requests_[issued % inFlight];
                request.Reset();
                request.SetOffset(settings_.dataOffset + position);
                const std::uint32_t result = file.ReadAsync(buffer, bytes, request);
                if (Error::Failed(result) && !Error::IsPending(result)) {
                    error = result;
                    break;
                }
                position += bytes;
                ++issued;
            }

            if (completed == issued)
                break;

            // the oldest read goes into the pipe first
            auto& request = requests_[completed % inFlight];
            WhileIdle(profile, [&] { request.Wait(); });
            ++completed;
            if (Error::Failed(request.GetError())) {
                if (Error::Succeeded(error))
                    error = request.GetError();
                continue; // let the reads still in flight finish
            }
            if (Error::Succeeded(error)) {
                pipe_.CommitWriteBlock(request.GetTransferredBytes());
                const std::uint32_t filled = pipe_.FilledBlocks();
                CountOperation(stats_, priority, filled, request.GetTransferredBytes());
                ApplyPriority(priority, settings_.policy.ForProducer(filled, pipe_.BlockCount(), priority), stats_);
            }
        }

        file.Close();
        error_ = error;
        finished_.store(true);
        pipe_.SetEndOfStream();
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
