/*
 *	MP3 huffman table selecting and bit counting
 *
 *	Copyright (c) 1999-2005 Takehiro TOMINAGA
 *	Copyright (c) 2002-2005 Gabriel Bouvigne
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
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
#include "tables.h"


static const struct {
    const int region0_count;
    const int region1_count;
} subdv_table[23] = {
    {
    0, 0},              /* 0 bands */
    {
    0, 0},              /* 1 bands */
    {
    0, 0},              /* 2 bands */
    {
    0, 0},              /* 3 bands */
    {
    0, 0},              /* 4 bands */
    {
    0, 1},              /* 5 bands */
    {
    1, 1},              /* 6 bands */
    {
    1, 1},              /* 7 bands */
    {
    1, 2},              /* 8 bands */
    {
    2, 2},              /* 9 bands */
    {
    2, 3},              /* 10 bands */
    {
    2, 3},              /* 11 bands */
    {
    3, 4},              /* 12 bands */
    {
    3, 4},              /* 13 bands */
    {
    3, 4},              /* 14 bands */
    {
    4, 5},              /* 15 bands */
    {
    4, 5},              /* 16 bands */
    {
    4, 6},              /* 17 bands */
    {
    5, 6},              /* 18 bands */
    {
    5, 6},              /* 19 bands */
    {
    5, 7},              /* 20 bands */
    {
    6, 7},              /* 21 bands */
    {
    6, 7},              /* 22 bands */
};





/*************************************************************************/
/*	      ix_max							 */
/*************************************************************************/

static int
ix_max(const int *ix, const int *end)
{
    int     max1 = 0, max2 = 0;

    do {
        int const x1 = *ix++;
        int const x2 = *ix++;
        if (max1 < x1)
            max1 = x1;

        if (max2 < x2)
            max2 = x2;
    } while (ix < end);
    if (max1 < max2)
        max1 = max2;
    return max1;
}








static int
count_bit_ESC(const int *ix, const int *const end, int t1, const int t2, unsigned int *const s)
{
    /* ESC-table is used */
    unsigned int const linbits = ht[t1].xlen * 65536u + ht[t2].xlen;
    unsigned int sum = 0, sum2;

    do {
        unsigned int x = *ix++;
        unsigned int y = *ix++;

        if (x >= 15u) {
            x = 15u;
            sum += linbits;
        }
        if (y >= 15u) {
            y = 15u;
            sum += linbits;
        }
        x <<= 4u;
        x += y;
        sum += largetbl[x];
    } while (ix < end);

    sum2 = sum & 0xffffu;
    sum >>= 16u;

    if (sum > sum2) {
        sum = sum2;
        t1 = t2;
    }

    *s += sum;
    return t1;
}


static int
count_bit_noESC(const int *ix, const int *end, int mx, unsigned int *s)
{
    /* No ESC-words */
    unsigned int sum1 = 0;
    const uint8_t *const hlen1 = ht[1].hlen;
    (void) mx;

    do {
        unsigned int const x0 = *ix++;
        unsigned int const x1 = *ix++;
        sum1 += hlen1[ x0+x0 + x1 ];
    } while (ix < end);

    *s += sum1;
    return 1;
}


static const int huf_tbl_noESC[] = {
    1, 2, 5, 7, 7, 10, 10, 13, 13, 13, 13, 13, 13, 13, 13
};


static int
count_bit_noESC_from2(const int *ix, const int *end, int max, unsigned int *s)
{
    int t1 = huf_tbl_noESC[max - 1];
    /* No ESC-words */
    const unsigned int xlen = ht[t1].xlen;
    uint32_t const* table = (t1 == 2) ? &table23[0] : &table56[0];
    unsigned int sum = 0, sum2;

    do {
        unsigned int const x0 = *ix++;
        unsigned int const x1 = *ix++;
        sum += table[ x0 * xlen + x1 ];
    } while (ix < end);

    sum2 = sum & 0xffffu;
    sum >>= 16u;

    if (sum > sum2) {
        sum = sum2;
        t1++;
    }

    *s += sum;
    return t1;
}


