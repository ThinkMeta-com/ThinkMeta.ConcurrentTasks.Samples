/*
 *	lame utility library source file
 *
 *	Copyright (c) 1999 Albert L Faber
 *	Copyright (c) 2000-2005 Alexander Leidinger
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

#include <float.h>
#include "lame.h"
#include "machine.h"
#include "encoder.h"
#include "util.h"
#include "tables.h"

#define PRECOMPUTE
#if defined(__FreeBSD__) && !defined(__alpha__)
# include <machine/floatingpoint.h>
#endif


/***********************************************************************
*
*  Global Function Definitions
*
***********************************************************************/
/*empty and close mallocs in gfc */



static void
free_global_data(lame_internal_flags * gfc)
{
    if (gfc && gfc->cd_psy) {
        if (gfc->cd_psy->l.s3) {
            /* XXX allocated in psymodel_init() */
            free(gfc->cd_psy->l.s3);
        }
        if (gfc->cd_psy->s.s3) {
            /* XXX allocated in psymodel_init() */
            free(gfc->cd_psy->s.s3);
        }
        free(gfc->cd_psy);
        gfc->cd_psy = 0;
    }
}


void
freegfc(lame_internal_flags * const gfc)
{                       /* bit stream structure */
    if (gfc == 0) return;

    if (gfc->bs.buf != NULL) {
        free(gfc->bs.buf);
        gfc->bs.buf = NULL;
    }

    if (gfc->VBR_seek_table.bag) {
        free(gfc->VBR_seek_table.bag);
        gfc->VBR_seek_table.bag = NULL;
        gfc->VBR_seek_table.size = 0;
    }
    if (gfc->ATH) {
        free(gfc->ATH);
    }
    if (gfc->sv_enc.in_buffer_0) {
        free(gfc->sv_enc.in_buffer_0);
    }
    if (gfc->sv_enc.in_buffer_1) {
        free(gfc->sv_enc.in_buffer_1);
    }

    free_global_data(gfc);

    free(gfc);
}

/*those ATH formulas are returning
their minimum value for input = -1*/

static  FLOAT
ATHformula_GB(FLOAT f, FLOAT value, FLOAT f_min, FLOAT f_max)
{
    /* from Painter & Spanias
       modified by Gabriel Bouvigne to better fit the reality
       ath =    3.640 * pow(f,-0.8)
       - 6.800 * exp(-0.6*pow(f-3.4,2.0))
       + 6.000 * exp(-0.15*pow(f-8.7,2.0))
       + 0.6* 0.001 * pow(f,4.0);


       In the past LAME was using the Painter &Spanias formula.
       But we had some recurrent problems with HF content.
       We measured real ATH values, and found the older formula
       to be inacurate in the higher part. So we made this new
       formula and this solved most of HF problematic testcases.
       The tradeoff is that in VBR mode it increases a lot the
       bitrate. */


/*this curve can be udjusted according to the VBR scale:
it adjusts from something close to Painter & Spanias
on V9 up to Bouvigne's formula for V0. This way the VBR
bitrate is more balanced according to the -V value.*/

    FLOAT   ath;

    /* the following Hack allows to ask for the lowest value */
    if (f < -.3)
        f = 3410;

    f /= 1000;          /* convert to khz */
    f = Max(f_min, f);
    f = Min(f_max, f);

    ath = 3.640 * pow(f, -0.8)
        - 6.800 * exp(-0.6 * pow(f - 3.4, 2.0))
        + 6.000 * exp(-0.15 * pow(f - 8.7, 2.0))
        + (0.6 + 0.04 * value) * 0.001 * pow(f, 4.0);
    return ath;
}



FLOAT
ATHformula(SessionConfig_t const *cfg, FLOAT freq)
{
    FLOAT   ath;
    switch (cfg->ATHtype) {
    case 0:
        ath = ATHformula_GB(freq, 9, 0.1f, 24.0f);
        break;
    case 1:
        ath = ATHformula_GB(freq, -1, 0.1f, 24.0f); /*over sensitive, should probably be removed */
        break;
    case 2:
        ath = ATHformula_GB(freq, 0, 0.1f, 24.0f);
        break;
    case 3:
        ath = ATHformula_GB(freq, 1, 0.1f, 24.0f) + 6; /*modification of GB formula by Roel */
        break;
    case 4:
        ath = ATHformula_GB(freq, cfg->ATHcurve, 0.1f, 24.0f);
        break;
    case 5:
        ath = ATHformula_GB(freq, cfg->ATHcurve, 3.41f, 16.1f);
        break;
    default:
        ath = ATHformula_GB(freq, 0, 0.1f, 24.0f);
        break;
    }
    return ath;
}

/* see for example "Zwicker: Psychoakustik, 1982; ISBN 3-540-11401-7 */
FLOAT
freq2bark(FLOAT freq)
{
    /* input: freq in hz  output: barks */
    if (freq < 0)
        freq = 0;
    freq = freq * 0.001;
    return 13.0 * atan(.76 * freq) + 3.5 * atan(freq * freq / (7.5 * 7.5));
}

