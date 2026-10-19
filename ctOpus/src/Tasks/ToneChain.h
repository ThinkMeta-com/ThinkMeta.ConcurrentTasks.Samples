// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <atomic>
#include <cstdint>

#include "CeltEncoder.h"
#include "HeapArray.h"
#include "OggOpusEncoder.h"
#include "OpusMultistreamEncoder.h"
#include "OpusStreamEncoder.h"

// headers with imports last
#include "GraphEdges.h"

import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    // The analysis chain, three nodes and the rings between them (docs/task-graph.md, steps 3 to 6): the tonality
    // analysis of a call depends only on the 48 kHz stream and on the schedule of the calls,
    // the CELT signal stage (sample peak, pre-emphasis, tone and transient detection, pitch
    // search) only on the CELT input, which the chain rebuilds from the stream; neither
    // depends on the encoder's decisions, so the chain runs ahead of the encoder task, which
    // only reads the results (ToneAnalysis::GetInfoFrom, CeltEncoder::Encode with the frame).
    // The speculative CELT front parts follow on a task of their own (RunFronts), between the
    // chain and the encoder. The nodes:
    //   Analyse (AnalyseTask, RunAnalyse): blocks from the ring → the tonality analysis per call
    //   Signal (SignalTask, RunSignals): per call → the input and CELT signal stages
    //   Front (FrontTask, RunFronts): per call → the speculative CELT front part
    // The edges are the rings of the calls (views_, inputFrames_, frames_, fronts_), which the
    // encoder reads through ToneAnalysisSource. A "call" is one call of the encoder's write loop
    // (OpusStreamEncoder::EncodeNative), the unit the chain analyses ahead. Shared counters are
    // std::atomic; the per-stream counters in HeapArrays and a front part's epoch are accessed
    // through std::atomic_ref (Load, Store, Epoch below).
    class ToneChain final : public ToneAnalysisSource
    {
    public:
        // ring of the per-call views; the chain stays this many calls ahead at most
        static constexpr std::int64_t ViewRing = 256;
        // run_analysis clamps the analysed frames to (DETECT_SIZE - 5) * 20 ms
        static constexpr std::int32_t AnalysisClamp = 95 * 960;
        static constexpr std::int32_t Unlimited     = 1 << 30; // an analysis size before the end is known

        // The analysing streams of the encoder (none: false); called before the encoding starts.
        bool Initialize(OggOpusEncoder& encoder, std::int32_t lookahead) noexcept
        {
            OpusMultistreamEncoder& ms = encoder.Multistream();
            nbStreams_                 = ms.NbStreams();
            schedule_                  = encoder.GetSchedule();
            if (!slotOf_.Allocate(static_cast<std::size_t>(nbStreams_)) || !signalOf_.Allocate(static_cast<std::size_t>(nbStreams_))
                || !inputs_.Allocate(static_cast<std::size_t>(nbStreams_)))
                return false;
            std::int32_t n       = 0;
            std::int32_t signals = 0;
            for (std::int32_t s = 0; s < nbStreams_; ++s) {
                slotOf_[s]   = -1;
                signalOf_[s] = -1;
                if (ms.Stream(s)->AnalysisEnabled())
                    slotOf_[s] = n++;
                // the CELT signal stage for frames of at most 20 ms (larger ones are coded as several)
                if (schedule_.frameSize <= 960 && inputs_[signals].Init(*ms.Stream(s)))
                    signalOf_[s] = signals++;
            }
            if (n + signals == 0)
                return false;
            count_   = n;
            signals_ = signals;
            if (!analyses_.Allocate(static_cast<std::size_t>(n)) || !history_.Allocate(static_cast<std::size_t>(n) * ToneAnalysis::HistorySize)
                || !views_.Allocate(static_cast<std::size_t>(n * ViewRing)) || !c1_.Allocate(static_cast<std::size_t>(nbStreams_))
                || !c2_.Allocate(static_cast<std::size_t>(nbStreams_)) || !lsbDepth_.Allocate(static_cast<std::size_t>(n))
                || !celt_.Allocate(static_cast<std::size_t>(signals)) || !signalConfig_.Allocate(static_cast<std::size_t>(signals))
                || !signalState_.Allocate(static_cast<std::size_t>(signals)) || !signalStream_.Allocate(static_cast<std::size_t>(signals))
                || !frames_.Allocate(static_cast<std::size_t>(signals * ViewRing)) || !streamPcm_.Allocate(2 * 960)
                || !fronts_.Allocate(static_cast<std::size_t>(signals * ViewRing)) || !inputFrames_.Allocate(static_cast<std::size_t>(signals * ViewRing))
                || !frontState_.Allocate(static_cast<std::size_t>(signals)) || !frontBase_.Allocate(static_cast<std::size_t>(signals))
                || !frontReady_.Allocate(static_cast<std::size_t>(signals)) || !reader_.Allocate(static_cast<std::size_t>(signals))
                || !frontNext_.Allocate(static_cast<std::size_t>(signals)) || !frontPublished_.Allocate(static_cast<std::size_t>(signals))
                || !resyncSeq_.Allocate(static_cast<std::size_t>(signals)) || !appliedSeq_.Allocate(static_cast<std::size_t>(signals))
                || !mailbox_.Allocate(static_cast<std::size_t>(signals)) || !callFrameSize_.Allocate(static_cast<std::size_t>(ViewRing)))
                return false;
            for (std::int32_t s = 0; s < nbStreams_; ++s) {
                ms.AnalysisChannels(s, c1_[s], c2_[s]);
                const std::int32_t i = slotOf_[s];
                if (i >= 0) {
                    analyses_[i] = ms.Stream(s)->Analysis();
                    analyses_[i].SetHistory(history_.Data() + static_cast<std::size_t>(i) * ToneAnalysis::HistorySize);
                    lsbDepth_[i] = ms.Stream(s)->AnalysisLsbDepth(OpusMultistreamEncoder::InputLsbDepth);
                }
                const std::int32_t k = signalOf_[s];
                if (k >= 0) {
                    celt_[k]         = &ms.Stream(s)->Celt();
                    signalConfig_[k] = celt_[k]->GetSignalConfig();
                    signalState_[k]  = celt_[k]->GetSignalState();
                    signalStream_[k] = s;
                    // the reader of the tonality analysis as the encoder reads it
                    reader_[k] = i >= 0 ? analyses_[i] : ms.Stream(s)->Analysis();
                }
            }
            preskip_  = lookahead;
            channels_ = encoder.Channels();
            // the analysis keeps 100 entries; the history 512: up to 400 entries ahead
            const std::int64_t entriesPerCall = (schedule_.frameSize + 959) / 960 + 1;
            maxLead_                          = MinI64(ViewRing - 8, 400 / entriesPerCall);
            window_                           = AnalysisClamp + 1 + 3 * BlockRing::BlockFrames;
            return buffer_.Allocate(static_cast<std::size_t>(window_) * channels_);
        }

        // Hands the stream encoders the chain (or takes it away again).
        void Attach(OggOpusEncoder& encoder, bool on) noexcept
        {
            OpusMultistreamEncoder& ms = encoder.Multistream();
            for (std::int32_t s = 0; s < nbStreams_; ++s) {
                if (slotOf_[s] >= 0 || signalOf_[s] >= 0)
                    ms.Stream(s)->SetToneSource(on ? this : nullptr, s);
            }
        }

        // the encoder task
        const ToneAnalysis& Wait(std::int32_t stream, std::int64_t call, ToneAnalysis::View& view) noexcept override
        {
            WaitCall(call);
            const std::int32_t i = slotOf_[stream];
            view                 = views_[static_cast<std::size_t>(i * ViewRing + call % ViewRing)];
            return analyses_[i];
        }

        const CeltSignalFrame* WaitSignal(std::int32_t stream, std::int64_t call) noexcept override
        {
            const std::int32_t k = signalOf_[stream];
            if (k < 0 || !WaitCall(call) || !WaitSignals(call))
                return nullptr;
            return &frames_[static_cast<std::size_t>(k * ViewRing + call % ViewRing)];
        }

        bool PeekCall(std::int32_t stream, std::int64_t call, ToneAnalysis::View& view, const CeltSignalFrame*& signal,
                      const CeltFrontFrame*& front) noexcept override
        {
            const std::int32_t k = signalOf_[stream];
            if (k < 0 || signalPublished_.load(std::memory_order_acquire) <= call)
                return false;
            const std::size_t slot = static_cast<std::size_t>(k * ViewRing + call % ViewRing);
            // the front part computed after the last resynchronisation (final until the encoder sends another)
            const CeltFrontFrame& f = fronts_[slot];
            if (!(Load(&frontPublished_[k]) > call && Epoch(f) == Load(&resyncSeq_[k])))
                return false;
            const std::int32_t i = slotOf_[stream];
            if (i >= 0)
                view = views_[static_cast<std::size_t>(i * ViewRing + call % ViewRing)];
            signal = &frames_[slot];
            front  = &f;
            return true;
        }

        const StreamInputFrame* WaitInput(std::int32_t stream, std::int64_t call) noexcept override
        {
            const std::int32_t k = signalOf_[stream];
            if (k < 0 || !WaitCall(call) || !WaitSignals(call))
                return nullptr;
            return &inputFrames_[static_cast<std::size_t>(k * ViewRing + call % ViewRing)];
        }

        const CeltFrontFrame* WaitFront(std::int32_t stream, std::int64_t call) noexcept override
        {
            const std::int32_t k = signalOf_[stream];
            if (k < 0)
                return nullptr;
            const CeltFrontFrame& frame = fronts_[static_cast<std::size_t>(k * ViewRing + call % ViewRing)];
            // computed (completely) after the last resynchronisation
            while (!(Load(&frontPublished_[k]) > call && Epoch(frame) == Load(&resyncSeq_[k]))) {
                if (frontsStopped_.load(std::memory_order_acquire))
                    return nullptr;
                chainDone_.Wait();
            }
            return &frame;
        }

        void Resync(std::int32_t stream, std::int64_t call, const FrontSync& sync) noexcept override
        {
            const std::int32_t k = signalOf_[stream];
            if (k < 0)
                return;
            // the encoder sends the next one only after the front part of call, so the chain has this one by then
            mailbox_[k].call = call;
            mailbox_[k].sync = sync;
            std::atomic_ref<std::int64_t>(resyncSeq_[k]).fetch_add(1);
            frontWake_.Set();
        }

        // the encoder task has finished
        void EncoderDone() noexcept
        {
            encoderDone_.store(true);
            encoderMoved_.Set();
            frontWake_.Set();
        }

        // the Analyse node (AnalyseTask)
        void RunAnalyse(BlockRing& ring) noexcept;
        // the Signal node (SignalTask: the input and signal stages of the signal streams)
        void RunSignals(BlockRing& ring) noexcept;
        // the Front node (FrontTask; only with a signal stage)
        void RunFronts(BlockRing& ring) noexcept;
        bool HasFronts() const noexcept { return signals_ > 0; }

        // the end of the chain (also on abort): the encoder no longer waits for its calls
        void Stop() noexcept
        {
            stopped_.store(true);
            chainDone_.Set();
            frontWake_.Set();
            signalWake_.Set();
        }

    private:
        // the next block into the window (keeping the frames from keep on); false at the end
        bool CopyBlock(BlockRing& ring, std::int64_t keep) noexcept;

        struct Mailbox {
            std::int64_t call;
            FrontSync    sync;
        };

        // the per-stream counters (HeapArray elements) and a front part's epoch, atomically
        static std::int64_t Load(const std::int64_t* x) noexcept
        {
            return std::atomic_ref<std::int64_t>(*const_cast<std::int64_t*>(x)).load(std::memory_order_acquire);
        }
        static void         Store(std::int64_t* x, std::int64_t value) noexcept { std::atomic_ref<std::int64_t>(*x).store(value); }
        static std::int32_t Epoch(const CeltFrontFrame& frame) noexcept
        {
            return std::atomic_ref<std::int32_t>(const_cast<std::int32_t&>(frame.epoch)).load(std::memory_order_acquire);
        }
        static void SetEpoch(CeltFrontFrame& frame, std::int32_t epoch) noexcept { std::atomic_ref<std::int32_t>(frame.epoch).store(epoch); }

        // the signal stage and the tonality analysis of the next call; false at the end of the calls
        bool AdvanceCall(BlockRing& ring) noexcept;
        // the resynchronisations the encoder sent
        void ApplyResyncs() noexcept;
        bool ResyncPending() const noexcept
        {
            for (std::int32_t k = 0; k < signals_; ++k) {
                if (Load(&resyncSeq_[k]) != appliedSeq_[k])
                    return true;
            }
            return false;
        }
        // the front part of the next call of signal stream k
        void ComputeFront(std::int32_t k) noexcept;

        // waits for the signal stages of call (false: they ended before it)
        bool WaitSignals(std::int64_t call) noexcept
        {
            while (signalPublished_.load(std::memory_order_acquire) <= call && !signalsStopped_.load(std::memory_order_acquire))
                chainDone_.Wait();
            return signalPublished_.load(std::memory_order_acquire) > call;
        }

        // waits for call (false: the chain ended before it); the encoder is at call now
        bool WaitCall(std::int64_t call) noexcept
        {
            while (published_.load(std::memory_order_acquire) <= call && !stopped_.load(std::memory_order_acquire))
                chainDone_.Wait();
            if (call > consumed_.load(std::memory_order_relaxed)) { // only the encoder moves it
                consumed_.store(call);
                // wake a stalled chain only once it may go on (not on every call)
                if (call >= resumeAt_.load())
                    encoderMoved_.Set();
            }
            return published_.load(std::memory_order_acquire) > call;
        }

        std::int32_t                  nbStreams_ = 0;
        std::int32_t                  count_     = 0;
        HeapArray<std::int32_t>       slotOf_; // per stream: its index among the analysing streams, or -1
        HeapArray<ToneAnalysis>       analyses_;
        HeapArray<AnalysisInfo>       history_;
        HeapArray<ToneAnalysis::View> views_;
        HeapArray<std::int32_t>       c1_;
        HeapArray<std::int32_t>       c2_;
        HeapArray<std::int32_t>       lsbDepth_;
        // the CELT signal stage
        std::int32_t                         signals_ = 0;
        HeapArray<std::int32_t>              signalOf_; // per stream: its index among the streams with a signal stage, or -1
        HeapArray<CeltInputChain>            inputs_;
        HeapArray<const CeltEncoder*>        celt_;
        HeapArray<CeltEncoder::SignalConfig> signalConfig_;
        HeapArray<CeltEncoder::SignalState>  signalState_;
        HeapArray<std::int32_t>              signalStream_;
        HeapArray<CeltSignalFrame>           frames_;
        HeapArray<StreamInputFrame>          inputFrames_; // the input stage per call
        HeapArray<float>                     streamPcm_;   // one frame of a stream's channels
        // the CELT front part
        HeapArray<CeltFrontFrame>           fronts_;
        HeapArray<CeltEncoder::FrontState>  frontState_;
        HeapArray<CeltEncoder::FrontParams> frontBase_;  // the parameters of the last resynchronisation
        HeapArray<bool>                     frontReady_; // resynchronised at least once
        HeapArray<ToneAnalysis>             reader_;     // the encoder's reads of the tonality analysis
        HeapArray<std::int64_t>             frontNext_;
        HeapArray<std::int64_t>             frontPublished_;
        HeapArray<std::int64_t>             resyncSeq_; // written by the encoder
        HeapArray<std::int64_t>             appliedSeq_;
        HeapArray<Mailbox>                  mailbox_;
        HeapArray<std::int32_t>             callFrameSize_; // per call (ring): the frame size
        std::uint8_t                        scratch_[1275]; // the empty range coder of a front part
        std::int64_t                        callStart_ = 0; // the next call: its start in the stream
        std::int64_t                        callNext_  = 0; // and its index
        alignas(64) std::atomic<bool> encoderDone_{false};
        OggOpusEncoder::Schedule schedule_ = {};
        std::int32_t             preskip_  = 0;
        std::int32_t             channels_ = 0;
        std::int64_t             maxLead_  = 0;
        HeapArray<float>         buffer_; // the 48 kHz stream from bufferStart_ to bufferEnd_
        std::int64_t             window_                   = 0;
        std::int64_t             bufferStart_              = 0;
        std::int64_t             bufferEnd_                = 0;
        alignas(64) std::atomic<std::int64_t> published_{0};       // calls analysed
        alignas(64) std::atomic<std::int64_t> signalPublished_{0}; // calls with their input and signal stages
        alignas(64) std::atomic<bool> signalsStopped_{false};
        std::int64_t                      signalNext_ = 0;
        Synchronization::AutoResetEvent_s signalWake_; // a call analysed, or the end; one waiter: Signal
        alignas(64) std::atomic<std::int64_t> consumed_{-1}; // the latest call the encoder read
        alignas(64) std::atomic<bool> stopped_{false};
        alignas(64) std::atomic<bool> frontsStopped_{false};
        alignas(64) std::atomic<std::int64_t> resumeAt_{1ll << 62}; // the encoder call that wakes a stalled chain
        Synchronization::AutoResetEvent_s chainDone_;    // a call analysed, or the end; one waiter: the encoder
        Synchronization::AutoResetEvent_s encoderMoved_; // the encoder read a new call; one waiter: Analyse
        Synchronization::AutoResetEvent_s frontWake_;    // a call analysed, a resynchronisation, or the end; one waiter: Front
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