inline static int
count_bit_noESC_from3(const int *ix, const int *end, int max, unsigned int * s)
{
    int t1 = huf_tbl_noESC[max - 1];
    /* No ESC-words */
    unsigned int sum1 = 0;
    unsigned int sum2 = 0;
    unsigned int sum3 = 0;
    const unsigned int xlen = ht[t1].xlen;
    const uint8_t *const hlen1 = ht[t1].hlen;
    const uint8_t *const hlen2 = ht[t1 + 1].hlen;
    const uint8_t *const hlen3 = ht[t1 + 2].hlen;
    int     t;

    do {
        unsigned int x0 = *ix++;
        unsigned int x1 = *ix++;
        unsigned int x = x0 * xlen + x1;
        sum1 += hlen1[x];
        sum2 += hlen2[x];
        sum3 += hlen3[x];
    } while (ix < end);

    t = t1;
    if (sum1 > sum2) {
        sum1 = sum2;
        t++;
    }
    if (sum1 > sum3) {
        sum1 = sum3;
        t = t1 + 2;
    }
    *s += sum1;

    return t;  
}


/*************************************************************************/
/*	      choose table						 */
/*************************************************************************/

/*
  Choose the Huffman table that will encode ix[begin..end] with
  the fewest bits.

  Note: This code contains knowledge about the sizes and characteristics
  of the Huffman tables as defined in the IS (Table B.7), and will not work
  with any arbitrary tables.
*/
static int count_bit_null(const int* ix, const int* end, int max, unsigned int* s)
{
    (void) ix;
    (void) end;
    (void) max;
    (void) s;
    return 0;
}

typedef int (*count_fnc)(const int* ix, const int* end, int max, unsigned int* s);
  
static const count_fnc count_fncs[] = 
{ &count_bit_null
, &count_bit_noESC
, &count_bit_noESC_from2
, &count_bit_noESC_from2
, &count_bit_noESC_from3
, &count_bit_noESC_from3
, &count_bit_noESC_from3
, &count_bit_noESC_from3
, &count_bit_noESC_from3
, &count_bit_noESC_from3
, &count_bit_noESC_from3
, &count_bit_noESC_from3
, &count_bit_noESC_from3
, &count_bit_noESC_from3
, &count_bit_noESC_from3
, &count_bit_noESC_from3
};

static int
choose_table(const int *ix, const int *const end, int *const _s)
{
    unsigned int* s = (unsigned int*)_s;
    unsigned int  max;
    int     choice, choice2;
    max = ix_max(ix, end);

    if (max <= 15) {
      return count_fncs[max](ix, end, max, s);
    }
    /* try tables with linbits */
    if (max > IXMAX_VAL) {
        *s = LARGE_BITS;
        return -1;
    }
    max -= 15u;
    for (choice2 = 24; choice2 < 32; choice2++) {
        if (ht[choice2].linmax >= max) {
            break;
        }
    }

    for (choice = choice2 - 8; choice < 24; choice++) {
        if (ht[choice].linmax >= max) {
            break;
        }
    }
    return count_bit_ESC(ix, end, choice, choice2, s);
}



