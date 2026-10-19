// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

// The speculation of the CELT encoder: the protocol and why the output never depends on it are
// described at the top of CeltSpeculation.h. No part of it is a port of libopus.

#include "CeltSpeculation.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "CeltBands.h"
#include "CeltMode.h"
#include "RangeEncoder.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    namespace {

        // the chain of speculations: the encoder's own and the ones cascaded from it
        void CollectChain(CeltSpeculation* first, CeltSpeculation* (&specs)[1 + CeltSpeculation::MaxAfter]) noexcept
        {
            std::int32_t k = 0;
            for (CeltSpeculation* sp = first; sp != nullptr && k < 1 + CeltSpeculation::MaxAfter; sp = sp->cascade)
                specs[k++] = sp;
        }

    } // namespace

    void CeltEncoder::BuildSpeculation(const EnergyTail& t, const float* error, bool transientGotDisabled, std::int32_t frameSize,
                                       std::int32_t nbCompressedBytes, const RangeEncoder& entryEnc, std::int64_t frame) noexcept
    {
        CeltSpeculation& s     = *spec_;
        CeltEncoder&     clone = s.clone;
        clone                  = *this;
        clone.spec_            = nullptr;
        clone.specOut_         = nullptr;
        clone.next_            = nullptr;
        clone.thetaRunner_     = s.trialRunner;
        clone.analysis_        = next_->analysis;
        // the end of this frame with the guessed bits left (the energies; the seed comes later)
        float specError[MaxChannels * NbEBands];
        std::memcpy(specError, error, sizeof(specError));
        std::uint8_t tailBytes[1276];
        RangeEncoder tailEnc;
        tailEnc.Init(tailBytes, sizeof(tailBytes));
        clone.FinishEnergies(t, specError, CeltSpeculation::GuessBitsLeft, &tailEnc);
        if (t.isTransient || transientGotDisabled)
            ++clone.consecTransient_;
        else
            clone.consecTransient_ = 0;
        // the request
        s.frame             = frame;
        s.pcm               = next_->pcm;
        s.signal            = next_->signal;
        s.front             = next_->front;
        s.frameSize         = frameSize;
        s.nbCompressedBytes = nbCompressedBytes;
        s.entryEnc          = entryEnc;
        s.afterCount        = 0;
        for (const NextCeltFrame* a = next_->after; a != nullptr && s.afterCount < CeltSpeculation::MaxAfter; a = a->after)
            s.after[s.afterCount++] = *a;
        for (std::int32_t i = 0; i < s.afterCount; ++i)
            s.after[i].after = i + 1 < s.afterCount ? &s.after[i + 1] : nullptr;
        s.cancel.store(0, std::memory_order_release);
        s.seed.Reset();
        // Idle -> Queued
        s.state.store(CeltSpeculation::Queued, std::memory_order_seq_cst);
        s.runner->Post(&s);
    }

    void CeltEncoder::PostQabJob(const QabInputs& in, const RangeEncoder& enc, const EnergyTail& t, const float* error, bool transientGotDisabled,
                                 std::int32_t frameSize, std::int32_t nbCompressedBytes, const RangeEncoder& entryEnc) noexcept
    {
        QabJob& job = *specOut_;
        job.in      = in;
        job.enc     = enc;
        job.enc.Rebase(job.bytes);
        job.enc.SetLog(nullptr);
        job.valid = true;
        if (spec_ != nullptr && next_ != nullptr && lastFrontUsed_ && spec_->runner != nullptr && spec_->State() == CeltSpeculation::Idle) {
            BuildSpeculation(t, error, transientGotDisabled, frameSize, nbCompressedBytes, entryEnc, specFrame_ + 1);
            job.cascaded = true;
        }
    }

    bool CeltEncoder::TakeSpeculation(std::int64_t frame, const QabInputs& in, RangeEncoder& enc) noexcept
    {
        // taken if its inputs are this frame's and it used no seed but this frame's
        bool             fromJob                              = false;
        CeltSpeculation* specs[1 + CeltSpeculation::MaxAfter] = {};
        CollectChain(spec_, specs);
        for (CeltSpeculation* sp : specs) {
            if (sp == nullptr || sp->State() == CeltSpeculation::Idle || sp->frame > frame)
                continue; // none, or for a later frame
            CeltSpeculation& s    = *sp;
            const bool       mine = s.frame == frame && !fromJob;
            if (!s.TakeBack()) {
                // started: its inputs, then its result (or its end after the cancellation)
                s.WaitFor(CeltSpeculation::Inputs);
                bool same = mine && s.State() != CeltSpeculation::Off && SameQabInputs(s.job.in, in);
                if (!same)
                    s.cancel.store(1, std::memory_order_seq_cst);
                s.WaitFor(CeltSpeculation::Done);
                same = same && s.State() == CeltSpeculation::Done
                    && (s.seed.used.load(std::memory_order_acquire) == 0 || s.seed.value.load(std::memory_order_acquire) == rng_);
                if (same) {
                    s.job.log.ReplayInto(enc, 0, s.job.log.Size());
                    fromJob = true;
                }
            }
            // Done/Off -> Idle
            s.state.store(CeltSpeculation::Idle, std::memory_order_seq_cst);
        }
        return fromJob;
    }

    bool CeltEncoder::SpeculateNext(const EnergyTail& t, const float* error, bool transientGotDisabled, std::int32_t frameSize, std::int32_t nbCompressedBytes,
                                    const RangeEncoder& entryEnc, std::int64_t frame) noexcept
    {
        // only with the next frame's inputs known and this frame's front part taken (no
        // resynchronisation)
        if (spec_ == nullptr || next_ == nullptr || !lastFrontUsed_ || spec_->runner == nullptr)
            return false;
        // a speculation still pending (a cascaded one, built on a frame quantised here after all)
        // is cancelled first
        CeltSpeculation* specs[1 + CeltSpeculation::MaxAfter] = {};
        CollectChain(spec_, specs);
        for (CeltSpeculation* sp : specs) {
            if (sp == nullptr || sp->State() == CeltSpeculation::Idle)
                continue;
            if (!sp->TakeBack()) {
                sp->cancel.store(1, std::memory_order_seq_cst);
                sp->WaitFor(CeltSpeculation::Done);
            }
            sp->state.store(CeltSpeculation::Idle, std::memory_order_seq_cst);
        }
        BuildSpeculation(t, error, transientGotDisabled, frameSize, nbCompressedBytes, entryEnc, frame + 1);
        return true;
    }

    void CeltEncoder::PublishSeed(std::uint32_t seed) noexcept
    {
        spec_->seed.Fulfil(seed);
    }

    void RunSpeculation(CeltSpeculation& s) noexcept
    {
        // Queued -> Running, unless the encoder has taken the request back
        std::int32_t expected = CeltSpeculation::Queued;
        if (!s.state.compare_exchange_strong(expected, CeltSpeculation::Running, std::memory_order_seq_cst))
            return;
        QabJob& job  = s.job;
        job.valid    = false;
        job.ran      = false;
        job.cascaded = false;
        if (s.cancel.load(std::memory_order_acquire) == 0) {
            // the frame up to its residual quantisation (which may request the frame after: the cascade)
            s.clone.specOut_   = &job;
            s.clone.specFrame_ = s.frame;
            s.clone.spec_      = s.cascade;
            s.clone.next_      = s.cascade != nullptr && s.afterCount != 0 ? &s.after[0] : nullptr;
            RangeEncoder enc   = s.entryEnc;
            enc.Rebase(job.bytes);
            std::uint8_t scratch[1276];
            s.clone.Encode(s.pcm, s.frameSize, scratch, s.nbCompressedBytes, &enc, s.signal, s.front);
            s.clone.specOut_ = nullptr;
            s.clone.spec_    = nullptr;
            s.clone.next_    = nullptr;
        }
        const bool usable = job.valid && s.cancel.load(std::memory_order_acquire) == 0;
        if (usable) {
            // Running -> Inputs
            s.state.store(CeltSpeculation::Inputs, std::memory_order_seq_cst);
            RunQabJob(job, s.trialRunner, &s.seed, &s.cancel);
        }
        // the seed of the frame after: this frame's final range (only raw bits follow); also when
        // unusable (then a different value, which the check rejects if it is used)
        if (job.cascaded)
            s.cascade->seed.Fulfil(job.enc.Range());
        // -> Done or Off
        s.state.store(usable && job.ran && s.cancel.load(std::memory_order_acquire) == 0 ? CeltSpeculation::Done : CeltSpeculation::Off,
                      std::memory_order_seq_cst);
    }

    bool SameQabInputs(const QabInputs& a, const QabInputs& b) noexcept
    {
        if (a.start != b.start || a.end != b.end || a.c != b.c || a.n != b.n || a.m != b.m || a.lm != b.lm || a.shortBlocks != b.shortBlocks
            || a.spread != b.spread || a.dualStereo != b.dualStereo || a.intensity != b.intensity || a.totalBits != b.totalBits || a.balance != b.balance
            || a.codedBands != b.codedBands || a.complexity != b.complexity || a.disableInv != b.disableInv || a.range != b.range || a.tellFrac != b.tellFrac
            || a.tell != b.tell || a.storage != b.storage)
            return false;
        const std::int32_t start = a.start;
        const std::int32_t count = a.end - a.start;
        if (std::memcmp(a.pulses + start, b.pulses + start, static_cast<std::size_t>(count) * sizeof(std::int32_t)) != 0
            || std::memcmp(a.tfRes + start, b.tfRes + start, static_cast<std::size_t>(count) * sizeof(std::int32_t)) != 0)
            return false;
        const std::int16_t* eBands = CeltMode::EBands;
        for (std::int32_t ch = 0; ch < a.c; ++ch) {
            if (std::memcmp(a.bandE + ch * CeltEncoder::NbEBands + start, b.bandE + ch * CeltEncoder::NbEBands + start,
                            static_cast<std::size_t>(count) * sizeof(float))
                    != 0
                || std::memcmp(a.x + ch * a.n + a.m * eBands[start], b.x + ch * b.n + b.m * eBands[start],
                               static_cast<std::size_t>(a.m * (eBands[a.end] - eBands[start])) * sizeof(float))
                    != 0)
                return false;
        }
        // the precomputed thetas are what QuantAllBands would compute from x and bandE
        return true;
    }

    void RunQabJob(QabJob& job, ThetaTrialRunner* runner, SeedPromise* seedPromise, const std::atomic<std::int32_t>* abort) noexcept
    {
        QabInputs& q = job.in;
        job.log.Attach(job.ops, QabJob::MaxOps);
        job.enc.SetLog(&job.log);
        std::uint8_t  collapseMasks[CeltEncoder::NbEBands * CeltEncoder::MaxChannels];
        std::uint32_t seed = 0;
        // the inputs stay as they were (for the check)
        std::memcpy(job.x, q.x, static_cast<std::size_t>(q.c * q.n) * sizeof(float));
        QuantAllBands(q.start, q.end, job.x, q.c == 2 ? job.x + q.n : nullptr, collapseMasks, q.bandE, q.pulses, q.shortBlocks, q.spread, q.dualStereo,
                      q.intensity, q.tfRes, q.totalBits, q.balance, &job.enc, q.lm, q.codedBands, &seed, q.complexity, q.disableInv, runner,
                      q.hasItheta ? q.stereoItheta : nullptr, seedPromise, abort);
        job.enc.SetLog(nullptr);
        job.ran = !job.log.Overflowed() && (abort == nullptr || abort->load(std::memory_order_acquire) == 0);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
