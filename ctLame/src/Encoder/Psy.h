// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <array>
#include <cstdint>

#include "Fft.h"
#include "PsyKernels.h"
#include "Types.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    struct SessionConfig;

    inline constexpr std::int32_t CBANDS = 64;

    /// <summary>III_psy_xmin: a value per scalefactor band, long and short.</summary>
    struct PsyXmin {
        std::array<float, SBMAX_l>                l;
        std::array<std::array<float, 3>, SBMAX_s> s;
    };

    /// <summary>III_psy_ratio: masking threshold and energy per scalefactor band.</summary>
    struct PsyRatio {
        PsyXmin thm;
        PsyXmin en;
    };

    /// <summary>PsyConst_CB2SB_t: partition band layout and constants for long or short blocks.</summary>
    struct PsyPartitions {
        std::array<float, CBANDS>                       masking_lower;
        std::array<float, CBANDS>                       minval;
        std::array<float, CBANDS>                       rnumlines;
        std::array<float, CBANDS>                       mld_cb;
        std::array<float, Max(SBMAX_l, SBMAX_s)>        mld;
        std::array<float, Max(SBMAX_l, SBMAX_s)>        bo_weight; // band weight long scalefactor bands, at transition
        std::array<std::array<std::int32_t, 2>, CBANDS> s3ind;
        std::array<std::int32_t, CBANDS>                numlines;
        std::array<std::int32_t, Max(SBMAX_l, SBMAX_s)> bm;
        std::array<std::int32_t, Max(SBMAX_l, SBMAX_s)> bo;
        std::int32_t                                    npart;
        std::int32_t                                    n_sb; // SBMAX_l or SBMAX_s
        std::array<float, CBANDS * CBANDS>              s3;   // the non-zero part of the spreading function, row by row
    };

    /// <summary>PsyConst_t: constants of the psychoacoustic model, set up by PsyInit.</summary>
    struct PsyConstants {
        FftWindows           fft;
        PsyPartitions        l;
        PsyPartitions        s;
        PsyPartitions        l_to_s;
        std::array<float, 4> attack_threshold;
        float                decay;
        std::int32_t         force_short_block_calc;
        AttackPrepFunction   attackPrep;  // AttackPrepScalar after PsyInit, AttackPrepFor(level) for SIMD
        MaskChainsFunction   maskChains;  // MaskChainsScalar after PsyInit, MaskChainsFor(level) for SIMD
        MaskingLongFunction  maskingLong; // MaskingLongScalar after PsyInit, MaskingLongFor(level) for SIMD
    };

    /// <summary>
    /// The constant part of ATH_t: the absolute threshold of hearing per band, set up by PsyInit
    /// and QuantizeInit. The adjustment that changes from frame to frame is AthAdjust.
    /// </summary>
    struct AthTables {
        std::int32_t               use_adjust;       // method for the auto adjustment
        float                      aa_sensitivity_p; // factor for tuning the (sample power) point below which adaptive threshold of hearing adjustment occurs
        float                      decay;            // determined to lower x dB each second
        float                      floor;            // lowest ATH value
        std::array<float, SBMAX_l> l;                // ATH for sfbs in long blocks
        std::array<float, SBMAX_s> s;                // ATH for sfbs in short blocks
        std::array<float, PSFB21>  psfb21;           // ATH for partitionned sfb21 in long blocks
        std::array<float, PSFB12>  psfb12;           // ATH for partitionned sfb12 in short blocks
        std::array<float, CBANDS>  cb_l;             // ATH for long block convolution bands
        std::array<float, CBANDS>  cb_s;             // ATH for short block convolution bands
        std::array<float, BLKSIZE / 2> eql_w;        // equal loudness weights (based on ATH)
    };

    /// <summary>The automatic ATH adjustment of ATH_t, carried from frame to frame by the psychoacoustic model.</summary>
    struct AthAdjust {
        float factor; // adjust_factor: lowering based on peak volume, 1 = no lowering
        float limit;  // adjust_limit: limit for dynamic ATH adjust
    };

    /// <summary>PsyStateVar_t: what the model carries from granule to granule.</summary>
    struct PsyState {
        std::array<std::array<float, CBANDS>, 4> nb_l1, nb_l2;
        std::array<std::array<float, CBANDS>, 4> nb_s1, nb_s2;
        std::array<PsyXmin, 4>                   thm;
        std::array<PsyXmin, 4>                   en;
        std::array<float, 2>                     loudness_sq_save; // account for granule delay of L3psycho_anal
        std::array<float, 4>                     tot_ener;
        std::array<std::array<float, 9>, 4>      last_en_subshort;
        std::array<std::int32_t, 4>              last_attacks;
        std::array<std::int32_t, 2>              blocktype_old;
    };

    /// <summary>PsyResult_t: loudness^2 approximation per granule and channel, for the ATH adjustment.</summary>
    struct PsyResult {
        std::array<std::array<float, 2>, 2> loudness_sq;
    };

    /// <summary>
    /// psymodel_init: sets up constants, state and the ATH values of the model and the start of
    /// the ATH adjustment. Like LAME it also settles config.msfix. Returns 0.
    /// </summary>
    std::int32_t PsyInit(SessionConfig& config, PsyConstants& constants, PsyState& state, AthTables& ath, AthAdjust& adjust);

    /// <summary>Energy, spread energy and tonal threshold limit per partition band of one block.</summary>
    struct PsyBandPrep {
        std::array<float, CBANDS> eb;    // energy
        std::array<float, CBANDS> ecb;   // energy convolved with the spreading function, times the average masking
        std::array<float, CBANDS> limit; // the threshold limit for strongly tonal signals
    };

    /// <summary>
    /// What L3psycho_anal_vbr computes for one granule without its state: the
    /// peaks of the sub-short blocks for the attack detection, the long FFT with its energies
    /// and the spreading, for L, R, M and S. The short blocks stay with PsyResolve: whether they
    /// are needed depends on the attacks, and they are needed rarely.
    /// </summary>
    struct PsyPrep {
        std::array<std::array<float, 9>, 4> subShort;  // peak of the high-passed signal in each sub-short block
        std::array<float, 4>                totEnergy; // tot_ener of this granule
        std::array<float, 2>                loudness;  // loudness^2 of this granule (L and R)
        std::array<PsyBandPrep, 4>          l;         // long block per channel
    };

    /// <summary>
    /// The part of L3psycho_anal_vbr without state, for one granule. buffer as for PsyAnalyze.
    /// Reads only constants, so it may run for several granules at once.
    /// </summary>
    void PsyPrepare(const SessionConfig& config, const PsyConstants& constants, const AthTables& ath, const ChannelSamples& buffer, PsyPrep& prep) noexcept;

    /// <summary>
    /// Where PsyResolve puts its results for granule gr (the output parameters of
    /// L3psycho_anal_vbr). Masking and energy are one granule late: the call for granule gr
    /// writes what the previous call computed.
    /// </summary>
    struct PsyGranuleOutput {
        GranuleChannel<PsyRatio>&    maskingRatio;   // masking_ratio: L/R masking and energy, [gr][ch] is written
        GranuleChannel<PsyRatio>&    maskingMsRatio; // masking_MS_ratio: the same for mid/side (joint stereo only)
        std::array<float, 2>&        pe;             // percep_entropy: L/R perceptual entropy of this granule
        std::array<float, 2>&        peMs;           // percep_MS_entropy: the same for mid/side
        std::array<float, 4>&        energy;         // energy: tot_ener of L, R, M, S
        std::array<std::int32_t, 2>& blocktype;      // blocktype_d: the block type of each channel
    };

    /// <summary>
    /// The part of L3psycho_anal_vbr with state, for the granules in order: attacks, block types,
    /// pre-echo control, the short blocks where they are needed (from buffer, as for
    /// PsyAnalyze), M/S thresholds, perceptual entropy. The results go to out.
    /// </summary>
    void PsyResolve(const SessionConfig& config, const PsyConstants& constants, PsyState& state, const AthTables& ath, float athAdjustFactor,
                    float maskingLower, PsyResult& result, const PsyPrep& prep, const ChannelSamples& buffer, std::int32_t gr,
                    const PsyGranuleOutput& out) noexcept;

    /// <summary>
    /// L3psycho_anal_vbr for one granule (PsyPrepare and PsyResolve). buffer points at the
    /// granule's PCM (mfbuf + 576 + gr * 576 - FFTOFFSET) of each channel. athAdjustFactor is the current ATH adjustment,
    /// maskingLower the quantizer's masking_lower of the previous frame (1 for the first frame).
    /// The masking results are one granule late.
    /// </summary>
    void PsyAnalyze(const SessionConfig& config, const PsyConstants& constants, PsyState& state, const AthTables& ath, float athAdjustFactor,
                    float maskingLower, PsyResult& result, const ChannelSamples& buffer, std::int32_t gr, GranuleChannel<PsyRatio>& maskingRatio,
                    GranuleChannel<PsyRatio>& maskingMsRatio, std::array<float, 2>& percepEntropy, std::array<float, 2>& percepMsEntropy,
                    std::array<float, 4>& energy, std::array<std::int32_t, 2>& blocktype);

    /// <summary>adjust_ATH (encoder.c): the automatic ATH adjustment after the psychoacoustic model of a frame.</summary>
    void AdjustAth(const SessionConfig& config, const AthTables& ath, AthAdjust& adjust, const PsyResult& result) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