/*************************************************************************/
/*	      count_bit							 */
/*************************************************************************/
int
noquant_count_bits(lame_internal_flags const *const gfc,
                   gr_info * const gi, calc_noise_data * prev_noise)
{
    SessionConfig_t const *const cfg = &gfc->cfg;
    int     bits = 0;
    int     i, a1, a2;
    int const *const ix = gi->l3_enc;

    i = Min(576, ((gi->max_nonzero_coeff + 2) >> 1) << 1);

    if (prev_noise)
        prev_noise->sfb_count1 = 0;

    /* Determine count1 region */
    for (; i > 1; i -= 2)
        if (ix[i - 1] | ix[i - 2])
            break;
    gi->count1 = i;

    /* Determines the number of bits to encode the quadruples. */
    a1 = a2 = 0;
    for (; i > 3; i -= 4) {
        int x4 = ix[i-4];
        int x3 = ix[i-3];
        int x2 = ix[i-2];
        int x1 = ix[i-1];
        int     p;
        /* hack to check if all values <= 1 */
        if ((unsigned int) (x4 | x3 | x2 | x1) > 1)
            break;

        p = ((x4 * 2 + x3) * 2 + x2) * 2 + x1;
        a1 += t32l[p];
        a2 += t33l[p];
    }

    bits = a1;
    gi->count1table_select = 0;
    if (a1 > a2) {
        bits = a2;
        gi->count1table_select = 1;
    }

    gi->count1bits = bits;
    gi->big_values = i;
    if (i == 0)
        return bits;

    if (gi->block_type == SHORT_TYPE) {
        a1 = 3 * gfc->scalefac_band.s[3];
        if (a1 > gi->big_values)
            a1 = gi->big_values;
        a2 = gi->big_values;

    }
    else if (gi->block_type == NORM_TYPE) {
        assert(i <= 576); /* bv_scf has 576 entries (0..575) */
        a1 = gi->region0_count = gfc->sv_qnt.bv_scf[i - 2];
        a2 = gi->region1_count = gfc->sv_qnt.bv_scf[i - 1];

        assert(a1 + a2 + 2 < SBPSY_l);
        a2 = gfc->scalefac_band.l[a1 + a2 + 2];
        a1 = gfc->scalefac_band.l[a1 + 1];
        if (a2 < i)
            gi->table_select[2] = choose_table(ix + a2, ix + i, &bits);

    }
    else {
        gi->region0_count = 7;
        /*gi->region1_count = SBPSY_l - 7 - 1; */
        gi->region1_count = SBMAX_l - 1 - 7 - 1;
        a1 = gfc->scalefac_band.l[7 + 1];
        a2 = i;
        if (a1 > a2) {
            a1 = a2;
        }
    }


    /* have to allow for the case when bigvalues < region0 < region1 */
    /* (and region0, region1 are ignored) */
    a1 = Min(a1, i);
    a2 = Min(a2, i);

    assert(a1 >= 0);
    assert(a2 >= 0);

    /* Count the number of bits necessary to code the bigvalues region. */
    if (0 < a1)
        gi->table_select[0] = choose_table(ix, ix + a1, &bits);
    if (a1 < a2)
        gi->table_select[1] = choose_table(ix + a1, ix + a2, &bits);
    if (cfg->use_best_huffman == 2) {
        gi->part2_3_length = bits;
        best_huffman_divide(gfc, gi);
        bits = gi->part2_3_length;
    }


    if (prev_noise) {
        if (gi->block_type == NORM_TYPE) {
            int     sfb = 0;
            while (gfc->scalefac_band.l[sfb] < gi->big_values) {
                sfb++;
            }
            prev_noise->sfb_count1 = sfb;
        }
    }

    return bits;
}

/***********************************************************************
  re-calculate the best scalefac_compress using scfsi
  the saved bits are kept in the bit reservoir.
 **********************************************************************/


inline static void
recalc_divide_init(const lame_internal_flags * const gfc,
                   gr_info const *cod_info,
                   int const *const ix, int r01_bits[], int r01_div[], int r0_tbl[], int r1_tbl[])
{
    int     r0, r1, bigv, r0t, r1t, bits;

    bigv = cod_info->big_values;

    for (r0 = 0; r0 <= 7 + 15; r0++) {
        r01_bits[r0] = LARGE_BITS;
    }

    for (r0 = 0; r0 < 16; r0++) {
        int const a1 = gfc->scalefac_band.l[r0 + 1];
        int     r0bits;
        if (a1 >= bigv)
            break;
        r0bits = 0;
        r0t = choose_table(ix, ix + a1, &r0bits);

        for (r1 = 0; r1 < 8; r1++) {
            int const a2 = gfc->scalefac_band.l[r0 + r1 + 2];
            if (a2 >= bigv)
                break;

            bits = r0bits;
            r1t = choose_table(ix + a1, ix + a2, &bits);
            if (r01_bits[r0 + r1] > bits) {
                r01_bits[r0 + r1] = bits;
                r01_div[r0 + r1] = r0;
                r0_tbl[r0 + r1] = r0t;
                r1_tbl[r0 + r1] = r1t;
            }
        }
    }
}

