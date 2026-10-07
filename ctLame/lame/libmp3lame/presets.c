/*
 * presets.c -- Apply presets
 *
 *	Copyright (c) 2002-2008 Gabriel Bouvigne
 *	Copyright (c) 2007-2012 Robert Hegemann
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
 */


#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "lame.h"
#include "machine.h"
#include "set_get.h"
#include "encoder.h"
#include "util.h"
#include "lame_global_flags.h"

#define SET_OPTION(opt, val, def) if (enforce) \
    (void) lame_set_##opt(gfp, val); \
    else if (!(fabs(lame_get_##opt(gfp) - def) > 0)) \
    (void) lame_set_##opt(gfp, val);

#define SET__OPTION(opt, val, def) if (enforce) \
    lame_set_##opt(gfp, val); \
    else if (!(fabs(lame_get_##opt(gfp) - def) > 0)) \
    lame_set_##opt(gfp, val);

#undef Min
#undef Max

typedef struct {
    int     vbr_q;
    int     expY;
    FLOAT   st_lrm;          /*short threshold */
    FLOAT   st_s;
    FLOAT   masking_adj;
    FLOAT   masking_adj_short;
    FLOAT   ath_lower;
    FLOAT   ath_curve;
    FLOAT   ath_sensitivity;
    FLOAT   interch;
    int     safejoint;
    int     sfb21mod;
    FLOAT   msfix;
    FLOAT   minval;
    FLOAT   ath_fixpoint;
} vbr_presets_t;

    /* *INDENT-OFF* */
    
    static const vbr_presets_t vbr_mt_psy_switch_map[] = {
    /*vbr_q  expY  st_lrm   st_s  mask adj_l  adj_s  ath_lower  ath_curve  ath_sens  ---  safejoint sfb21mod  msfix */
        {0,    0,   4.20,  25.0,      -6.8,   -6.8,       7.1,       1,          0,   0,         2,      31,  1.000,  5, 100},
        {1,    0,   4.20,  25.0,      -4.8,   -4.8,       5.4,       1.4,       -1,   0,         2,      27,  1.122,  5,  98},
        {2,    0,   4.20,  25.0,      -2.6,   -2.6,       3.7,       2.0,       -3,   0,         2,      23,  1.288,  5,  97},
        {3,    1,   4.20,  25.0,      -1.6,   -1.6,       2.0,       2.0,       -5,   0,         2,      18,  1.479,  5,  96},
        {4,    1,   4.20,  25.0,      -0.0,   -0.0,       0.0,       2.0,       -8,   0,         2,      12,  1.698,  5,  95},
        {5,    1,   4.20,  25.0,       1.3,    1.3,      -6,         3.5,      -11,   0,         2,       8,  1.950,  5,  94.2},
#if 0
        {6,    1,   4.50, 100.0,       1.5,    1.5,     -24.0,       6.0,      -14,   0,         2,       4,  2.239,  3,  93.9},
        {7,    1,   4.80, 200.0,       1.7,    1.7,     -28.0,       9.0,      -20,   0,         2,       0,  2.570,  1,  93.6},
#else
        {6,    1,   4.50, 100.0,       2.2,    2.3,     -12.0,       6.0,      -14,   0,         2,       4,  2.239,  3,  93.9},
        {7,    1,   4.80, 200.0,       2.7,    2.7,     -18.0,       9.0,      -17,   0,         2,       0,  2.570,  1,  93.6},
#endif
        {8,    1,   5.30, 300.0,       2.8,    2.8,     -21.0,      10.0,      -23,   0.0002,    0,       0,  2.951,  0,  93.3},
        {9,    1,   6.60, 300.0,       2.8,    2.8,     -23.0,      11.0,      -25,   0.0006,    0,       0,  3.388,  0,  93.3},
        {10,    1,  25.00, 300.0,       2.8,    2.8,     -25.0,      12.0,      -27,   0.0025,    0,       0,  3.500,  0,  93.3}
    };

    /* *INDENT-ON* */

#define NOOP(m) (void)p.m
#define LERP(m) (p.m = p.m + x * (q.m - p.m))

static void
apply_vbr_preset(lame_global_flags * gfp, int a, int enforce)
{
    vbr_presets_t const *vbr_preset = &vbr_mt_psy_switch_map[0];
    float   x = gfp->VBR_q_frac;
    vbr_presets_t p = vbr_preset[a];
    vbr_presets_t q = vbr_preset[a + 1];
    vbr_presets_t const *set = &p;

    NOOP(vbr_q);
    NOOP(expY);
    LERP(st_lrm);
    LERP(st_s);
    LERP(masking_adj);
    LERP(masking_adj_short);
    LERP(ath_lower);
    LERP(ath_curve);
    LERP(ath_sensitivity);
    LERP(interch);
    NOOP(safejoint);
    LERP(sfb21mod);
    LERP(msfix);
    LERP(minval);
    LERP(ath_fixpoint);

    (void) lame_set_VBR_q(gfp, set->vbr_q);
    if (set->expY) {
        (void) lame_set_experimentalY(gfp, set->expY);
    }
    SET_OPTION(short_threshold_lrm, set->st_lrm, -1);
    SET_OPTION(short_threshold_s, set->st_s, -1);
    SET_OPTION(maskingadjust, set->masking_adj, 0);
    SET_OPTION(maskingadjust_short, set->masking_adj_short, 0);
    lame_set_ATHtype(gfp, 5);
    SET_OPTION(ATHlower, set->ath_lower, 0);
    SET_OPTION(ATHcurve, set->ath_curve, -1);
    SET_OPTION(athaa_sensitivity, set->ath_sensitivity, 0);
    if (set->interch > 0) {
        SET_OPTION(interChRatio, set->interch, -1);
    }

    /* parameters for which there is no proper set/get interface */
    if (set->safejoint > 0) {
        (void) lame_set_exp_nspsytune(gfp, lame_get_exp_nspsytune(gfp) | 2);
    }
    if (set->sfb21mod > 0) {
        int const nsp = lame_get_exp_nspsytune(gfp);
        int const val = (nsp >> 20) & 63;
        if (val == 0) {
            int const sf21mod = (set->sfb21mod << 20) | nsp;
            (void) lame_set_exp_nspsytune(gfp, sf21mod);
        }
    }
    SET__OPTION(msfix, set->msfix, -1);

    if (enforce == 0) {
        gfp->VBR_q = a;
        gfp->VBR_q_frac = x;
    }
    gfp->internal_flags->cfg.minval = set->minval;
    {   /* take care of gain adjustments */
        double const scale_abs = fabs(gfp->scale);
        double const y = (scale_abs > 0.f) ? (10.f * log10(scale_abs)) : 0.f;
        gfp->internal_flags->cfg.ATHfixpoint = set->ath_fixpoint - y;
    }
}

int
apply_preset(lame_global_flags * gfp, int preset, int enforce)
{
    /*translate legacy presets */
    switch (preset) {
    case R3MIX:
        {
            preset = V3;
            break;
        }
    case MEDIUM:
    case MEDIUM_FAST:
        {
            preset = V4;
            break;
        }
    case STANDARD:
    case STANDARD_FAST:
        {
            preset = V2;
            break;
        }
    case EXTREME:
    case EXTREME_FAST:
        {
            preset = V0;
            break;
        }
    }

    gfp->preset = preset;
    {
        switch (preset) {
        case V9:
            apply_vbr_preset(gfp, 9, enforce);
            return preset;
        case V8:
            apply_vbr_preset(gfp, 8, enforce);
            return preset;
        case V7:
            apply_vbr_preset(gfp, 7, enforce);
            return preset;
        case V6:
            apply_vbr_preset(gfp, 6, enforce);
            return preset;
        case V5:
            apply_vbr_preset(gfp, 5, enforce);
            return preset;
        case V4:
            apply_vbr_preset(gfp, 4, enforce);
            return preset;
        case V3:
            apply_vbr_preset(gfp, 3, enforce);
            return preset;
        case V2:
            apply_vbr_preset(gfp, 2, enforce);
            return preset;
        case V1:
            apply_vbr_preset(gfp, 1, enforce);
            return preset;
        case V0:
            apply_vbr_preset(gfp, 0, enforce);
            return preset;
        default:
            break;
        }
    }
    gfp->preset = 0;    /*no corresponding preset found */
    return preset;
}