#if 0
extern FLOAT freq2cbw(FLOAT freq);

/* see for example "Zwicker: Psychoakustik, 1982; ISBN 3-540-11401-7 */
FLOAT
freq2cbw(FLOAT freq)
{
    /* input: freq in hz  output: critical band width */
    freq = freq * 0.001;
    return 25 + 75 * pow(1 + 1.4 * (freq * freq), 0.69);
}

#endif




#define ABS(A) (((A)>0) ? (A) : -(A))

int
FindNearestBitrate(int bRate) /* legal rates from 32 to 320, MPEG-1 only */
{
    int     bitrate;
    int     i;

    bitrate = bitrate_table[1];

    for (i = 2; i <= 14; i++) {
        if (bitrate_table[i] > 0) {
            if (ABS(bitrate_table[i] - bRate) < ABS(bitrate - bRate))
                bitrate = bitrate_table[i];
        }
    }
    return bitrate;
}





#ifndef Min
#define         Min(A, B)       ((A) < (B) ? (A) : (B))
#endif
#ifndef Max
#define         Max(A, B)       ((A) > (B) ? (A) : (B))
#endif






int
BitrateIndex(int bRate) /* convert bitrate in kbps to index, MPEG-1 only */
{
    int     i;
    for (i = 0; i <= 14; i++) {
        if (bitrate_table[i] > 0) {
            if (bitrate_table[i] == bRate) {
                return i;
            }
        }
    }
    return -1;
}

/* convert samp freq in Hz to index. MPEG-1 only - returns -1 for anything
 * outside 32/44.1/48 kHz, this build does not support MPEG-2/2.5 LSF. */

int
SmpFrqIndex(int sample_freq)
{
    switch (sample_freq) {
    case 44100:
        return 0;
    case 48000:
        return 1;
    case 32000:
        return 2;
    default:
        return -1;
    }
}


/*****************************************************************************
*
*  End of bit_stream.c package
*
*****************************************************************************/










/* copy in new samples from in_buffer into mfbuf (the input is never resampled).
   n_in = number of samples from the input buffer that were used.
   n_out = number of samples copied into mfbuf, always n_in  */

void
fill_buffer(lame_internal_flags * gfc,
            sample_t * const mfbuf[2], sample_t const * const in_buffer[2], int nsamples, int *n_in, int *n_out)
{
    SessionConfig_t const *const cfg = &gfc->cfg;
    int     mf_size = gfc->sv_enc.mf_size;
    int     framesize = 576 * 2;
    int     nout, ch = 0;
    int     nch = cfg->channels_out;

    nout = Min(framesize, nsamples);
    do {
        memcpy(&mfbuf[ch][mf_size], &in_buffer[ch][0], nout * sizeof(mfbuf[0][0]));
    } while (++ch < nch);
    *n_out = nout;
    *n_in = nout;
}







/***********************************************************************
*
*  Message Output
*
***********************************************************************/

void
lame_report_def(const char *format, va_list args)
{
    (void) vfprintf(stderr, format, args);
    fflush(stderr); /* an debug function should flush immediately */
}

void 
lame_report_fnc(lame_report_function print_f, const char *format, ...)
{
    if (print_f) {
        va_list args;
        va_start(args, format);
        print_f(format, args);
        va_end(args);
    }
}


void
lame_debugf(const lame_internal_flags* gfc, const char *format, ...)
{
    if (gfc && gfc->report_dbg) {
        va_list args;
        va_start(args, format);
        gfc->report_dbg(format, args);
        va_end(args);
    }
}


void
lame_msgf(const lame_internal_flags* gfc, const char *format, ...)
{
    if (gfc && gfc->report_msg) {
        va_list args;
        va_start(args, format);
        gfc->report_msg(format, args);
        va_end(args);
    }
}


void
lame_errorf(const lame_internal_flags* gfc, const char *format, ...)
{
    if (gfc && gfc->report_err) {
        va_list args;
        va_start(args, format);
        gfc->report_err(format, args);
        va_end(args);
    }
}