inline static void
recalc_divide_sub(const lame_internal_flags * const gfc,
                  const gr_info * cod_info2,
                  gr_info * const gi,
                  const int *const ix,
                  const int r01_bits[], const int r01_div[], const int r0_tbl[], const int r1_tbl[])
{
    int     bits, r2, a2, bigv, r2t;

    bigv = cod_info2->big_values;

    for (r2 = 2; r2 < SBMAX_l + 1; r2++) {
        a2 = gfc->scalefac_band.l[r2];
        if (a2 >= bigv)
            break;

        bits = r01_bits[r2 - 2] + cod_info2->count1bits;
        if (gi->part2_3_length <= bits)
            break;

        r2t = choose_table(ix + a2, ix + bigv, &bits);
        if (gi->part2_3_length <= bits)
            continue;

        memcpy(gi, cod_info2, sizeof(gr_info));
        gi->part2_3_length = bits;
        gi->region0_count = r01_div[r2 - 2];
        gi->region1_count = r2 - 2 - r01_div[r2 - 2];
        gi->table_select[0] = r0_tbl[r2 - 2];
        gi->table_select[1] = r1_tbl[r2 - 2];
        gi->table_select[2] = r2t;
    }
}




void
best_huffman_divide(const lame_internal_flags * const gfc, gr_info * const gi)
{
    int     i, a1, a2;
    gr_info cod_info2;
    int const *const ix = gi->l3_enc;

    int     r01_bits[7 + 15 + 1];
    int     r01_div[7 + 15 + 1];
    int     r0_tbl[7 + 15 + 1];
    int     r1_tbl[7 + 15 + 1];

    memcpy(&cod_info2, gi, sizeof(gr_info));
    if (gi->block_type == NORM_TYPE) {
        recalc_divide_init(gfc, gi, ix, r01_bits, r01_div, r0_tbl, r1_tbl);
        recalc_divide_sub(gfc, &cod_info2, gi, ix, r01_bits, r01_div, r0_tbl, r1_tbl);
    }

    i = cod_info2.big_values;
    if (i == 0 || (unsigned int) (ix[i - 2] | ix[i - 1]) > 1)
        return;

    i = gi->count1 + 2;
    if (i > 576)
        return;

    /* Determines the number of bits to encode the quadruples. */
    memcpy(&cod_info2, gi, sizeof(gr_info));
    cod_info2.count1 = i;
    a1 = a2 = 0;

    assert(i <= 576);

    for (; i > cod_info2.big_values; i -= 4) {
        int const p = ((ix[i - 4] * 2 + ix[i - 3]) * 2 + ix[i - 2]) * 2 + ix[i - 1];
        a1 += t32l[p];
        a2 += t33l[p];
    }
    cod_info2.big_values = i;

    cod_info2.count1table_select = 0;
    if (a1 > a2) {
        a1 = a2;
        cod_info2.count1table_select = 1;
    }

    cod_info2.count1bits = a1;

    if (cod_info2.block_type == NORM_TYPE)
        recalc_divide_sub(gfc, &cod_info2, gi, ix, r01_bits, r01_div, r0_tbl, r1_tbl);
    else {
        /* Count the number of bits necessary to code the bigvalues region. */
        cod_info2.part2_3_length = a1;
        a1 = gfc->scalefac_band.l[7 + 1];
        if (a1 > i) {
            a1 = i;
        }
        if (a1 > 0)
            cod_info2.table_select[0] =
                choose_table(ix, ix + a1, (int *) &cod_info2.part2_3_length);
        if (i > a1)
            cod_info2.table_select[1] =
                choose_table(ix + a1, ix + i, (int *) &cod_info2.part2_3_length);
        if (gi->part2_3_length > cod_info2.part2_3_length)
            memcpy(gi, &cod_info2, sizeof(gr_info));
    }
}

static const int slen1_n[16] = { 1, 1, 1, 1, 8, 2, 2, 2, 4, 4, 4, 8, 8, 8, 16, 16 };
static const int slen2_n[16] = { 1, 2, 4, 8, 1, 2, 4, 8, 2, 4, 8, 2, 4, 8, 4, 8 };
const int slen1_tab[16] = { 0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4 };
const int slen2_tab[16] = { 0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3 };

