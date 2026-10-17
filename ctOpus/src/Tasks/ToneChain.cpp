// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include "ToneChain.h"

#include <cstdint>
#include <cstring>

#include "OpusDefines.h"
#include "RangeEncoder.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    bool ToneChain::CopyBlock(BlockRing& ring, std::int64_t keep) noexcept
    {
        const std::int64_t b = ring.AnalyseNext();
        if (!ring.AcquireAnalyse(b))
            return false;
        const std::int64_t j0 = b * BlockRing::BlockFrames;
        const std::int64_t j1 = ring.BlockEnd(b);
        // room for the block: drop what lies before keep
        if (j1 - bufferStart_ > window_) {
            const std::int64_t drop = keep - bufferStart_;
            std::memmove(buffer_.Data(), buffer_.Data() + static_cast<std::size_t>(drop) * channels_,
                         static_cast<std::size_t>(bufferEnd_ - keep) * channels_ * sizeof(float));
            bufferStart_ = keep;
        }
        std::memcpy(buffer_.Data() + static_cast<std::size_t>(j0 - bufferStart_) * channels_, ring.SlotOf(b),
                    static_cast<std::size_t>(j1 - j0) * channels_ * sizeof(float));
        bufferEnd_ = j1;
        ring.CompleteAnalyse(b);
        return true;
    }

    bool ToneChain::AdvanceCall(BlockRing& ring) noexcept
    {
        const std::int64_t s = callStart_;
        const std::int64_t j = callNext_;

        // the data of the call: the analysed frames (the clamp, or up to the end once it is
        // known; with more than the clamp present, run_analysis sees the clamp either way)
        std::int64_t end = 0;
        for (;;) {
            end                     = ring.EndKnown() ? ring.TotalOutputs() : -1;
            const std::int64_t need = end >= 0 ? MinI64(end, s + AnalysisClamp + 1) : s + AnalysisClamp + 1;
            if (bufferEnd_ >= need)
                break;
            if (!CopyBlock(ring, s)) {
                if (ring.Aborted() || !ring.EndKnown())
                    return false;
                end = ring.TotalOutputs();
                break;
            }
        }

        // the frame size of the call; the end of the calls (as EncodeBuffer)
        std::int32_t f = schedule_.frameSize;
        if (end >= 0) {
            const std::int64_t endGranule48k = (ring.InputFrames() * 48000 + schedule_.rate - 1) / schedule_.rate + preskip_;
            if (s >= endGranule48k || end - s <= schedule_.frameSize)
                return false;
            // the last packet: not more padding than needed
            if (s + schedule_.frameSize >= endGranule48k && schedule_.frameSizeRequest > OPUS_FRAMESIZE_20_MS) {
                std::int32_t request = OPUS_FRAMESIZE_20_MS;
                while (OggOpusEncoder::FrameSamples(request) < endGranule48k - s)
                    ++request;
                f = OggOpusEncoder::FrameSamples(request);
            }
        }
        const std::int32_t analysisSize = end >= 0 ? static_cast<std::int32_t>(MinI64(end - s, Unlimited)) : Unlimited;

        const float* pcm = buffer_.Data() + static_cast<std::size_t>(s - bufferStart_) * channels_;
        for (std::int32_t st = 0; st < nbStreams_; ++st) {
            const std::int32_t i = slotOf_[st];
            if (i < 0)
                continue;
            analyses_[i].Advance(pcm, analysisSize, f, c1_[st], c2_[st], channels_, lsbDepth_[i]);
            views_[static_cast<std::size_t>(i * ViewRing + j % ViewRing)] = analyses_[i].CurrentView();
        }
        // the stream's channels of the frame, for the signal task
        for (std::int32_t k = 0; k < signals_; ++k) {
            const std::int32_t st    = signalStream_[k];
            const std::int32_t c1    = c1_[st];
            const std::int32_t c2    = c2_[st];
            float*             frame = inputFrames_[static_cast<std::size_t>(k * ViewRing + j % ViewRing)].pcm;
            if (c2 >= 0) {
                for (std::int32_t i = 0; i < f; ++i) {
                    frame[2 * i]     = pcm[static_cast<std::size_t>(i) * channels_ + c1];
                    frame[2 * i + 1] = pcm[static_cast<std::size_t>(i) * channels_ + c2];
                }
            } else {
                for (std::int32_t i = 0; i < f; ++i)
                    frame[i] = pcm[static_cast<std::size_t>(i) * channels_ + c1];
            }
        }
        callFrameSize_[static_cast<std::size_t>(j % ViewRing)] = f;
        InterlockedExchange64(&published_, j + 1);
        chainDone_.Set();
        signalWake_.Set();
        callStart_ = s + f;
        callNext_  = j + 1;
        return true;
    }

    void ToneChain::ApplyResyncs() noexcept
    {
        for (std::int32_t k = 0; k < signals_; ++k) {
            const std::int64_t seq = Load(&resyncSeq_[k]);
            if (seq == appliedSeq_[k])
                continue;
            const Mailbox& m = mailbox_[k];
            appliedSeq_[k]   = seq;
            frontState_[k]   = m.sync.state;
            frontBase_[k]    = m.sync.params;
            reader_[k].SetRead(m.sync.readPos, m.sync.readSubframe);
            frontReady_[k] = true;
            frontNext_[k]  = m.call;
        }
    }

    void ToneChain::ComputeFront(std::int32_t k) noexcept
    {
        const std::int64_t j     = frontNext_[k];
        const std::size_t  slot  = static_cast<std::size_t>(k * ViewRing + j % ViewRing);
        CeltFrontFrame&    frame = fronts_[slot];
        // not readable while it is being computed: the epoch is published last
        InterlockedExchange(reinterpret_cast<volatile long*>(&frame.epoch), -1);
        frame.valid = frontReady_[k];
        if (frontReady_[k]) {
            // the parameters as at the resynchronisation, with the tonality analysis of this frame
            CeltEncoder::FrontParams params = frontBase_[k];
            const std::int32_t       f      = callFrameSize_[static_cast<std::size_t>(j % ViewRing)];
            const std::int32_t       i      = slotOf_[signalStream_[k]];
            AnalysisInfo             info   = {};
            if (i >= 0)
                reader_[k].GetInfoFrom(analyses_[i], views_[static_cast<std::size_t>(i * ViewRing + j % ViewRing)], info, f);
            params.analysisValid         = info.valid;
            params.analysisTonality      = info.tonality;
            params.analysisMaxPitchRatio = info.maxPitchRatio;
            std::memcpy(params.analysisLeakBoost, info.leakBoost, sizeof(params.analysisLeakBoost));
            RangeEncoder scratch;
            scratch.Init(scratch_, sizeof(scratch_));
            frame.params = params;
            frame.before = frontState_[k];
            static_assert(sizeof(frame.before.prefilterMem) == sizeof(frames_[slot].prefilterMemBefore));
            frame.prefilterMemAsSignal = std::memcmp(frame.before.prefilterMem, frames_[slot].prefilterMemBefore, sizeof(frame.before.prefilterMem)) == 0;
            celt_[k]->Front(params, frames_[slot], frontState_[k], &scratch, frame);
            frame.after = frontState_[k];
        }
        frontNext_[k] = j + 1;
        InterlockedExchange64(&frontPublished_[k], MaxI64(Load(&frontPublished_[k]), j + 1));
        InterlockedExchange(reinterpret_cast<volatile long*>(&frame.epoch), static_cast<long>(appliedSeq_[k]));
        chainDone_.Set();
    }

    void ToneChain::RunAnalyse(BlockRing& ring) noexcept
    {
        for (;;) {
            if (ring.Aborted())
                break;
            // too far ahead of the encoder: on again at half the lead
            if (callNext_ - consumed_ > maxLead_ && encoderDone_ == 0) {
                InterlockedExchange64(&resumeAt_, callNext_ - maxLead_ / 2);
                while (callNext_ - consumed_ > maxLead_ / 2 && !ring.Aborted() && encoderDone_ == 0)
                    encoderMoved_.Wait();
                InterlockedExchange64(&resumeAt_, 1ll << 62);
                continue;
            }
            if (!AdvanceCall(ring))
                break;
        }
        Stop();
        // the remaining blocks are the encoder's alone
        ring.DetachAnalyse();
    }

    void ToneChain::RunSignals(BlockRing& ring) noexcept
    {
        for (;;) {
            if (ring.Aborted())
                break;
            const std::int64_t published = published_;
            if (signalNext_ < published) {
                // the CELT input and the input stage of the frame, then the CELT signal stage
                const std::int64_t j = signalNext_;
                const std::int32_t f = callFrameSize_[static_cast<std::size_t>(j % ViewRing)];
                for (std::int32_t k = 0; k < signals_; ++k) {
                    const std::size_t slot  = static_cast<std::size_t>(k * ViewRing + j % ViewRing);
                    StreamInputFrame& input = inputFrames_[slot];
                    const float*      celt  = inputs_[k].Next(input.pcm, f, &input);
                    celt_[k]->AnalyseSignal(signalConfig_[k], celt, f, signalState_[k], frames_[slot]);
                }
                signalNext_ = j + 1;
                InterlockedExchange64(&signalPublished_, j + 1);
                chainDone_.Set();
                frontWake_.Set();
                continue;
            }
            // the chain has ended and every call is done
            if (stopped_ != 0 && signalNext_ >= published_)
                break;
            signalWake_.Wait();
        }
        InterlockedExchange(&signalsStopped_, 1);
        chainDone_.Set();
        frontWake_.Set();
    }

    void ToneChain::RunFronts(BlockRing& ring) noexcept
    {
        for (;;) {
            if (ring.Aborted())
                break;
            ApplyResyncs();
            // the front parts of the calls with their signal stage
            const std::int64_t published = signalPublished_;
            bool               worked    = false;
            for (std::int32_t k = 0; k < signals_; ++k) {
                if (frontNext_[k] < published) {
                    ComputeFront(k);
                    worked = true;
                }
            }
            if (worked)
                continue;
            // until the encoder is done: it may still resynchronise
            if (encoderDone_ != 0)
                break;
            frontWake_.Wait();
        }
        InterlockedExchange(&frontsStopped_, 1);
        chainDone_.Set();
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
