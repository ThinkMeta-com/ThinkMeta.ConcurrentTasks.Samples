// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

// The speculation of the CELT encoder: the next frame runs ahead on another thread while this
// frame's residual quantisation (QuantAllBands, the longest serial step) is still coded.
//
// What it rests on: of everything frame N+1 takes from frame N, only two things depend on N's
// residual quantisation - the bits left for the energy finalisation (the end of N's energies,
// FinishEnergies) and the seed of the folding noise (N's final range). The speculation builds
// N+1's state from N before N's residual quantisation, with GuessBitsLeft bits left for the
// finalisation, and runs N+1 on a copy of the encoder (clone) up to its own residual quantisation.
// That quantisation runs as a job (QabJob) whose range coder logs its operations; the seed is a
// SeedPromise that QuantAllBands waits for at its first use. The real frame N+1 replays the log
// into its own coder only if the job read exactly its inputs (SameQabInputs) and, if the job used
// the seed, it was N+1's real seed. Otherwise the job is dropped and N+1 quantises itself. The
// output therefore never depends on the speculation, only the time does: a wrong guess costs the
// speculative work, never a bit.
//
// A speculative run may itself request the speculation of the frame after it (cascade, up to
// MaxAfter levels): its clone has the next level's CeltSpeculation, and the seed of that level is
// the final range of this level's job.
//
// The states (CeltSpeculation::state) and who changes them:
//
//   Idle    -> Queued   the encoder (BuildSpeculation; the request is set, then Post to the runner)
//   Queued  -> Running  the runner (RunSpeculation, by CAS: it starts the run)
//   Queued  -> Off      the encoder (by CAS: it takes the request back before the runner starts it)
//   Running -> Inputs   the runner (job.in is set; the job's QuantAllBands runs)
//   Running -> Off      the runner (no usable inputs: the run was cancelled or ended early)
//   Inputs  -> Done     the runner (job.log holds the operations)
//   Inputs  -> Off      the runner (cancelled, aborted or the log overflowed)
//   Done/Off -> Idle    the encoder (once it has taken or dropped the result)
//
// The encoder cancels a run it will not use (cancel = 1): the job stops at its next band and no
// longer waits for the seed. The seed is published by whoever quantises the frame before: the
// encoder after its QuantAllBands, or the run of the speculation it was cascaded from (also
// when that run is unusable, then with a value the check rejects).
//
// The encoder's spins (_mm_pause, unbounded) are safe because of one invariant: it spins only
// after its CAS Queued -> Off failed, that is, after the runner has taken the speculation. The
// awaited run is then executing and needs nothing further from the spinning encoder: its seed
// was published before (by the encoder after the frame before, or by the parent run, which the
// encoder waits for first since it walks the chain in order), and a cancelled run does not wait
// for the seed at all.
//
// The memory orders: the state changes, the compare-and-swaps and the cancellation are
// sequentially consistent; the other reads acquire, the other writes release.