static void
scfsi_calc(int ch, III_side_info_t * l3_side)
{
    unsigned int i;
    int     s1, s2, c1, c2;
    int     sfb;
    gr_info *const gi = &l3_side->tt[1][ch];
    gr_info const *const g0 = &l3_side->tt[0][ch];

    for (i = 0; i < (sizeof(scfsi_band) / sizeof(int)) - 1; i++) {
        for (sfb = scfsi_band[i]; sfb < scfsi_band[i + 1]; sfb++) {
            if (g0->scalefac[sfb] != gi->scalefac[sfb]
                && gi->scalefac[sfb] >= 0)
                break;
        }
        if (sfb == scfsi_band[i + 1]) {
            for (sfb = scfsi_band[i]; sfb < scfsi_band[i + 1]; sfb++) {
                gi->scalefac[sfb] = -1;
            }
            l3_side->scfsi[ch][i] = 1;
        }
    }

    s1 = c1 = 0;
    for (sfb = 0; sfb < 11; sfb++) {
        if (gi->scalefac[sfb] == -1)
            continue;
        c1++;
        if (s1 < gi->scalefac[sfb])
            s1 = gi->scalefac[sfb];
    }

    s2 = c2 = 0;
    for (; sfb < SBPSY_l; sfb++) {
        if (gi->scalefac[sfb] == -1)
            continue;
        c2++;
        if (s2 < gi->scalefac[sfb])
            s2 = gi->scalefac[sfb];
    }

    for (i = 0; i < 16; i++) {
        if (s1 < slen1_n[i] && s2 < slen2_n[i]) {
            int const c = slen1_tab[i] * c1 + slen2_tab[i] * c2;
            if (gi->part2_length > c) {
                gi->part2_length = c;
                gi->scalefac_compress = (int)i;
            }
        }
    }
}

/*
Find the optimal way to store the scalefactors.
Only call this routine after final scalefactors have been
chosen and the channel/granule will not be re-encoded.
 */
void
best_scalefac_store(const lame_internal_flags * gfc,
                    const int gr, const int ch, III_side_info_t * const l3_side)
{
    /* use scalefac_scale if we can */
    gr_info *const gi = &l3_side->tt[gr][ch];
    int     sfb, i, j, l;
    int     recalc = 0;

    /* remove scalefacs from bands with ix=0.  This idea comes
     * from the AAC ISO docs.  added mt 3/00 */
    /* check if l3_enc=0 */
    j = 0;
    for (sfb = 0; sfb < gi->sfbmax; sfb++) {
        int const width = gi->width[sfb];
        assert(width >= 0);
        for (l = j, j += width; l < j; ++l) {
            if (gi->l3_enc[l] != 0)
                break;
        }
        if (l == j)
            gi->scalefac[sfb] = recalc = -2; /* anything goes. */
        /*  only best_scalefac_store and calc_scfsi 
         *  know--and only they should know--about the magic number -2. 
         */
    }

    if (!gi->scalefac_scale && !gi->preflag) {
        int     s = 0;
        for (sfb = 0; sfb < gi->sfbmax; sfb++)
            if (gi->scalefac[sfb] > 0)
                s |= gi->scalefac[sfb];

        if (!(s & 1) && s != 0) {
            for (sfb = 0; sfb < gi->sfbmax; sfb++)
                if (gi->scalefac[sfb] > 0)
                    gi->scalefac[sfb] >>= 1;

            gi->scalefac_scale = recalc = 1;
        }
    }

    if (!gi->preflag && gi->block_type != SHORT_TYPE) {
        for (sfb = 11; sfb < SBPSY_l; sfb++)
            if (gi->scalefac[sfb] < pretab[sfb] && gi->scalefac[sfb] != -2)
                break;
        if (sfb == SBPSY_l) {
            for (sfb = 11; sfb < SBPSY_l; sfb++)
                if (gi->scalefac[sfb] > 0)
                    gi->scalefac[sfb] -= pretab[sfb];

            gi->preflag = recalc = 1;
        }
    }

    for (i = 0; i < 4; i++)
        l3_side->scfsi[ch][i] = 0;

    if (gr == 1
        && l3_side->tt[0][ch].block_type != SHORT_TYPE
        && l3_side->tt[1][ch].block_type != SHORT_TYPE) {
        scfsi_calc(ch, l3_side);
        recalc = 0;
    }
    for (sfb = 0; sfb < gi->sfbmax; sfb++) {
        if (gi->scalefac[sfb] == -2) {
            gi->scalefac[sfb] = 0; /* if anything goes, then 0 is a good choice */
        }
    }
    if (recalc) {
        (void) scale_bitcount(gfc, gi);
    }
}


#ifndef NDEBUG
static int
all_scalefactors_not_negative(int const *scalefac, int n)
{
    int     i;
    for (i = 0; i < n; ++i) {
        if (scalefac[i] < 0)
            return 0;
    }
    return 1;
}
#endif


