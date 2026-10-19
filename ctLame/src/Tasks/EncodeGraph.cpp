// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "EncodeGraph.h"

#include <cstdint>
#include <memory>
#include <new>

import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    namespace {

        // Blocks of the two pipes: large enough for few I/O requests, small enough to start early.
        constexpr std::uint32_t PipeBlockCount = 16;
        constexpr std::uint32_t PipeBlockSize  = 64 * 1024;

        TaskProfile* Of(EncodeGraph::Profile* profile, TaskProfile EncodeGraph::Profile::* node) noexcept
        {
            return profile != nullptr ? &(profile->*node) : nullptr;
        }

    } // namespace

    EncodeGraph::EncodeGraph(Encoder& encoder, const SessionConfig& config, const Settings& settings, Profile* profile) noexcept
        : encoder_(encoder),
          config_(config),
          settings_(settings),
          profile_(profile)
    {}

    bool EncodeGraph::Initialize() noexcept
    {
        // the PCM pipe holds the reads in flight and as many again for the dispatcher
        const std::uint32_t pcmBlocks = settings_.readsInFlight * 2 > PipeBlockCount ? settings_.readsInFlight * 2 : PipeBlockCount;
        if (!pcm_.Initialize(pcmBlocks, settings_.readSize) || !mp3_.Initialize(PipeBlockCount, PipeBlockSize))
            return false;
        if (!stream_.Initialize(config_, settings_.maxInputSamples))
            return false;
        if (!settings_.sequential) {
            ring_.emplace();
            if (!ring_->Initialize(FrameRing::SlotsFor(settings_.threads)))
                return false;
            workerCount_ = settings_.threads;
            workers_.reset(new (std::nothrow) std::optional<WorkerTask>[workerCount_]);
            if (!workers_)
                return false;
        }
        return true;
    }

    EncodeGraph::Result EncodeGraph::Run(ITaskScheduler& io, ITaskScheduler& compute) noexcept
    {
        Profile* const profile = profile_;

        // --- nodes on the I/O scheduler: Read and Write
        WavReaderTask::Settings readSettings;
        readSettings.path          = settings_.inPath;
        readSettings.dataOffset    = settings_.dataOffset;
        readSettings.byteLimit     = settings_.byteLimit;
        readSettings.readsInFlight = settings_.readsInFlight;
        readSettings.profile       = Of(profile, &Profile::read);
        read_.emplace(pcm_, readSettings);

        // (the writer's producerFinished stays unset: Encode writes the MP3 as it goes, the last flush is small)
        Mp3WriterTask::Settings writeSettings;
        writeSettings.path    = settings_.outPath;
        writeSettings.profile = Of(profile, &Profile::write);
        write_.emplace(mp3_, writeSettings);

        // --- nodes on the compute scheduler; the encode node's profile also counts its waiting for the MP3 pipe
        frameEncoder_.emplace(mp3_, encoder_, *write_, Of(profile, &Profile::encode));
        if (settings_.sequential) {
            sequential_.emplace(pcm_, stream_, settings_.format, encoder_, *frameEncoder_, Of(profile, &Profile::encode));
        } else {
            dispatch_.emplace(pcm_, stream_, settings_.format, *ring_, Of(profile, &Profile::dispatch));
            for (std::uint32_t i = 0; i < workerCount_; ++i)
                workers_[i].emplace(*ring_, encoder_, Of(profile, &Profile::worker));
            resolve_.emplace(*ring_, encoder_, Of(profile, &Profile::resolve));
            encode_.emplace(*ring_, stream_, *frameEncoder_, Of(profile, &Profile::encode));
        }

        // --- start: all nodes of the graph in one group, the later stages first with High priority
        Synchronization::TaskGroup_s group; // one waiter: the caller
        group.AddTask(&*read_);
        group.AddTask(&*write_);
        if (sequential_)
            group.AddTask(&*sequential_);
        if (dispatch_)
            group.AddTask(&*dispatch_);
        if (resolve_)
            group.AddTask(&*resolve_);
        if (encode_)
            group.AddTask(&*encode_);
        for (std::uint32_t i = 0; i < workerCount_; ++i)
            group.AddTask(&*workers_[i]);
        if (profile != nullptr)
            profile->start = ProfileNow();

        io.AddTask(&*read_, TaskPriority::Normal);
        io.AddTask(&*write_, TaskPriority::Normal);
        if (encode_)
            compute.AddTask(&*encode_, TaskPriority::High);
        if (resolve_)
            compute.AddTask(&*resolve_, TaskPriority::High);
        if (sequential_)
            compute.AddTask(&*sequential_, TaskPriority::Normal);
        if (dispatch_)
            compute.AddTask(&*dispatch_, TaskPriority::Normal);
        for (std::uint32_t i = 0; i < workerCount_; ++i)
            compute.AddTask(&*workers_[i], TaskPriority::Normal);
        group.Wait();
        if (profile != nullptr)
            profile->end = ProfileNow();

        Result result;
        result.readError   = read_->Error();
        result.writeError  = write_->Error();
        result.encodeError = frameEncoder_->Error();
        return result;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
