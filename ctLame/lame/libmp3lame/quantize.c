/*
 * MP3 quantization
 *
 *      Copyright (c) 1999-2000 Mark Taylor
 *      Copyright (c) 1999-2003 Takehiro Tominaga
 *      Copyright (c) 2000-2019 Robert Hegemann
 *      Copyright (c) 2001-2005 Gabriel Bouvigne
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.     See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/* $Id$ */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "lame.h"
#include "machine.h"
#include "encoder.h"
#include "util.h"
#include "quantize_pvt.h"
#include "reservoir.h"
#include "bitstream.h"
#include "vbrquantize.h"
#include "quantize.h"
#include "vector/lame_intrin.h"




/* convert from L/R <-> Mid/Side */
static void
ms_convert(III_side_info_t * l3_side, int gr)
{
    int     i;
    for (i = 0; i < 576; ++i) {
        FLOAT   l, r;
        l = l3_side->tt[gr][0].xr[i];
        r = l3_side->tt[gr][1].xr[i];
        l3_side->tt[gr][0].xr[i] = (l + r) * (FLOAT) (SQRT2 * 0.5);
        l3_side->tt[gr][1].xr[i] = (l - r) * (FLOAT) (SQRT2 * 0.5);
    }
}

/************************************************************************
 *
 *      init_outer_loop()
 *  mt 6/99
 *
 *  initializes cod_info, scalefac and xrpow
 *
 *  returns 0 if all energies in xr are zero, else 1
 *
 ************************************************************************/

static int
init_xrpow(lame_internal_flags * gfc, gr_info * const cod_info, FLOAT xrpow[576])
{
    FLOAT   sum = 0;
    int     i;
    int const upper = cod_info->max_nonzero_coeff;

    assert(xrpow != NULL);
    cod_info->xrpow_max = 0;

    /*  check if there is some energy we have to quantize
     *  and calculate xrpow matching our fresh scalefactors
     */
    assert(0 <= upper && upper <= 575);
    memset(&(xrpow[upper]), 0, (576 - upper) * sizeof(xrpow[0]));


    init_xrpow_core_sse(cod_info, xrpow, upper, &sum);

    /*  return 1 if we have something to quantize, else 0
     */
    if (sum > (FLOAT) 1E-20) {
        int     j = 0;
        if (gfc->sv_qnt.substep_shaping & 2)
            j = 1;

        for (i = 0; i < cod_info->psymax; i++)
            gfc->sv_qnt.pseudohalf[i] = j;

        return 1;
    }

    memset(&cod_info->l3_enc[0], 0, sizeof(int) * 576);
    return 0;
}





