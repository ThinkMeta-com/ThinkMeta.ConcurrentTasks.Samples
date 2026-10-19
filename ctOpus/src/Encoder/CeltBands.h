// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <atomic>
#include <cstdint>

#include "RangeEncoder.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    constexpr std::int32_t SpreadNone       = 0;
    constexpr std::int32_t SpreadLight      = 1;
    constexpr std::int32_t SpreadNormal     = 2;
    constexpr std::int32_t SpreadAggressive = 3;

    // Port of the band functions of libopus 1.6.1 (celt/bands.c) for the static 48 kHz mode.

    /// <summary>hysteresis_decision.</summary>
    std::int32_t HysteresisDecision(float val, const float* thresholds, const float* hysteresis, std::int32_t n, std::int32_t prev) noexcept;

    /// <summary>compute_band_energies: the amplitude (sqrt energy) of each band.</summary>
    void ComputeBandEnergies(const float* x, float* bandE, std::int32_t end, std::int32_t c, std::int32_t lm) noexcept;

    /// <summary>normalise_bands: every band to unit energy.</summary>
    void NormaliseBands(const float* freq, float* x, const float* bandE, std::int32_t end, std::int32_t c, std::int32_t m) noexcept;

    /// <summary>haar1: one level of the Haar transform over stride interleaved signals.</summary>
    void Haar1(float* x, std::int32_t n0, std::int32_t stride) noexcept;

    /// <summary>spreading_decision.</summary>
    std::int32_t SpreadingDecision(const float* x, std::int32_t* average, std::int32_t lastDecision, std::int32_t* hfAverage, std::int32_t* tapsetDecision,
                                   bool updateHf, std::int32_t end, std::int32_t c, std::int32_t m, const std::int32_t* spreadWeight) noexcept;

    /// <summary>
    /// The theta of a stereo band before its quantisation (Q30), as quant_band_stereo computes it
    /// first: with the louder channel for both if one is almost silent.
    /// </summary>
    std::int32_t BandStereoItheta(const float* x, const float* y, std::int32_t n, float bandEx, float bandEy) noexcept;

    /// <summary>
    /// A seed that is set later (by another thread): QuantAllBands waits for it at its first use
    /// (see CeltSpeculation.h).
    /// </summary>
    struct SeedPromise {
        std::atomic<std::int32_t>  ready = 0;
        std::atomic<std::uint32_t> value = 0;
        std::atomic<std::int32_t>  used  = 0; // waited for (the result depends on it)

        /// <summary>Before a new request: no seed yet, none used.</summary>
        void Reset() noexcept
        {
            ready.store(0, std::memory_order_release);
            used.store(0, std::memory_order_release);
        }

        /// <summary>Publishes the seed.</summary>
        void Fulfil(std::uint32_t seed) noexcept
        {
            value.store(seed, std::memory_order_release);
            ready.store(1, std::memory_order_seq_cst);
        }
    };

    /// <summary>One theta RDO trial of a stereo band (rounding up), see QuantAllBands.</summary>
    struct ThetaTrial;

    /// <summary>
    /// Runs theta RDO trials on another thread: Start hands it the trial (RunThetaTrial there),
    /// Finish waits for it. One trial at a time.
    /// </summary>
    class ThetaTrialRunner
    {
    public:
        /// <summary>The storage of the trial (NewThetaTrial), the same every time.</summary>
        virtual ThetaTrial* Trial() noexcept                  = 0;
        virtual void        Start(ThetaTrial* trial) noexcept = 0;
        virtual void        Finish() noexcept                 = 0;

    protected:
        ~ThetaTrialRunner() = default;
    };

    /// <summary>The trial itself (on the runner's thread).</summary>
    void RunThetaTrial(ThetaTrial* trial) noexcept;

    /// <summary>The storage of a trial (nullptr if out of memory), and its release.</summary>
    ThetaTrial* NewThetaTrial() noexcept;
    void        DeleteThetaTrial(ThetaTrial* trial) noexcept;

    /// <summary>
    /// quant_all_bands (encoding): the PVQ residual of all bands, with theta splits, stereo and
    /// folding; yAll is the second channel or nullptr. collapseMasks gets one byte per band and channel.
    /// With a runner, the two theta RDO trials of a stereo band (complexity 8 and more) run at the
    /// same time; the result is the same. stereoItheta (or nullptr): BandStereoItheta of every
    /// band of xAll and yAll as given, used with the resynthesis only (else the last band serves
    /// as scratch space before it is coded). With seedPromise, the seed comes from there (when it is
    /// first needed); abort (if set) ends the quantisation at the next band, its result unusable.
    /// </summary>
    void QuantAllBands(std::int32_t start, std::int32_t end, float* xAll, float* yAll, std::uint8_t* collapseMasks, const float* bandE,
                       const std::int32_t* pulses, std::int32_t shortBlocks, std::int32_t spread, std::int32_t dualStereo, std::int32_t intensity,
                       const std::int32_t* tfRes, std::int32_t totalBits, std::int32_t balance, RangeEncoder* ec, std::int32_t lm, std::int32_t codedBands,
                       std::uint32_t* seed, std::int32_t complexity, std::int32_t disableInv, ThetaTrialRunner* runner = nullptr,
                       const std::int32_t* stereoItheta = nullptr, SeedPromise* seedPromise = nullptr,
                       const std::atomic<std::int32_t>* abort = nullptr) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
