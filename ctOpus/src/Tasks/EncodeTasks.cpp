// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "EncodeTasks.h"

#include <cstdint>
#include <cstring>

#include "HeapArray.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    namespace {

        constexpr std::int64_t OpenEnded = 1ll << 40; // input length while it is not known yet

    } // namespace

    // --- Read

    void ReadTask::Run() noexcept
    {
        PcmFeed&           p           = pcm_;
        BlockRing&         ring        = ring_;
        const std::int64_t BlockFrames = BlockRing::BlockFrames;
        const std::int32_t channels    = p.channels;
        std::int64_t       w           = p.filterLen - 1; // next padded write position
        std::int64_t       next        = 0;               // next block to dispatch
        HeapArray<float>   frames;
        if (!frames.Allocate(static_cast<std::size_t>(PcmFeed::ReadFrames) * channels)) {
            ring.FailRead();
            return;
        }
        for (;;) {
            if (ring.Aborted())
                break;
            const std::int32_t got = wav_.Read(frames.Data(), PcmFeed::ReadFrames);
            if (got > 0) {
                if (!p.input.Commit(w + got + PcmFeed::CommitMargin)) {
                    ring.FailRead();
                    return;
                }
                for (std::int32_t c = 0; c < channels; ++c) {
                    float* dst = p.input.Channel(c) + w;
                    for (std::int32_t i = 0; i < got; ++i)
                        dst[i] = frames[static_cast<std::size_t>(i) * channels + c];
                }
                w += got;
                // every block whose inputs are all read
                while (p.resampler.InputNeeded((next + 1) * BlockFrames) <= w) {
                    if (!ring.AcquireRead(next))
                        break;
                    ring.CompleteRead(next);
                    ++next;
                }
                // below what the unreleased blocks and the drain extension (the last 480 frames) read
                p.input.Decommit(MinI64(p.resampler.TapStart(ring.Released() * BlockFrames), w - LpcExtensionInput) - p.filterLen);
            }
            if (got < PcmFeed::ReadFrames)
                break;
        }

        // the end of the input: ope_encoder_drain's LPC extension of the last 480 frames, then zeros.
        // With the resampler it extends libopusenc's LPC buffer (zeros before a short input); at
        // 48 kHz it extends the encoder's own buffer, which holds only the samples: at most 480.
        const std::int64_t n = w - (p.filterLen - 1);
        HeapArray<float>   lpc;
        if (!lpc.Allocate(static_cast<std::size_t>(LpcExtensionInput + LpcExtensionLength) * channels)) {
            ring.FailRead();
            return;
        }
        for (std::int32_t i = 0; i < LpcExtensionInput; ++i) {
            const std::int64_t src = n - LpcExtensionInput + i;
            for (std::int32_t c = 0; c < channels; ++c)
                lpc[static_cast<std::size_t>(i) * channels + c] = src >= 0 ? p.input.Channel(c)[p.filterLen - 1 + src] : 0.f;
        }
        const std::int32_t history = p.resampled ? LpcExtensionInput : static_cast<std::int32_t>(MinI64(n, LpcExtensionInput));
        LpcExtend(lpc.Data() + static_cast<std::size_t>(LpcExtensionInput) * channels, history, LpcExtensionLength, channels);

        const std::int64_t realOutputs  = p.resampler.OutputsFor(n);
        const std::int64_t totalOutputs = realOutputs + p.drainFrames;
        const std::int64_t needed       = MaxI64(p.resampler.InputNeeded(totalOutputs), w + LpcExtensionLength);
        if (!p.input.Commit(needed + PcmFeed::CommitMargin)) {
            ring.FailRead();
            return;
        }
        for (std::int32_t c = 0; c < channels; ++c) {
            float* dst = p.input.Channel(c) + w;
            for (std::int32_t i = 0; i < LpcExtensionLength; ++i)
                dst[i] = lpc[static_cast<std::size_t>(LpcExtensionInput + i) * channels + c];
        }
        ring.PublishEnd(n, realOutputs, totalOutputs);
        const std::int64_t blocks = (totalOutputs + BlockFrames - 1) / BlockFrames;
        while (next < blocks && ring.AcquireRead(next)) {
            ring.CompleteRead(next);
            ++next;
        }
        ring.EndRead();
    }

    // --- Resample

    void ResampleTask::Run() noexcept
    {
        for (;;) {
            const std::int64_t b = ring_.AcquireResample();
            if (b < 0)
                return;
            if (!ring_.Aborted())
                pcm_.resampler.Compute(pcm_.input.Channels(), pcm_.channels, b * BlockRing::BlockFrames, ring_.BlockEnd(b), ring_.SlotOf(b));
            ring_.CompleteResample(b);
        }
    }

    // --- Encode

    void EncodeTask::Run() noexcept
    {
        BlockRing&         ring        = ring_;
        const std::int64_t BlockFrames = BlockRing::BlockFrames;
        const std::int32_t channels    = pcm_.channels;
        HeapArray<float>   pad;
        if (!pad.Allocate(static_cast<std::size_t>(pcm_.drainFrames) * channels)) {
            result_ = OpeAllocFail;
            ring.Abort();
        }
        encoder_.SetInputLength(OpenEnded);
        // fewer moves of the encoder's buffer (where the output stays the same)
        if (!encoder_.EnlargeBuffer(8 * BlockFrames)) {
            result_ = OpeAllocFail;
            ring.Abort();
        }
        for (std::int64_t next = 0; ring.AcquireEncode(next); ++next) {
            if (result_ == OpeOk && !ring.Aborted()) {
                const float*       out       = ring.SlotOf(next);
                const std::int64_t j0        = next * BlockFrames;
                const std::int64_t j1        = ring.BlockEnd(next);
                const std::int64_t realEnd   = ring.EndKnown() ? ring.RealOutputs() : j1;
                const std::int64_t realCount = MaxI64(0, MinI64(j1, realEnd) - j0);
                if (realCount > 0)
                    result_ = encoder_.WriteResampled(out, static_cast<std::int32_t>(realCount));
                for (std::int64_t j = j0 + realCount; j < j1; ++j)
                    std::memcpy(pad.Data() + (j - ring.RealOutputs()) * channels, out + (j - j0) * channels, sizeof(float) * channels);
                if (result_ != OpeOk)
                    ring.Abort(); // wakes every stage, Analyse too
            }
            ring.CompleteEncode(next);
            progress_();
        }
        if (ring.MemoryError() && result_ == OpeOk)
            result_ = OpeAllocFail;
        if (result_ == OpeOk) {
            encoder_.SetInputLength(ring.InputFrames());
            result_ = encoder_.DrainResampled(pad.Data(), pcm_.drainFrames);
        }
        // the end of the nodes that serve Encode
        if (served_.chain != nullptr)
            served_.chain->EncoderDone();
        if (served_.theta != nullptr)
            served_.theta->Stop();
        for (std::int32_t l = 0; l < SpecLevels; ++l) {
            if (served_.speculations[l] != nullptr)
                served_.speculations[l]->Stop();
            if (served_.specThetas[l] != nullptr)
                served_.specThetas[l]->Stop();
        }
    }

    // --- SequentialEncode

    void SequentialEncodeTask::Run() noexcept
    {
        std::int32_t nbSamples = 0;
        for (;;) {
            nbSamples = wav_.Read(buffer_, frameSize_);
            result_   = encoder_.WriteFloat(buffer_, nbSamples);
            if (result_ != OpeOk || nbSamples < frameSize_)
                break;
            progress_();
        }
        if (result_ == OpeOk)
            result_ = encoder_.Drain();
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