#include <intrin.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "CeltBands.h"
#include "CeltEncoder.h"
#include "RangeEncoder.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Opus {

    /// <summary>
    /// What QuantAllBands reads of a frame: if two runs have the same inputs (and the same seed),
    /// they code the same symbols. Arrays beyond the coded bands are not compared.
    /// </summary>
    struct QabInputs {
        std::int32_t  start;
        std::int32_t  end;
        std::int32_t  c;
        std::int32_t  n;
        std::int32_t  m;
        std::int32_t  lm;
        std::int32_t  shortBlocks;
        std::int32_t  spread;
        std::int32_t  dualStereo;
        std::int32_t  intensity;
        std::int32_t  totalBits;
        std::int32_t  balance;
        std::int32_t  codedBands;
        std::int32_t  complexity;
        std::int32_t  disableInv;
        std::uint32_t range; // the coder (its tell state and size; the decisions read nothing else)
        std::uint32_t tellFrac;
        std::int32_t  tell;
        std::uint32_t storage;
        std::int32_t  pulses[CeltEncoder::NbEBands];
        std::int32_t  tfRes[CeltEncoder::NbEBands];
        bool          hasItheta;
        std::int32_t  stereoItheta[CeltEncoder::NbEBands];
        float         bandE[CeltEncoder::NbEBands * CeltEncoder::MaxChannels];
        float         x[CeltEncoder::MaxChannels * 960];
    };

    /// <summary>The inputs are the same as far as QuantAllBands reads them.</summary>
    bool SameQabInputs(const QabInputs& a, const QabInputs& b) noexcept;

    /// <summary>QuantAllBands of a frame elsewhere: its inputs, its coder and, once run, its coding operations.</summary>
    struct QabJob {
        static constexpr std::size_t MaxOps = 8192;
        QabInputs                    in;
        RangeEncoder                 enc; // the coder before (writing to bytes)
        std::uint8_t                 bytes[1276];
        float                        x[CeltEncoder::MaxChannels * 960]; // in.x as QuantAllBands leaves it (it works in place)
        bool                         valid;                             // in and enc are set
        bool                         cascaded;                          // the run also requested the speculation of the frame after
        bool                         ran;                               // log holds the operations
        SymbolLog                    log;
        SymbolLog::Op                ops[MaxOps];
    };

    /// <summary>QuantAllBands of the job, its operations logged (the seed from seed, ended early by abort).</summary>
    void RunQabJob(QabJob& job, ThetaTrialRunner* runner, SeedPromise* seed, const std::atomic<std::int32_t>* abort) noexcept;

    /// <summary>Runs the speculations elsewhere: RunSpeculation on another thread, in the order posted.</summary>
    class SpeculationRunner
    {
    public:
        virtual void Post(CeltSpeculation* spec) noexcept = 0;

    protected:
        ~SpeculationRunner() = default;
    };

    /// <summary>
    /// The speculation of a stream's CELT encoder (see the top of this file): before the residual
    /// quantisation of frame N, the state of frame N+1 is built as if N left GuessBitsLeft bits
    /// for the finalisation; frame N+1 runs on it up to its residual quantisation, which runs as a
    /// job. The real frame N+1 takes the job's operations if the job's inputs are its own.
    /// </summary>
    struct CeltSpeculation {
        static constexpr std::int32_t GuessBitsLeft = 2;
        // the states and their transitions: see the top of this file
        static constexpr std::int32_t Idle    = 0;
        static constexpr std::int32_t Queued  = 1;
        static constexpr std::int32_t Running = 2;
        static constexpr std::int32_t Inputs  = 3; // job.in is set
        static constexpr std::int32_t Done    = 4; // job.log holds the operations
        static constexpr std::int32_t Off     = 5; // no usable result

        CeltEncoder        clone; // the state for the next frame (the seed comes in seed)
        QabJob             job;
        SpeculationRunner* runner      = nullptr;
        ThetaTrialRunner*  trialRunner = nullptr;
        CeltSpeculation*   cascade     = nullptr; // the speculation of the frame after, requested by this one's run
        // the request: the frame (the encoder's count) and its expected inputs
        std::int64_t                  frame             = -1;
        const float*                  pcm               = nullptr;
        const CeltSignalFrame*        signal            = nullptr;
        const CeltFrontFrame*         front             = nullptr;
        std::int32_t                  frameSize         = 0;
        std::int32_t                  nbCompressedBytes = 0;
        RangeEncoder                  entryEnc;
        static constexpr std::int32_t MaxAfter        = 3;
        NextCeltFrame                 after[MaxAfter] = {}; // the expected inputs of the frames after (for the cascade)
        std::int32_t                  afterCount      = 0;
        alignas(64) std::atomic<std::int32_t> state   = Idle;
        alignas(64) std::atomic<std::int32_t> cancel  = 0;
        alignas(64) SeedPromise seed;

        std::int32_t State() const noexcept { return state.load(std::memory_order_acquire); }

        /// <summary>The encoder takes a queued request back (Queued -> Off); false if the runner has started it.</summary>
        bool TakeBack() noexcept
        {
            std::int32_t expected = Queued;
            return State() == Queued && state.compare_exchange_strong(expected, Off, std::memory_order_seq_cst);
        }

        /// <summary>Spins until the run has reached at least the state s (only once the runner has started it).</summary>
        void WaitFor(std::int32_t s) const noexcept
        {
            while (State() < s)
                _mm_pause();
        }
    };

    /// <summary>The speculation as requested (on the runner's thread).</summary>
    void RunSpeculation(CeltSpeculation& spec) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Opus