static void
init_outer_loop(lame_internal_flags const *gfc, gr_info * const cod_info)
{
    int     sfb, j;
    /*  initialize fresh cod_info
     */
    cod_info->part2_3_length = 0;
    cod_info->big_values = 0;
    cod_info->count1 = 0;
    cod_info->global_gain = 210;
    cod_info->scalefac_compress = 0;
    /* mixed_block_flag, block_type was set in psymodel.c */
    cod_info->table_select[0] = 0;
    cod_info->table_select[1] = 0;
    cod_info->table_select[2] = 0;
    cod_info->subblock_gain[0] = 0;
    cod_info->subblock_gain[1] = 0;
    cod_info->subblock_gain[2] = 0;
    cod_info->subblock_gain[3] = 0; /* this one is always 0 */
    cod_info->region0_count = 0;
    cod_info->region1_count = 0;
    cod_info->preflag = 0;
    cod_info->scalefac_scale = 0;
    cod_info->count1table_select = 0;
    cod_info->part2_length = 0;
    cod_info->sfb_lmax = SBPSY_l;
    cod_info->sfb_smin = SBPSY_s;
    cod_info->psy_lmax = gfc->sv_qnt.sfb21_extra ? SBMAX_l : SBPSY_l;
    cod_info->psymax = cod_info->psy_lmax;
    cod_info->sfbmax = cod_info->sfb_lmax;
    cod_info->sfbdivide = 11;
    for (sfb = 0; sfb < SBMAX_l; sfb++) {
        cod_info->width[sfb]
            = gfc->scalefac_band.l[sfb + 1] - gfc->scalefac_band.l[sfb];
        cod_info->window[sfb] = 3; /* which is always 0. */
    }
    if (cod_info->block_type == SHORT_TYPE) {
        FLOAT   ixwork[576];
        FLOAT  *ix;

        cod_info->sfb_smin = 0;
        cod_info->sfb_lmax = 0;
        if (cod_info->mixed_block_flag) {
            /*
             *  MPEG-1:      sfbs 0-7 long block, 3-12 short blocks
             */
            cod_info->sfb_smin = 3;
            cod_info->sfb_lmax = 8;
        }
        cod_info->psymax
            = cod_info->sfb_lmax
            + 3 * ((gfc->sv_qnt.sfb21_extra ? SBMAX_s : SBPSY_s) - cod_info->sfb_smin);
        cod_info->sfbmax = cod_info->sfb_lmax + 3 * (SBPSY_s - cod_info->sfb_smin);
        cod_info->sfbdivide = cod_info->sfbmax - 18;
        cod_info->psy_lmax = cod_info->sfb_lmax;
        /* re-order the short blocks, for more efficient encoding below */
        /* By Takehiro TOMINAGA */
        /*
           Within each scalefactor band, data is given for successive
           time windows, beginning with window 0 and ending with window 2.
           Within each window, the quantized values are then arranged in
           order of increasing frequency...
         */
        ix = &cod_info->xr[gfc->scalefac_band.l[cod_info->sfb_lmax]];
        memcpy(ixwork, cod_info->xr, 576 * sizeof(FLOAT));
        for (sfb = cod_info->sfb_smin; sfb < SBMAX_s; sfb++) {
            int const start = gfc->scalefac_band.s[sfb];
            int const end = gfc->scalefac_band.s[sfb + 1];
            int     window, l;
            for (window = 0; window < 3; window++) {
                for (l = start; l < end; l++) {
                    *ix++ = ixwork[3 * l + window];
                }
            }
        }

        j = cod_info->sfb_lmax;
        for (sfb = cod_info->sfb_smin; sfb < SBMAX_s; sfb++) {
            cod_info->width[j] = cod_info->width[j + 1] = cod_info->width[j + 2]
                = gfc->scalefac_band.s[sfb + 1] - gfc->scalefac_band.s[sfb];
            cod_info->window[j] = 0;
            cod_info->window[j + 1] = 1;
            cod_info->window[j + 2] = 2;
            j += 3;
        }
    }

    cod_info->count1bits = 0;

    cod_info->max_nonzero_coeff = 575;

    /*  fresh scalefactors are all zero
     */
    memset(cod_info->scalefac, 0, sizeof(cod_info->scalefac));
}



/************************************************************************
 *
 *      get_framebits()
 *
 *  Robert Hegemann 2000-09-05
 *
 *  calculates
 *  * how many bits are available for analog silent granules
 *  * how many bits to use for the lowest allowed bitrate
 *  * how many bits each bitrate would provide
 *
 ************************************************************************/

static void
get_framebits(lame_internal_flags * gfc, int frameBits[15])
{
    SessionConfig_t const *const cfg = &gfc->cfg;
    EncResult_t *const eov = &gfc->ov_enc;
    int     bitsPerFrame, i;

    /*  always use at least this many bits per granule per channel
     *  unless we detect analog silence, see below
     */
    eov->bitrate_index = cfg->vbr_min_bitrate_index;
    bitsPerFrame = getframebits(gfc);

    /*  bits for analog silence
     */
    eov->bitrate_index = 1;
    bitsPerFrame = getframebits(gfc);

    for (i = 1; i <= cfg->vbr_max_bitrate_index; i++) {
        eov->bitrate_index = i;
        frameBits[i] = ResvFrameBegin(gfc, &bitsPerFrame);
    }
}



static int
VBR_new_prepare(lame_internal_flags * gfc,
                const FLOAT pe[2][2], const III_psy_ratio ratio[2][2],
                FLOAT l3_xmin[2][2][SFBMAX], int frameBits[16], int max_bits[2][2],
                int* max_resv)
{
    SessionConfig_t const *const cfg = &gfc->cfg;
    EncResult_t *const eov = &gfc->ov_enc;

    int     gr, ch;
    int     analog_silence = 1;
    int     avg, bits = 0;
    int     maximum_framebits;

    eov->bitrate_index = cfg->vbr_max_bitrate_index;
    (void) ResvFrameBegin(gfc, &avg);
    *max_resv = gfc->sv_enc.ResvMax;

    get_framebits(gfc, frameBits);
    maximum_framebits = frameBits[cfg->vbr_max_bitrate_index];

    for (gr = 0; gr < 2; gr++) {
        (void) on_pe(gfc, pe, max_bits[gr], avg, gr);
        if (gfc->ov_enc.mode_ext == MPG_MD_MS_LR) {
            ms_convert(&gfc->l3_side, gr);
        }
        for (ch = 0; ch < cfg->channels_out; ++ch) {
            gr_info *const cod_info = &gfc->l3_side.tt[gr][ch];

            gfc->sv_qnt.masking_lower = pow(10.0, gfc->sv_qnt.mask_adjust * 0.1);

            init_outer_loop(gfc, cod_info);
            if (0 != calc_xmin(gfc, &ratio[gr][ch], cod_info, l3_xmin[gr][ch]))
                analog_silence = 0;

            bits += max_bits[gr][ch];
        }
    }
    for (gr = 0; gr < 2; gr++) {
        for (ch = 0; ch < cfg->channels_out; ch++) {
            if (bits > maximum_framebits && bits > 0) {
                max_bits[gr][ch] *= maximum_framebits;
                max_bits[gr][ch] /= bits;
            }

        }               /* for ch */
    }                   /* for gr */
    if (analog_silence) {
        *max_resv = 0;
    }
    return analog_silence;
}