void
disable_FPE(void)
{
/* extremly system dependent stuff, move to a lib to make the code readable */
/*==========================================================================*/



    /*
     *  Disable floating point exceptions
     */




#if defined(__FreeBSD__) && defined(FP_X_INV) && defined(FP_X_DZ)
    {
        /* seet floating point mask to the Linux default */
        fp_except_t mask;
        mask = fpgetmask();
        /* if bit is set, we get SIGFPE on that error! */
        fpsetmask(mask & ~(FP_X_INV | FP_X_DZ));
        /*  DEBUGF("FreeBSD mask is 0x%x\n",mask); */
    }
#endif

#if defined(__riscos__) && !defined(ABORTFP)
    /* Disable FPE's under RISC OS */
    /* if bit is set, we disable trapping that error! */
    /*   _FPE_IVO : invalid operation */
    /*   _FPE_DVZ : divide by zero */
    /*   _FPE_OFL : overflow */
    /*   _FPE_UFL : underflow */
    /*   _FPE_INX : inexact */
    DisableFPETraps(_FPE_IVO | _FPE_DVZ | _FPE_OFL);
#endif

    /*
     *  Debugging stuff
     *  The default is to ignore FPE's, unless compiled with -DABORTFP
     *  so add code below to ENABLE FPE's.
     */

#if defined(ABORTFP)
#if defined(_MSC_VER)
    {
#if 0
        /* rh 061207
           the following fix seems to be a workaround for a problem in the
           parent process calling LAME. It would be better to fix the broken
           application => code disabled.
         */

        /* set affinity to a single CPU.  Fix for EAC/lame on SMP systems from
           "Todd Richmond" <todd.richmond@openwave.com> */
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        SetProcessAffinityMask(GetCurrentProcess(), si.dwActiveProcessorMask);
#endif
#include <float.h>
        unsigned int mask;
        mask = _controlfp(0, 0);
        mask &= ~(_EM_OVERFLOW | _EM_UNDERFLOW | _EM_ZERODIVIDE | _EM_INVALID);
        mask = _controlfp(mask, _MCW_EM);
    }
#elif defined(__CYGWIN__)
#  define _FPU_GETCW(cw) __asm__ ("fnstcw %0" : "=m" (*&cw))
#  define _FPU_SETCW(cw) __asm__ ("fldcw %0" : : "m" (*&cw))

#  define _EM_INEXACT     0x00000020 /* inexact (precision) */
#  define _EM_UNDERFLOW   0x00000010 /* underflow */
#  define _EM_OVERFLOW    0x00000008 /* overflow */
#  define _EM_ZERODIVIDE  0x00000004 /* zero divide */
#  define _EM_INVALID     0x00000001 /* invalid */
    {
        unsigned int mask;
        _FPU_GETCW(mask);
        /* Set the FPU control word to abort on most FPEs */
        mask &= ~(_EM_OVERFLOW | _EM_ZERODIVIDE | _EM_INVALID);
        _FPU_SETCW(mask);
    }
# elif defined(__linux__)
    {

#  include <fpu_control.h>
#  ifndef _FPU_GETCW
#  define _FPU_GETCW(cw) __asm__ ("fnstcw %0" : "=m" (*&cw))
#  endif
#  ifndef _FPU_SETCW
#  define _FPU_SETCW(cw) __asm__ ("fldcw %0" : : "m" (*&cw))
#  endif

        /* 
         * Set the Linux mask to abort on most FPE's
         * if bit is set, we _mask_ SIGFPE on that error!
         *  mask &= ~( _FPU_MASK_IM | _FPU_MASK_ZM | _FPU_MASK_OM | _FPU_MASK_UM );
         */

        unsigned int mask;
        _FPU_GETCW(mask);
        mask &= ~(_FPU_MASK_IM | _FPU_MASK_ZM | _FPU_MASK_OM);
        _FPU_SETCW(mask);
    }
#endif
#endif /* ABORTFP */
}





#ifdef USE_FAST_LOG
/***********************************************************************
 *
 * Fast Log Approximation for log2, used to approximate every other log
 * (log10 and log)
 * maximum absolute error for log10 is around 10-6
 * maximum *relative* error can be high when x is almost 1 because error/log10(x) tends toward x/e
 *
 * use it if typical RESULT values are > 1e-5 (for example if x>1.00001 or x<0.99999)
 * or if the relative precision in the domain around 1 is not important (result in 1 is exact and 0)
 *
 ***********************************************************************/


#define LOG2_SIZE       (512)
#define LOG2_SIZE_L2    (9)

static ieee754_float32_t log_table[LOG2_SIZE + 1];



void
init_log_table(void)
{
    int     j;
    static int init = 0;

    /* Range for log2(x) over [1,2[ is [0,1[ */
    assert((1 << LOG2_SIZE_L2) == LOG2_SIZE);

    if (!init) {
        for (j = 0; j < LOG2_SIZE + 1; j++)
            log_table[j] = log(1.0f + j / (ieee754_float32_t) LOG2_SIZE) / log(2.0f);
    }
    init = 1;
}



ieee754_float32_t
fast_log2(ieee754_float32_t x)
{
    ieee754_float32_t log2val, partial;
    union {
        ieee754_float32_t f;
        int     i;
    } fi;
    int     mantisse;
    fi.f = x;
    mantisse = fi.i & 0x7fffff;
    log2val = ((fi.i >> 23) & 0xFF) - 0x7f;
    partial = (mantisse & ((1 << (23 - LOG2_SIZE_L2)) - 1));
    partial *= 1.0f / ((1 << (23 - LOG2_SIZE_L2)));


    mantisse >>= (23 - LOG2_SIZE_L2);

    /* log2val += log_table[mantisse];  without interpolation the results are not good */
    log2val += log_table[mantisse] * (1.0f - partial) + log_table[mantisse + 1] * partial;

    return log2val;
}

#else /* Don't use FAST_LOG */


void
init_log_table(void)
{
}


#endif

/* end of util.c */
