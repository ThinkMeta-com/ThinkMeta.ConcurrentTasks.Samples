// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <immintrin.h>

#include <cstdint>

#include "CeltBands.h"
#include "CeltEncoder.h"

// headers with imports last
import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    // The edges between Encode and its helper nodes: Encode posts work, the helper node (ThetaTask,
    // SpeculateTask) runs it.

    // The edge Encode ⇄ Theta: the second theta RDO trial of the stereo bands on a task of its own
    // (QuantAllBands with a runner): the encoder hands it over and waits for it a few microseconds later, so both
    // sides spin first and only then block.
    class ThetaHelper final : public ThetaTrialRunner
    {
    public:
        ThetaHelper() noexcept : storage_(NewThetaTrial()) {}
        ~ThetaHelper() { DeleteThetaTrial(storage_); }
        ThetaHelper(const ThetaHelper&)            = delete;
        ThetaHelper& operator=(const ThetaHelper&) = delete;

        bool Valid() const noexcept { return storage_ != nullptr; }

        ThetaTrial* Trial() noexcept override { return storage_; }

        void Start(ThetaTrial* trial) noexcept override
        {
            trial_ = trial;
            InterlockedIncrement64(&posted_);
            if (sleeping_ != 0)
                wake_.Set();
        }

        void Finish() noexcept override
        {
            const std::int64_t ticket = posted_;
            for (std::int32_t spin = 0; done_ != ticket; ++spin) {
                if (spin < FinishSpins) {
                    _mm_pause();
                    continue;
                }
                InterlockedExchange(&encoderWaiting_, 1);
                if (done_ != ticket)
                    finished_.Wait();
                InterlockedExchange(&encoderWaiting_, 0);
            }
        }

        // the end (the encoder is done)
        void Stop() noexcept
        {
            InterlockedExchange(&stop_, 1);
            wake_.Set();
        }

        // the Theta node (ThetaTask)
        void Run() noexcept
        {
            for (;;) {
                const std::int64_t seen = done_;
                for (std::int32_t spin = 0; posted_ == seen && stop_ == 0; ++spin) {
                    if (spin < IdleSpins) {
                        _mm_pause();
                        continue;
                    }
                    InterlockedExchange(&sleeping_, 1);
                    if (posted_ == seen && stop_ == 0)
                        wake_.Wait();
                    InterlockedExchange(&sleeping_, 0);
                    spin = 0;
                }
                if (posted_ == seen)
                    return;
                RunThetaTrial(trial_);
                InterlockedExchange64(&done_, seen + 1);
                if (encoderWaiting_ != 0)
                    finished_.Set();
            }
        }

    private:
        static constexpr std::int32_t FinishSpins = 1 << 14; // a trial takes a few microseconds
        static constexpr std::int32_t IdleSpins   = 1 << 12; // over the gaps between frames

        ThetaTrial* const storage_;
        ThetaTrial* volatile trial_               = nullptr;
        alignas(64) volatile std::int64_t posted_ = 0;
        alignas(64) volatile std::int64_t done_   = 0;
        alignas(64) volatile long sleeping_       = 0;
        alignas(64) volatile long encoderWaiting_ = 0;
        alignas(64) volatile long stop_           = 0;
        Synchronization::AutoResetEvent_s wake_;     // a trial for a sleeping helper, or the end
        Synchronization::AutoResetEvent_s finished_; // a trial done for a waiting encoder
    };

    // The edge Encode → Speculate: the speculations of the CELT encoders on a task of
    // their own, in the order posted.
    class SpeculationQueue final : public SpeculationRunner
    {
    public:
        void Post(CeltSpeculation* spec) noexcept override
        {
            const std::int64_t n = posted_;
            ring_[n % Ring]      = spec;
            InterlockedExchange64(&posted_, n + 1);
            wake_.Set();
        }

        void Stop() noexcept
        {
            InterlockedExchange(&stop_, 1);
            wake_.Set();
        }

        void Run() noexcept
        {
            std::int64_t next = 0;
            for (;;) {
                while (next < posted_) {
                    RunSpeculation(*ring_[next % Ring]);
                    ++next;
                }
                if (stop_ != 0)
                    return;
                wake_.Wait();
            }
        }

    private:
        static constexpr std::int64_t Ring = 64; // more than streams: one speculation per stream at a time

        CeltSpeculation* ring_[Ring]              = {};
        alignas(64) volatile std::int64_t posted_ = 0;
        alignas(64) volatile long stop_           = 0;
        Synchronization::AutoResetEvent_s wake_;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