void
VBR_new_iteration_loop(lame_internal_flags * gfc, const FLOAT pe[2][2],
                       const FLOAT ms_ener_ratio[2], const III_psy_ratio ratio[2][2])
{
    SessionConfig_t const *const cfg = &gfc->cfg;
    EncResult_t *const eov = &gfc->ov_enc;
    FLOAT   l3_xmin[2][2][SFBMAX];

    FLOAT   xrpow[2][2][576];
    int     frameBits[16];
    int     used_bits;
    int     max_bits[2][2];
    int     ch, gr, analog_silence, pad;
    III_side_info_t *const l3_side = &gfc->l3_side;

    const FLOAT (*const_l3_xmin)[2][SFBMAX] = (const FLOAT (*)[2][SFBMAX])l3_xmin;
    const FLOAT (*const_xrpow)[2][576] = (const FLOAT (*)[2][576])xrpow;
    const int (*const_max_bits)[2] = (const int (*)[2])max_bits;
    
    (void) ms_ener_ratio; /* not used */

    memset(xrpow, 0, sizeof(xrpow));

    analog_silence = VBR_new_prepare(gfc, pe, ratio, l3_xmin, frameBits, max_bits, &pad);

    for (gr = 0; gr < 2; gr++) {
        for (ch = 0; ch < cfg->channels_out; ch++) {
            gr_info *const cod_info = &l3_side->tt[gr][ch];

            /*  init_outer_loop sets up cod_info, scalefac and xrpow
             */
            if (0 == init_xrpow(gfc, cod_info, xrpow[gr][ch])) {
                max_bits[gr][ch] = 0; /* silent granule needs no bits */
            }
        }               /* for ch */
    }                   /* for gr */

    /*  quantize granules with lowest possible number of bits
     */

    used_bits = VBR_encode_frame(gfc, const_xrpow, const_l3_xmin, const_max_bits);

    {
        int     i, j;

        /*  find lowest bitrate able to hold used bits
         */
        if (analog_silence && !cfg->enforce_min_bitrate) {
            /*  we detected analog silence and the user did not specify
             *  any hard framesize limit, so start with smallest possible frame
             */
            i = 1;
        }
        else {
            i = cfg->vbr_min_bitrate_index;
        }

        for (; i < cfg->vbr_max_bitrate_index; i++) {
            if (used_bits <= frameBits[i]) 
                break;
        }
        if (i > cfg->vbr_max_bitrate_index) {
            i = cfg->vbr_max_bitrate_index;
        }
        if (pad > 0) {
            for (j = cfg->vbr_max_bitrate_index; j > i; --j) {
                int const unused = frameBits[j] - used_bits;
                if (unused <= pad) 
                    break;
            }
            eov->bitrate_index = j;
        }
        else {
            eov->bitrate_index = i;
        }
    }
    if (used_bits <= frameBits[eov->bitrate_index]) {
        /* update Reservoire status */
        int     mean_bits, fullframebits;
        fullframebits = ResvFrameBegin(gfc, &mean_bits);
        assert(used_bits <= fullframebits);
        for (gr = 0; gr < 2; gr++) {
            for (ch = 0; ch < cfg->channels_out; ch++) {
                gr_info const *const cod_info = &l3_side->tt[gr][ch];
                ResvAdjust(gfc, cod_info);
            }
        }
        ResvFrameEnd(gfc, mean_bits);
    }
    else {
        /* SHOULD NOT HAPPEN INTERNAL ERROR
         */
        ERRORF(gfc, "INTERNAL ERROR IN VBR NEW CODE, please send bug report\n");
        exit(-1);
    }
}





