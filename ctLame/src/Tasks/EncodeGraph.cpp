// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "EncodeGraph.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <new>

#include "SessionConfig.h"

import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    namespace {

        // The PCM pipe: a few blocks of the read size, so that Read stays ahead of Dispatch.
        constexpr std::uint32_t PcmBlockCount = 4;
        // The MP3 stream: small blocks, so that the file starts early.
        constexpr std::uint32_t Mp3BlockCount = 16;
        constexpr std::uint32_t Mp3BlockSize  = 64 * 1024;

        // The profile of a node (CTLAME_PROFILE), or null without profiling.
        TaskProfile* ProfileOf(EncodeGraph::Profile* profile, TaskProfile EncodeGraph::Profile::* node) noexcept
        {
            return profile != nullptr ? &(profile->*node) : nullptr;
        }

    } // namespace

    EncodeGraph::EncodeGraph(Encoder& encoder, const SessionConfig& config, const Settings& settings, Profile* profile) noexcept
        : encoder_(encoder),
          config_(config),
          settings_(settings),
          profile_(profile),
          mp3_(settings.id3)
    {}

    bool EncodeGraph::Initialize() noexcept
    {
        if (!pcm_.Initialize(PcmBlockCount, settings_.readSize) || !mp3_.Initialize(Mp3BlockCount, Mp3BlockSize))
            return false;
        // ReplayGain: in the sequential graph the stream analyzes it itself, else Gain tasks do
        const auto replayGainBy = settings_.sequential ? SharedSampleStream::ReplayGainBy::Stream : SharedSampleStream::ReplayGainBy::Tasks;
        if (!stream_.Initialize(config_, settings_.maxInputSamples, replayGainBy, encoder_.Simd()))
            return false;
        if (!settings_.sequential) {
            ring_.emplace();
            if (!ring_->Initialize(FrameRing::SlotsFor(settings_.threads)))
                return false;
            stageTasks_ = settings_.threads;
            prepares_.reset(new (std::nothrow) std::optional<PrepareTask>[stageTasks_]);
            searches_.reset(new (std::nothrow) std::optional<SearchTask>[stageTasks_]);
            if (!prepares_ || !searches_)
                return false;
        }
        nodes_.reset(new (std::nothrow) Node[MaxNodes(stageTasks_)]);
        if (!nodes_)
            return false;

        // CTLAME_PROFILE: each edge counts the time its nodes wait in it (nothing without a profile)
        pcm_.MeasureWaits(ProfileOf(profile_, &Profile::read), ProfileOf(profile_, settings_.sequential ? &Profile::encode : &Profile::dispatch));
        stream_.MeasureWaits(ProfileOf(profile_, &Profile::gain), ProfileOf(profile_, &Profile::encode));
        if (ring_)
            ring_->MeasureWaits(ProfileOf(profile_, &Profile::dispatch), ProfileOf(profile_, &Profile::prepare), ProfileOf(profile_, &Profile::resolve),
                                ProfileOf(profile_, &Profile::search), ProfileOf(profile_, &Profile::encode));
        mp3_.MeasureWaits(ProfileOf(profile_, &Profile::encode), ProfileOf(profile_, &Profile::write));
        return true;
    }

    EncodeGraph::Nodes EncodeGraph::CreateNodes() noexcept
    {
        assert(nodes_ != nullptr && nodeCount_ == 0); // after Initialize, and once

        // The nodes in the order of the diagram. Each line says where the node runs and with which priority: Read and
        // Write on the I/O scheduler; the stages behind Prepare with High priority, so that the frames in the ring are
        // finished before new ones come in.
        WavReaderTask::Settings readSettings;
        readSettings.path       = settings_.inPath;
        readSettings.dataOffset = settings_.dataOffset;
        readSettings.byteLimit  = settings_.byteLimit;
        readSettings.profile    = ProfileOf(profile_, &Profile::read);
        read_.emplace(pcm_, readSettings);
        Add(&*read_, RunsOn::Io, TaskPriority::Normal);

        frameEncoder_.emplace(mp3_, encoder_);
        if (settings_.sequential) {
            sequential_.emplace(pcm_, stream_, settings_.format, encoder_, *frameEncoder_, ProfileOf(profile_, &Profile::encode));
            Add(&*sequential_, RunsOn::Compute, TaskPriority::Normal);
        } else {
            dispatch_.emplace(pcm_, stream_, settings_.format, *ring_, ProfileOf(profile_, &Profile::dispatch));
            Add(&*dispatch_, RunsOn::Compute, TaskPriority::Normal);
            for (std::uint32_t i = 0; i < stageTasks_; ++i) {
                prepares_[i].emplace(*ring_, encoder_, ProfileOf(profile_, &Profile::prepare));
                Add(&*prepares_[i], RunsOn::Compute, TaskPriority::Normal);
            }
            resolve_.emplace(*ring_, encoder_, ProfileOf(profile_, &Profile::resolve));
            Add(&*resolve_, RunsOn::Compute, TaskPriority::High);
            for (std::uint32_t i = 0; i < stageTasks_; ++i) {
                searches_[i].emplace(*ring_, encoder_, ProfileOf(profile_, &Profile::search));
                Add(&*searches_[i], RunsOn::Compute, TaskPriority::High);
            }
            encode_.emplace(*ring_, stream_, *frameEncoder_, ProfileOf(profile_, &Profile::encode));
            Add(&*encode_, RunsOn::Compute, TaskPriority::High);
            for (std::int32_t ch = 0; ch < stream_.GainReaderCount(); ++ch) {
                gains_[ch].emplace(stream_, ch, ProfileOf(profile_, &Profile::gain));
                Add(&*gains_[ch], RunsOn::Compute, TaskPriority::Normal);
            }
        }

        Mp3WriterTask::Settings writeSettings;
        writeSettings.path    = settings_.outPath;
        writeSettings.profile = ProfileOf(profile_, &Profile::write);
        write_.emplace(mp3_, writeSettings);
        Add(&*write_, RunsOn::Io, TaskPriority::High); // the last stage: what is encoded leaves before more is read

        return {nodes_.get(), nodeCount_};
    }

    EncodeGraph::Result EncodeGraph::Run(ITaskScheduler& io, ITaskScheduler& compute) noexcept
    {
        const Nodes nodes = CreateNodes();

        // all nodes in one group, counted in before the first one starts
        Synchronization::TaskGroup_s group; // one waiter: the caller
        for (const Node& node : nodes)
            group.AddTask(node.task);
        if (profile_ != nullptr)
            profile_->start = ProfileNow();
        for (const Node& node : nodes)
            (node.runsOn == RunsOn::Io ? io : compute).AddTask(node.task, node.priority);
        group.Wait();
        if (profile_ != nullptr)
            profile_->end = ProfileNow();
        return Errors();
    }

    EncodeGraph::Result EncodeGraph::Errors() const noexcept
    {
        Result result;
        result.readError   = read_->Error();
        result.writeError  = write_->Error();
        result.encodeError = frameEncoder_->Error();
        return result;
    }

    // --- private

    void EncodeGraph::Add(ITask* task, RunsOn runsOn, TaskPriority priority) noexcept
    {
        assert(nodeCount_ < MaxNodes(stageTasks_));
        nodes_[nodeCount_++] = {task, runsOn, priority};
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
