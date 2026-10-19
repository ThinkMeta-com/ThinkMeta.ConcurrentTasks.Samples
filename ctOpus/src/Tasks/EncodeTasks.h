// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <cstdint>

#include "OggOpusEncoder.h"
#include "WavInput.h"

// headers with imports last
#include "EncodeHelpers.h"
#include "GraphEdges.h"
#include "ToneChain.h"

import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    /// <summary>Called by the Encode node after every block (or every frame of the sequential graph).</summary>
    struct EncodeProgress {
        void (*callback)(void* user) = nullptr;
        void* user                   = nullptr;

        void operator()() const noexcept
        {
            if (callback != nullptr)
                callback(user);
        }
    };

    /// <summary>
    /// The node Read (compute scheduler, or the I/O scheduler for stdin): reads the WAV samples into the PCM feed and dispatches every
    /// block of the ring whose inputs are all read; at the end it appends libopusenc's drain
    /// extension (LPC continuation, then zeros) and publishes the end of the stream.
    /// </summary>
    class ReadTask : public Tasks::Task
    {
    public:
        ReadTask(WavInput& wav, PcmFeed& pcm, BlockRing& ring) noexcept : wav_(wav), pcm_(pcm), ring_(ring) {}

        void Run() noexcept override;

    private:
        WavInput&  wav_;
        PcmFeed&   pcm_;
        BlockRing& ring_;
    };

    /// <summary>
    /// The node Resample (compute scheduler, one task per thread): computes the dispatched blocks of
    /// 48 kHz output, any block on any worker (input at 48 kHz: copies them).
    /// </summary>
    class ResampleTask : public Tasks::Task
    {
    public:
        ResampleTask(PcmFeed& pcm, BlockRing& ring) noexcept : pcm_(pcm), ring_(ring) {}

        void Run() noexcept override;

    private:
        PcmFeed&   pcm_;
        BlockRing& ring_;
    };

    /// <summary>The node Analyse (compute, High): the tonality analysis of the calls (ToneChain::RunAnalyse).</summary>
    class AnalyseTask : public Tasks::Task
    {
    public:
        AnalyseTask(ToneChain& chain, BlockRing& ring) noexcept : chain_(chain), ring_(ring) {}

        void Run() noexcept override { chain_.RunAnalyse(ring_); }

    private:
        ToneChain& chain_;
        BlockRing& ring_;
    };

    /// <summary>The node Signal (compute, High): the input and CELT signal stages of the calls (ToneChain::RunSignals).</summary>
    class SignalTask : public Tasks::Task
    {
    public:
        SignalTask(ToneChain& chain, BlockRing& ring) noexcept : chain_(chain), ring_(ring) {}

        void Run() noexcept override { chain_.RunSignals(ring_); }

    private:
        ToneChain& chain_;
        BlockRing& ring_;
    };

    /// <summary>The node Front (compute, High): the speculative CELT front parts of the calls (ToneChain::RunFronts).</summary>
    class FrontTask : public Tasks::Task
    {
    public:
        FrontTask(ToneChain& chain, BlockRing& ring) noexcept : chain_(chain), ring_(ring) {}

        void Run() noexcept override { chain_.RunFronts(ring_); }

    private:
        ToneChain& chain_;
        BlockRing& ring_;
    };

    /// <summary>The node Speculate (compute, High), one per level of the cascade: runs the speculations posted to its queue.</summary>
    class SpeculateTask : public Tasks::Task
    {
    public:
        explicit SpeculateTask(SpeculationQueue& queue) noexcept : queue_(queue) {}

        void Run() noexcept override { queue_.Run(); }

    private:
        SpeculationQueue& queue_;
    };

    /// <summary>The node Theta (compute, High): runs the theta trials its encoder (Encode or a Speculate level) hands over.</summary>
    class ThetaTask : public Tasks::Task
    {
    public:
        explicit ThetaTask(ThetaHelper& helper) noexcept : helper_(helper) {}

        void Run() noexcept override { helper_.Run(); }

    private:
        ThetaHelper& helper_;
    };

    /// <summary>
    /// The node Encode (compute, High): hands the blocks of the ring in order to libopusenc's buffer
    /// logic, which encodes (with the results of the analysis chain, the speculations and the theta
    /// helpers) and writes the Ogg pages; at the end it drains with the extension's output and stops
    /// the nodes that serve it.
    /// </summary>
    class EncodeTask : public Tasks::Task
    {
    public:
        static constexpr std::int32_t SpecLevels = 3; // the depth of the speculation cascade at most

        /// <summary>The nodes Encode stops when it is done (null: not in the graph).</summary>
        struct Served {
            ToneChain*        chain                    = nullptr;
            ThetaHelper*      theta                    = nullptr;
            SpeculationQueue* speculations[SpecLevels] = {};
            ThetaHelper*      specThetas[SpecLevels]   = {};
        };

        EncodeTask(OggOpusEncoder& encoder, PcmFeed& pcm, BlockRing& ring, const Served& served, const EncodeProgress& progress) noexcept
            : encoder_(encoder),
              pcm_(pcm),
              ring_(ring),
              served_(served),
              progress_(progress)
        {}

        std::int32_t Result() const noexcept { return result_; }

        void Run() noexcept override;

    private:
        OggOpusEncoder&      encoder_;
        PcmFeed&             pcm_;
        BlockRing&           ring_;
        const Served         served_;
        const EncodeProgress progress_;
        std::int32_t         result_ = OpeOk;
    };

    /// <summary>
    /// The node SequentialEncode, the only node of the sequential graph: opusenc's main loop, one
    /// frame per iteration (read, ope_encoder_write_float), then ope_encoder_drain.
    /// </summary>
    class SequentialEncodeTask : public Tasks::Task
    {
    public:
        /// <summary>buffer: frameSize frames of the input's channels.</summary>
        SequentialEncodeTask(WavInput& wav, OggOpusEncoder& encoder, float* buffer, std::int32_t frameSize, const EncodeProgress& progress) noexcept
            : wav_(wav),
              encoder_(encoder),
              buffer_(buffer),
              frameSize_(frameSize),
              progress_(progress)
        {}

        std::int32_t Result() const noexcept { return result_; }

        void Run() noexcept override;

    private:
        WavInput&            wav_;
        OggOpusEncoder&      encoder_;
        float* const         buffer_;
        const std::int32_t   frameSize_;
        const EncodeProgress progress_;
        std::int32_t         result_ = OpeOk;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
