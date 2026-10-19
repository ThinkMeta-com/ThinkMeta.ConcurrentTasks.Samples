// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <array>
#include <cstdint>

#include "EncoderOptions.h"
#include "Tables.h"
#include "Types.h"

// Naming in the encoder (ThinkMeta.ConcurrentTasks.Samples.ctLame.Encoder):
// - State and configuration ported from LAME keep LAME's names, snake_case included
//   (SessionConfig::samplerate_out, GrInfo::part2_3_length, PsyState::nb_l1, ATH_offset_db), so
//   a field can be looked up in LAME's sources as it is.
// - What ctLame adds has C++ names: functions, classes and structures (FrameData, PsyPrep,
//   PsyGranuleOutput, ReservoirState), their members (camelCase) and local variables.
// - A ported function has a C++ name and names its LAME original in its comment or summary
//   ("LAME: pecalc_s() (psymodel.c)" or "pecalc_s: ..."); constants of LAME keep their names
//   (SBMAX_l, FFTOFFSET, NS_PREECHO_ATT0).

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    /// <summary>
    /// Port of SessionConfig_t plus the values lame_init_params derives outside of it (in the
    /// ATH, quantizer and scalefactor band state). It is filled once by InitParams and stays
    /// constant while encoding.
    /// </summary>
    struct SessionConfig {
        std::int32_t samplerate_index = 0;
        std::int32_t sideinfo_len     = 0;

        std::int32_t noise_shaping    = 0;
        std::int32_t subblock_gain    = 0;
        std::int32_t use_best_huffman = 0;
        std::int32_t full_outer_loop  = 0;

        std::int32_t lowpassfreq    = 0;
        std::int32_t highpassfreq   = 0;
        std::int32_t samplerate_in  = 0;
        std::int32_t samplerate_out = 0;
        std::int32_t channels_in    = 0;
        std::int32_t channels_out   = 0;
        std::int32_t force_ms       = 0;

        std::int32_t use_temporal_masking_effect = 0;
        std::int32_t use_safe_joint_stereo       = 0;

        std::int32_t preset = 0;

        std::int32_t vbr_min_bitrate_index = 1;
        std::int32_t vbr_max_bitrate_index = 13;
        std::int32_t avg_bitrate           = 0;
        std::int32_t enforce_min_bitrate   = 0;

        std::int32_t disable_reservoir = 0;
        std::int32_t buffer_constraint = 0;
        std::int32_t write_lame_tag    = 0;

        std::int32_t error_protection = 0;
        std::int32_t copyright        = 0;
        std::int32_t original         = 0;
        std::int32_t extension        = 0;
        std::int32_t emphasis         = 0;

        MpegMode   mode         = NotSet;
        ShortBlock short_blocks = ShortBlockNotSet;

        float        interChRatio      = 0;
        float        msfix             = 0;
        float        ATH_offset_db     = 0;
        float        ATH_offset_factor = 0;
        float        ATHcurve          = 0;
        std::int32_t ATHtype           = 0;
        std::int32_t ATHonly           = 0;
        std::int32_t ATHshort          = 0;
        std::int32_t noATH             = 0;

        float ATHfixpoint = 0;

        float adjust_alto_db   = 0;
        float adjust_bass_db   = 0;
        float adjust_treble_db = 0;
        float adjust_sfb21_db  = 0;

        float lowpass1 = 0, lowpass2 = 0;
        float highpass1 = 0, highpass2 = 0;

        std::array<std::array<float, 2>, 2> pcm_transform = {};

        float minval = 0;

        // --- derived outside of SessionConfig_t in LAME ---

        /// <summary>sv_enc.amp_filter: polyphase filter amplitude per subband.</summary>
        std::array<float, 32> amp_filter = {};

        /// <summary>scalefac_band.</summary>
        ScalefacStruct scalefac_band = {};

        /// <summary>ATH->use_adjust and ATH->aa_sensitivity_p.</summary>
        std::int32_t ath_use_adjust       = 0;
        float        ath_aa_sensitivity_p = 0;

        /// <summary>sv_qnt.mask_adjust, mask_adjust_short, sfb21_extra, substep_shaping.</summary>
        float        mask_adjust       = 0;
        float        mask_adjust_short = 0;
        std::int32_t sfb21_extra       = 0;
        std::int32_t substep_shaping   = 0;

        /// <summary>sv_qnt.longfact and shortfact (masking adjustment per band, iteration_init).</summary>
        std::array<float, SBMAX_l> longfact  = {};
        std::array<float, SBMAX_s> shortfact = {};

        /// <summary>
        /// The global flags as lame_init_params left them. The psychoacoustic model and the
        /// frontend read some of them (thresholds, quality, -Z, scale, VBR quality).
        /// </summary>
        EncoderOptions options;
    };

    /// <summary>
    /// Port of lame_init_params, up to the parts that belong to later stages (bitstream,
    /// psychoacoustic model and quantizer tables). Completes the options exactly like LAME
    /// changes the global flags and fills the configuration. Returns 0 or a negative LAME error.
    /// </summary>
    std::int32_t InitParams(EncoderOptions& options, SessionConfig& config);

    /// <summary>
    /// Maximum size of the frame buffer in bits for a buffer constraint
    /// (get_max_frame_buffer_size_by_constraint).
    /// </summary>
    std::int32_t GetMaxFrameBufferSizeByConstraint(const SessionConfig& config, std::int32_t constraint) noexcept;

    /// <summary>Frame length in bits (calcFrameLength).</summary>
    std::int32_t CalcFrameLength(const SessionConfig& config, std::int32_t kbps, std::int32_t pad) noexcept;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
