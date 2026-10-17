// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#pragma once

#include <array>
#include <cstdint>

#include "HuffmanTables.h"
#include "QuantizeKernels.h"
#include "Types.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    // Constants of l3side.h, encoder.h, quantize_pvt.h and util.h.
    inline constexpr std::int32_t SFBMAX       = SBMAX_s * 3;
    inline constexpr std::int32_t SBPSY_l      = 21;
    inline constexpr std::int32_t SBPSY_s      = 12;
    inline constexpr std::int32_t IXMAX_VAL    = 8206; // ix always <= 8191 + 15, see count_bits
    inline constexpr std::int32_t PRECALC_SIZE = IXMAX_VAL + 2;
    inline constexpr std::int32_t Q_MAX        = 256 + 1;
    inline constexpr std::int32_t Q_MAX2 = 116; // minimum possible number of -cod_info->global_gain + ((scalefactor[]+(cod_info->preflag?pretab[sfb]:0)) <<
                                                // (cod_info->scalefac_scale + 1)) + cod_info->subblock_gain[cod_info->window[sfb]] * 8
    inline constexpr std::int32_t LARGE_BITS           = 100000;
    inline constexpr std::int32_t MAX_BITS_PER_CHANNEL = 4095;
    inline constexpr std::int32_t MAX_BITS_PER_GRANULE = 7680;

    /// <summary>Table B.6: layer 3 preemphasis (pretab).</summary>
    extern const std::array<std::int32_t, SBMAX_l> pretab;

    /// <summary>A value per scalefactor band (scalefactors, their limits and work copies).</summary>
    using SfbValues = std::array<std::int32_t, SFBMAX>;

    /// <summary>gr_info: side information and quantization data of one granule and channel.</summary>
    struct GrInfo {
        std::array<float, 576>           xr;
        std::array<std::int32_t, 576>    l3_enc;
        std::array<std::int32_t, SFBMAX> scalefac;
        float                            xrpow_max;

        std::int32_t                    part2_3_length;
        std::int32_t                    big_values;
        std::int32_t                    count1;
        std::int32_t                    global_gain;
        std::int32_t                    scalefac_compress;
        std::int32_t                    block_type;
        std::int32_t                    mixed_block_flag;
        std::array<std::int32_t, 3>     table_select;
        std::array<std::int32_t, 3 + 1> subblock_gain;
        std::int32_t                    region0_count;
        std::int32_t                    region1_count;
        std::int32_t                    preflag;
        std::int32_t                    scalefac_scale;
        std::int32_t                    count1table_select;

        std::int32_t                     part2_length;
        std::int32_t                     sfb_lmax;
        std::int32_t                     sfb_smin;
        std::int32_t                     psy_lmax;
        std::int32_t                     sfbmax;
        std::int32_t                     psymax;
        std::int32_t                     sfbdivide;
        std::array<std::int32_t, SFBMAX> width;
        std::array<std::int32_t, SFBMAX> window;
        std::int32_t                     count1bits;

        std::int32_t                     max_nonzero_coeff;
        std::array<std::uint8_t, SFBMAX> energy_above_cutoff;
    };

    /// <summary>
    /// III_side_info_t: the side information of one frame. main_data_begin is the value written
    /// into this frame's header; the value carried to the next frame is in ReservoirState.
    /// </summary>
    struct SideInfo {
        std::array<std::array<GrInfo, 2>, 2>       tt;
        std::int32_t                               main_data_begin;
        std::int32_t                               private_bits;
        std::int32_t                               resvDrain_pre;
        std::int32_t                               resvDrain_post;
        std::array<std::array<std::int32_t, 4>, 2> scfsi;
    };

    /// <summary>
    /// What the scalefactor search of one granule and channel leaves for the out-of-bits
    /// strategy of the commit: the step distribution, its lower limits, the
    /// lowest gains of block_sf, the error flag and the bits the granule uses as it is.
    /// </summary>
    struct GranuleSearch {
        std::array<std::int32_t, SFBMAX> sfwork;
        std::array<std::int32_t, SFBMAX> vbrsfmin;
        std::int32_t                     mingainL;
        std::array<std::int32_t, 3>      mingainS;
        std::int32_t                     usedBits; // after ReduceBitUsage
        bool                             active;   // not silent: searched and quantized
        bool                             failed;   // LAME's "INTERNAL ERROR IN VBR NEW CODE"
    };

    /// <summary>
    /// The tables iteration_init computes once (LAME keeps most of them global): powers for the
    /// quantizer, the region split of huffman_init and the packed Huffman code lengths.
    /// </summary>
    struct QuantizeConstants {
        std::array<float, Q_MAX + Q_MAX2 + 1> pow20;  // 2^((i - 210 - Q_MAX2) / 4)
        std::array<float, Q_MAX>              ipow20; // 2^(-(i - 210) * 3 / 16)
        std::array<float, PRECALC_SIZE>       pow43;  // i^(4/3)
        std::array<float, PRECALC_SIZE>       adj43;  // rounding adjustment for the truncating quantizer
        std::array<std::int32_t, 576>         bv_scf; // region0/region1 count by big_values (sv_qnt.bv_scf)
        HuffmanPairLengths                    pairs;
        QuantizeKernels                       kernels; // scalar after QuantizeInit, QuantizeKernelsFor(level) for SIMD
    };

    /// <summary>
    /// EncResult_t: what the encoder reports in total. bitrate_index and mode_ext are those of the
    /// last frame written (the flush and the LAME tag use them); the values of each frame are in
    /// FrameData.
    /// </summary>
    struct EncoderResult {
        std::array<std::array<std::int32_t, 4 + 1>, 16>     bitrate_channelmode_hist;
        std::array<std::array<std::int32_t, 4 + 1 + 1>, 16> bitrate_blocktype_hist; // norm/start/short/stop/mixed(short)/sum
        std::int32_t                                        bitrate_index;
        std::int32_t                                        frame_number;
        std::int32_t                                        mode_ext;
        std::int32_t                                        encoder_delay;
        std::int32_t                                        encoder_padding;
    };

    // mode_ext values
    inline constexpr std::int32_t MPG_MD_LR_LR = 0;
    inline constexpr std::int32_t MPG_MD_MS_LR = 2;

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
