// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <Windows.h>

// Before any import: MSVC cannot merge a textual include that follows an import of the same entities.
#include <ThinkMeta.ConcurrentTasks/Core.h>

#include <immintrin.h>

#include <atomic>
#include <cstdint>

#include "CeltBands.h"
#include "CeltEncoder.h"

// headers with imports last
import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    // The edges between Encode and its helper nodes: Encode posts work, the helper node (ThetaTask,
    // SpeculateTask) runs it.

    // The edge Encode ⇄ Theta: the second theta RDO trial of the stereo bands on a task of its own
    // (QuantAllBands with a runner): the encoder hands it over and waits for it a few microseconds later, so both
    // sides spin first and only then block. The handshake before blocking is Dekker's: each side announces
    // that it is about to sleep (sleeping_, encoderWaiting_), then checks the other side's counter again; with
    // sequentially consistent operations on both sides one of them always sees the other, so no wake-up is lost.
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
            trial_.store(trial, std::memory_order_relaxed); // published by posted_
            posted_.fetch_add(1);
            if (sleeping_.load())
                wake_.Set();
        }

        void Finish() noexcept override
        {
            const std::int64_t ticket = posted_.load(std::memory_order_relaxed); // the encoder's own count
            for (std::int32_t spin = 0; done_.load(std::memory_order_acquire) != ticket; ++spin) {
                if (spin < FinishSpins) {
                    _mm_pause();
                    continue;
                }
                encoderWaiting_.store(true);
                if (done_.load() != ticket)
                    finished_.Wait();
                encoderWaiting_.store(false);
            }
        }

        // the end (the encoder is done)
        void Stop() noexcept
        {
            stop_.store(true);
            wake_.Set();
        }

        // the Theta node (ThetaTask)
        void Run() noexcept
        {
            for (;;) {
                const std::int64_t seen = done_.load(std::memory_order_relaxed); // only this task moves it
                for (std::int32_t spin = 0; posted_.load(std::memory_order_acquire) == seen && !stop_.load(std::memory_order_acquire); ++spin) {
                    if (spin < IdleSpins) {
                        _mm_pause();
                        continue;
                    }
                    sleeping_.store(true);
                    if (posted_.load() == seen && !stop_.load())
                        wake_.Wait();
                    sleeping_.store(false);
                    spin = 0;
                }
                if (posted_.load(std::memory_order_acquire) == seen)
                    return;
                RunThetaTrial(trial_.load(std::memory_order_relaxed));
                done_.store(seen + 1);
                if (encoderWaiting_.load())
                    finished_.Set();
            }
        }

    private:
        static constexpr std::int32_t FinishSpins = 1 << 14; // a trial takes a few microseconds
        static constexpr std::int32_t IdleSpins   = 1 << 12; // over the gaps between frames

        ThetaTrial* const        storage_;
        std::atomic<ThetaTrial*> trial_{nullptr};
        alignas(64) std::atomic<std::int64_t> posted_{0};
        alignas(64) std::atomic<std::int64_t> done_{0};
        alignas(64) std::atomic<bool> sleeping_{false};
        alignas(64) std::atomic<bool> encoderWaiting_{false};
        alignas(64) std::atomic<bool> stop_{false};
        Synchronization::AutoResetEvent_s wake_;     // a trial for a sleeping helper, or the end; one waiter: Theta
        Synchronization::AutoResetEvent_s finished_; // a trial done for a waiting encoder; one waiter: the encoder
    };

    // The edge Encode → Speculate: the speculations of the CELT encoders on a task of
    // their own, in the order posted.
    class SpeculationQueue final : public SpeculationRunner
    {
    public:
        void Post(CeltSpeculation* spec) noexcept override
        {
            const std::int64_t n = posted_.load(std::memory_order_relaxed); // only the encoder posts
            ring_[n % Ring]      = spec;
            posted_.store(n + 1);
            wake_.Set();
        }

        void Stop() noexcept
        {
            stop_.store(true);
            wake_.Set();
        }

        void Run() noexcept
        {
            std::int64_t next = 0;
            for (;;) {
                while (next < posted_.load(std::memory_order_acquire)) {
                    RunSpeculation(*ring_[next % Ring]);
                    ++next;
                }
                if (stop_.load(std::memory_order_acquire))
                    return;
                wake_.Wait();
            }
        }

    private:
        // More than streams: one speculation per stream is in the queue at a time, and mapping family 255 has up to 255
        // streams. (An overwritten entry would only lose its speculation, never change the output.)
        static constexpr std::int64_t Ring = 256;

        CeltSpeculation* ring_[Ring] = {};
        alignas(64) std::atomic<std::int64_t> posted_{0};
        alignas(64) std::atomic<bool> stop_{false};
        Synchronization::AutoResetEvent_s wake_; // one waiter: Speculate
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
