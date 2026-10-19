// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "EncodeGraph.h"

#include <cstdint>
#include <new>

#include "OpusMultistreamEncoder.h"
#include "OpusStreamEncoder.h"

import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    namespace {

        // The block ring: two slots per compute thread and four more, at least 8 and at most 64 (a slot is
        // BlockFrames frames of every channel).
        constexpr std::int64_t MinRingSlots = 8;
        constexpr std::int64_t MaxRingSlots = 64;

        // Which helper nodes the graph gets, measured on 8 cores / 16 threads at --comp 10: the theta helper from 3
        // threads (it spins and needs a thread to spare); the speculation of the frames ahead from 7 threads with one
        // level and from 8 as a cascade of three levels (with fewer threads it costs more than it gains); the levels'
        // own theta helpers from 12 threads (below, they cost more than they gain) or with a single level.
        constexpr std::uint32_t ThetaHelperThreads    = 3;
        constexpr std::uint32_t OneLevelThreads       = 7;
        constexpr std::uint32_t CascadeThreads        = 8;
        constexpr std::uint32_t LevelHelperThreads    = 12;
        constexpr std::int32_t  ThetaTrialComplexity  = 8; // CELT tries a second theta only from this complexity (QuantAllBands)

        // The input store's reservation for an input of unknown length (2^31 frames of address space, committed as
        // the input arrives).
        constexpr std::int64_t UnknownLengthFrames = 1ll << 31;

        // The input store's reservation: the samples (or a large reservation when the length is
        // unknown), the drain extension and zeros.
        std::int64_t Capacity(const WavInput& wav, std::int32_t filterLen, std::int32_t drainFrames) noexcept
        {
            const std::int64_t total   = wav.GetInfo().totalSamplesPerChannel;
            const std::int64_t samples = total > 0 ? total + PcmFeed::ReadFrames : UnknownLengthFrames;
            return filterLen + samples + LpcExtensionLength + 4ll * drainFrames + 2ll * filterLen + PcmFeed::CommitMargin + BlockRing::BlockFrames;
        }

    } // namespace

    EncodeGraph::EncodeGraph(WavInput& wav, OggOpusEncoder& encoder, const Settings& settings) noexcept
        : wav_(wav),
          encoder_(encoder),
          settings_(settings),
          threads_(settings.threads > 0 ? settings.threads : GetActiveProcessorCount(ALL_PROCESSOR_GROUPS))
    {}

    bool EncodeGraph::Initialize() noexcept
    {
        pcm_.channels    = wav_.GetInfo().channels;
        pcm_.drainFrames = encoder_.DrainFrames(settings_.lookahead);
        // input at 48 kHz goes through the same graph, with a copy instead of the resampler
        pcm_.resampled = encoder_.Resamples();
        if (!pcm_.resampled)
            pcm_.resampler.InitializeIdentity();
        else if (!pcm_.resampler.Initialize(encoder_.GetResampler()))
            return false;
        pcm_.filterLen = pcm_.resampler.FilterLength();
        if (!pcm_.input.Initialize(pcm_.channels, Capacity(wav_, pcm_.filterLen, pcm_.drainFrames))
            || !pcm_.input.Commit(pcm_.filterLen + PcmFeed::CommitMargin))
            return false;
        // the analysis chain, if a stream analyses
        chained_ = chain_.Initialize(encoder_, settings_.lookahead);
        return ring_.Initialize(MinI64(MaxRingSlots, MaxI64(MinRingSlots, 2ll * threads_ + 4)), pcm_.channels, chained_);
    }

    std::int32_t EncodeGraph::Run(SchedulerHandle& io, SchedulerHandle& compute) noexcept
    {
        const std::uint32_t     threads = threads_;
        OpusMultistreamEncoder& ms      = encoder_.Multistream();
        const std::int32_t      streams = ms.NbStreams();

        // --- which nodes the graph has (the thresholds above)
        const bool         helped     = threads >= ThetaHelperThreads && settings_.complexity >= ThetaTrialComplexity && theta_.Valid();
        const bool         fronts     = chained_ && chain_.HasFronts();
        const std::int32_t levels     = threads >= CascadeThreads ? SpecLevels : threads >= OneLevelThreads ? 1 : 0;
        const bool         specHelped = helped && (threads >= LevelHelperThreads || levels == 1);
        HeapArray<CeltSpeculation*> specs; // per level and stream
        const bool                  speculate = chained_ && levels >= 1 && levels <= SpecLevels && specs.Allocate(static_cast<std::size_t>(levels * streams));

        // --- nodes, in the order the data flows
        resamplers_.reset(new (std::nothrow) std::optional<ResampleTask>[threads]);
        if (!resamplers_)
            return OpeAllocFail;
        resamplerCount_ = threads;
        read_.emplace(wav_, pcm_, ring_);
        for (std::uint32_t i = 0; i < resamplerCount_; ++i)
            resamplers_[i].emplace(pcm_, ring_);
        EncodeTask::Served served;
        if (chained_) {
            served.chain = &chain_;
            analyse_.emplace(chain_, ring_);
            chain_.Attach(encoder_, true);
        }
        if (fronts) {
            signal_.emplace(chain_, ring_);
            front_.emplace(chain_, ring_);
        }
        if (speculate) {
            for (std::int32_t l = 0; l < levels; ++l) {
                const bool levelHelped = specHelped && specThetas_[l].Valid();
                for (std::int32_t s = 0; s < streams; ++s) {
                    CeltSpeculation* spec                            = new (std::nothrow) CeltSpeculation;
                    specs[static_cast<std::size_t>(l * streams + s)] = spec;
                    if (spec == nullptr)
                        continue;
                    spec->runner      = &speculations_[l];
                    spec->trialRunner = levelHelped ? &specThetas_[l] : nullptr;
                    if (l > 0 && specs[static_cast<std::size_t>((l - 1) * streams + s)] != nullptr)
                        specs[static_cast<std::size_t>((l - 1) * streams + s)]->cascade = spec;
                }
                served.speculations[l] = &speculations_[l];
                speculate_[l].emplace(speculations_[l]);
                if (levelHelped) {
                    served.specThetas[l] = &specThetas_[l];
                    specTheta_[l].emplace(specThetas_[l]);
                }
            }
            for (std::int32_t s = 0; s < streams; ++s)
                ms.Stream(s)->SetSpeculation(specs[static_cast<std::size_t>(s)]);
        }
        if (helped) {
            served.theta = &theta_;
            for (std::int32_t s = 0; s < streams; ++s)
                ms.Stream(s)->SetThetaTrialRunner(&theta_);
            thetaTask_.emplace(theta_);
        }
        encode_.emplace(encoder_, pcm_, ring_, served, settings_.progress);

        // --- start: all nodes in one group; Encode, the long serial stage, first, then its helpers
        // and the analysis chain with High priority, the resamplers last
        Synchronization::TaskGroup group;
        group.AddTask(&*read_);
        group.AddTask(&*encode_);
        if (analyse_)
            group.AddTask(&*analyse_);
        if (front_)
            group.AddTask(&*front_);
        if (signal_)
            group.AddTask(&*signal_);
        for (std::int32_t l = 0; l < SpecLevels; ++l) {
            if (speculate_[l])
                group.AddTask(&*speculate_[l]);
            if (specTheta_[l])
                group.AddTask(&*specTheta_[l]);
        }
        if (thetaTask_)
            group.AddTask(&*thetaTask_);
        for (std::uint32_t i = 0; i < resamplerCount_; ++i)
            group.AddTask(&*resamplers_[i]);

        // Read waits only for tasks when FileRead delivers the input; synchronous reads (stdin) block
        // their thread and stay on the I/O scheduler
        if (settings_.asyncInput)
            compute->AddTask(&*read_, TaskPriority::High);
        else
            io->AddTask(&*read_, TaskPriority::Normal);
        compute->AddTask(&*encode_, TaskPriority::High);
        if (thetaTask_)
            compute->AddTask(&*thetaTask_, TaskPriority::High);
        for (std::int32_t l = 0; l < SpecLevels; ++l) {
            if (speculate_[l])
                compute->AddTask(&*speculate_[l], TaskPriority::High);
            if (specTheta_[l])
                compute->AddTask(&*specTheta_[l], TaskPriority::High);
        }
        if (front_) {
            compute->AddTask(&*front_, TaskPriority::High);
            compute->AddTask(&*signal_, TaskPriority::High);
        }
        if (analyse_)
            compute->AddTask(&*analyse_, TaskPriority::High);
        for (std::uint32_t i = 0; i < resamplerCount_; ++i)
            compute->AddTask(&*resamplers_[i], TaskPriority::Normal);
        group.Wait();

        // --- the encoder without the graph again
        if (chained_)
            chain_.Attach(encoder_, false);
        for (std::int32_t s = 0; s < streams; ++s)
            ms.Stream(s)->SetThetaTrialRunner(nullptr);
        if (speculate) {
            for (std::int32_t s = 0; s < streams; ++s)
                ms.Stream(s)->SetSpeculation(nullptr);
            for (std::size_t i = 0; i < specs.Size(); ++i)
                delete specs[i];
        }
        return encode_->Result();
    }

    std::int32_t SequentialGraph::Run(SchedulerHandle& compute) noexcept
    {
        Synchronization::TaskGroup group;
        group.AddTask(&encode_);
        compute->AddTask(&encode_, TaskPriority::Normal);
        group.Wait();
        return encode_.Result();
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
