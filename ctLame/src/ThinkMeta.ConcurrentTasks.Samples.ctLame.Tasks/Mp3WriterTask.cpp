// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "Mp3WriterTask.h"

#include <cstdint>

import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    using Windows::Error;

    Mp3WriterTask::Mp3WriterTask(BlockPipe& pipe, const Settings& settings) noexcept : pipe_(pipe), settings_(settings)
    {
        if (settings_.writesInFlight < 1)
            settings_.writesInFlight = 1;
        if (settings_.writesInFlight > MaxWritesInFlight)
            settings_.writesInFlight = MaxWritesInFlight;
        if (settings_.writesInFlight >= pipe_.BlockCount())
            settings_.writesInFlight = pipe_.BlockCount() > 1 ? pipe_.BlockCount() - 1 : 1;
    }

    void Mp3WriterTask::Run() noexcept
    {
        TaskProfile* const profile = settings_.profile;
        const std::int64_t begin   = ProfileTaskBegin(profile);

        AsyncIo::AsyncFile file;
        std::uint32_t      error = file.Create(settings_.path, GENERIC_WRITE, FILE_SHARE_READ, settings_.creationDisposition, FILE_FLAG_SEQUENTIAL_SCAN);

        const std::uint32_t inFlight    = settings_.writesInFlight;
        std::uint64_t       issued      = 0;
        std::uint64_t       completed   = 0;
        std::uint64_t       offset      = 0;
        bool                endOfStream = false;
        TaskPriority        priority    = TaskPriority::Normal;

        auto producerFinished = [this] { return settings_.producerFinished != nullptr && ReadAcquire(settings_.producerFinished) != 0; };

        for (;;) {
            // take blocks while slots are free; wait for data only with nothing in flight, since
            // the producer may need the blocks this task still holds
            while (!endOfStream && issued - completed < inFlight) {
                BlockPipe::ReadBlock block{};
                bool                 ended = false;
                if (issued == completed) {
                    ProfileScope idle(IdleCounter(profile));
                    endOfStream = !pipe_.AcquireReadBlock(block);
                } else if (!pipe_.TryAcquireReadBlock(block, ended)) {
                    endOfStream = ended;
                    break;
                }
                if (endOfStream)
                    break;

                auto& request = requests_[issued % inFlight];
                request.Reset();
                request.SetOffset(offset);
                offset += block.size;
                const std::uint32_t result = Error::Succeeded(error) ? file.WriteAsync(block.data, block.size, request) : error;
                if (Error::Failed(result) && !Error::IsPending(result)) {
                    // keep draining the pipe, so that the producer is never stuck on a full pipe
                    error = result;
                    request.RequestCompleted(result, 0);
                }
                ++issued;
            }

            if (completed == issued) {
                if (endOfStream)
                    break;
                continue;
            }

            // blocks go back to the producer in the order they were taken
            auto& request = requests_[completed % inFlight];
            {
                ProfileScope idle(IdleCounter(profile));
                request.Wait();
            }
            ++completed;
            if (Error::Failed(request.GetError()) && Error::Succeeded(error))
                error = request.GetError();
            pipe_.ReleaseReadBlock();

            const std::uint32_t filled = pipe_.FilledBlocks();
            CountOperation(stats_, priority, filled, request.GetTransferredBytes());
            ApplyPriority(priority, settings_.policy.ForConsumer(filled, pipe_.BlockCount(), producerFinished(), priority), stats_);
        }

        if (Error::Succeeded(error) && header_ != nullptr && headerSize_ > 0) {
            unsigned long written = 0; // DWORD of the API
            error                 = file.Write(header_, headerSize_, written, 0);
        }

        file.Close();
        error_ = error;
        ProfileTaskEnd(profile, begin);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