/* number of bits used to encode scalefacs */

/* 18*slen1_tab[i] + 18*slen2_tab[i] */
static const int scale_short[16] = {
    0, 18, 36, 54, 54, 36, 54, 72, 54, 72, 90, 72, 90, 108, 108, 126
};

/* 17*slen1_tab[i] + 18*slen2_tab[i] */
static const int scale_mixed[16] = {
    0, 18, 36, 54, 51, 35, 53, 71, 52, 70, 88, 69, 87, 105, 104, 122
};

/* 11*slen1_tab[i] + 10*slen2_tab[i] */
static const int scale_long[16] = {
    0, 10, 20, 30, 33, 21, 31, 41, 32, 42, 52, 43, 53, 63, 64, 74
};


/*************************************************************************/
/*            scale_bitcount                                             */
/*************************************************************************/

/* Also calculates the number of bits necessary to code the scalefactors. */

static int
mpeg1_scale_bitcount(const lame_internal_flags * gfc, gr_info * const cod_info)
{
    int     k, sfb, max_slen1 = 0, max_slen2 = 0;

    /* maximum values */
    const int *tab;
    int    *const scalefac = cod_info->scalefac;

    (void) gfc;
    assert(all_scalefactors_not_negative(scalefac, cod_info->sfbmax));

    if (cod_info->block_type == SHORT_TYPE) {
        tab = scale_short;
        if (cod_info->mixed_block_flag)
            tab = scale_mixed;
    }
    else {              /* block_type == 1,2,or 3 */
        tab = scale_long;
        if (!cod_info->preflag) {
            for (sfb = 11; sfb < SBPSY_l; sfb++)
                if (scalefac[sfb] < pretab[sfb])
                    break;

            if (sfb == SBPSY_l) {
                cod_info->preflag = 1;
                for (sfb = 11; sfb < SBPSY_l; sfb++)
                    scalefac[sfb] -= pretab[sfb];
            }
        }
    }

    for (sfb = 0; sfb < cod_info->sfbdivide; sfb++)
        if (max_slen1 < scalefac[sfb])
            max_slen1 = scalefac[sfb];

    for (; sfb < cod_info->sfbmax; sfb++)
        if (max_slen2 < scalefac[sfb])
            max_slen2 = scalefac[sfb];

    /* from Takehiro TOMINAGA <tominaga@isoternet.org> 10/99
     * loop over *all* posible values of scalefac_compress to find the
     * one which uses the smallest number of bits.  ISO would stop
     * at first valid index */
    cod_info->part2_length = LARGE_BITS;
    for (k = 0; k < 16; k++) {
        if (max_slen1 < slen1_n[k] && max_slen2 < slen2_n[k]
            && cod_info->part2_length > tab[k]) {
            cod_info->part2_length = tab[k];
            cod_info->scalefac_compress = k;
        }
    }
    return cod_info->part2_length == LARGE_BITS;
}



int
scale_bitcount(const lame_internal_flags * gfc, gr_info * cod_info)
{
    return mpeg1_scale_bitcount(gfc, cod_info);
}


void
huffman_init(lame_internal_flags * const gfc)
{
    int     i;

    for (i = 2; i <= 576; i += 2) {
        int     scfb_anz = 0, bv_index;
        while (gfc->scalefac_band.l[++scfb_anz] < i);

        bv_index = subdv_table[scfb_anz].region0_count;
        while (gfc->scalefac_band.l[bv_index + 1] > i)
            bv_index--;

        if (bv_index < 0) {
            /* this is an indication that everything is going to
               be encoded as region0:  bigvalues < region0 < region1
               so lets set region0, region1 to some value larger
               than bigvalues */
            bv_index = subdv_table[scfb_anz].region0_count;
        }

        gfc->sv_qnt.bv_scf[i - 2] = bv_index;

        bv_index = subdv_table[scfb_anz].region1_count;
        while (gfc->scalefac_band.l[bv_index + gfc->sv_qnt.bv_scf[i - 2] + 2] > i)
            bv_index--;

        if (bv_index < 0) {
            bv_index = subdv_table[scfb_anz].region1_count;
        }

        gfc->sv_qnt.bv_scf[i - 1] = bv_index;
    }
}
