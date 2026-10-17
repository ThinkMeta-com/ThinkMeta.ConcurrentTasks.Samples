/*
 *      Command line parsing related functions
 *
 *      Copyright (c) 1999 Mark Taylor
 *                    2000-2017 Robert Hegemann
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/* $Id$ */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <ctype.h>
#include <math.h>
 
#ifdef STDC_HEADERS
# include <stdio.h>
# include <stdlib.h>
# include <string.h>
#else
# ifndef HAVE_STRCHR
#  define strchr index
#  define strrchr rindex
# endif
char   *strchr(), *strrchr();
# ifndef HAVE_MEMCPY
#  define memcpy(d, s, n) bcopy ((s), (d), (n))
#  define memmove(d, s, n) bcopy ((s), (d), (n))
# endif
#endif


#ifdef HAVE_LIMITS_H
# include <limits.h>
#endif

#include "lame.h"

#include "parse.h"
#include "main.h"
#include "get_audio.h"
#include "version.h"
#include "console.h"

#undef dimension_of
#define dimension_of(array) (sizeof(array)/sizeof(array[0]))

#ifdef WITH_DMALLOC
#include <dmalloc.h>
#endif

                 

static int const lame_alpha_version_enabled = LAME_ALPHA_VERSION;

/* GLOBAL VARIABLES.  set by parse_args() */
/* we need to clean this up */

ReaderConfig global_reader = { 0, 0 };
WriterConfig global_writer = { 0 };

UiConfig global_ui_config = {0};



static int evaluateArgument(char const* token, char const* arg, char* _EndPtr)
{
    if (arg != 0 && arg != _EndPtr)
        return 1;
    error_printf("WARNING: argument missing for '%s'\n", token);
    return 0;
}

static int getDoubleValue(char const* token, char const* arg, double* ptr)
{
    char *_EndPtr=0;
    double d = strtod(arg, &_EndPtr);
    if (ptr != 0) {
        *ptr = d;
    }
    return evaluateArgument(token, arg, _EndPtr);
}

static int getIntValue(char const* token, char const* arg, int* ptr)
{
    char *_EndPtr=0;
    long d = strtol(arg, &_EndPtr, 10);
    if (ptr != 0) {
        *ptr = d;
    }
    return evaluateArgument(token, arg, _EndPtr);
}





/************************************************************************
*
* license
*
* PURPOSE:  Writes version and license to the file specified by fp
*
************************************************************************/

static int
lame_version_print(FILE * const fp)
{
    const char *b = get_lame_os_bitness();
    const char *v = get_lame_version();
    const char *u = get_lame_url();
    const size_t lenb = strlen(b);
    const size_t lenv = strlen(v);
    const size_t lenu = strlen(u);
    const size_t lw = 80;       /* line width of terminal in characters */
    const size_t sw = 16;       /* static width of text */

    if (lw >= lenb + lenv + lenu + sw || lw < lenu + 2)
        /* text fits in 80 chars per line, or line even too small for url */
        if (lenb > 0)
            fprintf(fp, "LAME %s version %s (%s)\n\n", b, v, u);
        else
            fprintf(fp, "LAME version %s (%s)\n\n", v, u);
    else {
        int const n_white_spaces = (int)((lenu+2) > lw ? 0 : lw-2-lenu);
        /* text too long, wrap url into next line, right aligned */
        if (lenb > 0)
            fprintf(fp, "LAME %s version %s\n%*s(%s)\n\n", b, v, n_white_spaces, "", u);
        else
            fprintf(fp, "LAME version %s\n%*s(%s)\n\n", v, n_white_spaces, "", u);
    }
    if (lame_alpha_version_enabled)
        fprintf(fp, "warning: alpha versions should be used for testing only\n\n");


    return 0;
}

static int
print_license(FILE * const fp)
{                       /* print version & license */
    lame_version_print(fp);
    fprintf(fp,
            "Copyright (c) 1999-2011 by The LAME Project\n"
            "Copyright (c) 1999,2000,2001 by Mark Taylor\n"
            "Copyright (c) 1998 by Michael Cheng\n"
            "Copyright (c) 1995,1996,1997 by Michael Hipp: mpglib\n" "\n");
    fprintf(fp,
            "This library is free software; you can redistribute it and/or\n"
            "modify it under the terms of the GNU Library General Public\n"
            "License as published by the Free Software Foundation; either\n"
            "version 2 of the License, or (at your option) any later version.\n"
            "\n");
    fprintf(fp,
            "This library is distributed in the hope that it will be useful,\n"
            "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
            "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU\n"
            "Library General Public License for more details.\n"
            "\n");
    fprintf(fp,
            "You should have received a copy of the GNU Library General Public\n"
            "License along with this program. If not, see\n"
            "<http://www.gnu.org/licenses/>.\n");
    return 0;
}


/************************************************************************
*
* usage
*
* PURPOSE:  Writes command line syntax to the file specified by fp
*
************************************************************************/

int
usage(FILE * const fp, const char *ProgramName)
{                       /* print general syntax */
    lame_version_print(fp);
    fprintf(fp,
            "usage: %s [options] <infile> [outfile]\n"
            "\n"
            "    <infile> must be a WAVE file. Reading from stdin and writing to stdout\n"
            "    are not supported.\n"
            "\n"
            "Try:\n"
            "     \"%s --help\"           for general usage information\n"
            " or:\n"
            "     \"%s --preset help\"    for information on suggested predefined settings\n"
            " or:\n"
            "     \"%s --longhelp\"\n"
            "  or \"%s -?\"              for a complete options list\n\n",
            ProgramName, ProgramName, ProgramName, ProgramName, ProgramName);
    return 0;
}


/************************************************************************
*
* usage
*
* PURPOSE:  Writes command line syntax to the file specified by fp
*           but only the most important ones, to fit on a vt100 terminal
*
************************************************************************/

int
short_help(const lame_global_flags * gfp, FILE * const fp, const char *ProgramName)
{                       /* print short syntax help */
    lame_version_print(fp);
    fprintf(fp,
            "usage: %s [options] <infile> [outfile]\n"
            "\n"
            "    <infile> must be a WAVE file with 32, 44.1 or 48 kHz. Reading from\n"
            "    stdin and writing to stdout are not supported. LAME encodes MPEG-1\n"
            "    Layer III VBR (the \"new\" VBR routine) only.\n"
            "\n" "RECOMMENDED:\n" "    lame -V2 input.wav output.mp3\n" "\n", ProgramName);
    fprintf(fp,
            "OPTIONS:\n"
            "    -V n            quality setting for VBR.  default n=%i\n"
            "                    0=high quality,bigger files. 9.999=smaller files\n"
            "    -b bitrate      minimum allowed bitrate, default 32 kbps\n"
            "    -B bitrate      maximum allowed bitrate, default 320 kbps\n"
            "    -h              higher quality, but a little slower.\n"
            "    -f              fast mode (lower quality)\n",
            lame_get_VBR_q(gfp));
    fprintf(fp,
            "    --preset type   type must be \"medium\", \"standard\" or \"extreme\"\n"
            "                    \"--preset help\" gives more info on these\n" "\n");
    fprintf(fp,
#if defined(WIN32)
            "    --priority type  sets the process priority\n"
            "                     0,1 = Low priority\n"
            "                     2   = normal priority\n"
            "                     3,4 = High priority\n" "\n"
#endif
#if defined(__OS2__)
            "    --priority type  sets the process priority\n"
            "                     0 = Low priority\n"
            "                     1 = Medium priority\n"
            "                     2 = Regular priority\n"
            "                     3 = High priority\n"
            "                     4 = Maximum priority\n" "\n"
#endif
            "    --longhelp      full list of options\n" "\n"
            "    --license       print License information\n\n"
            );

    return 0;
}

/************************************************************************
*
* usage
*
* PURPOSE:  Writes command line syntax to the file specified by fp
*
************************************************************************/

static void
wait_for(FILE * const fp, int lessmode)
{
    if (lessmode) {
        fflush(fp);
        getchar();
    }
    else {
        fprintf(fp, "\n");
    }
    fprintf(fp, "\n");
}

int
long_help(const lame_global_flags * gfp, FILE * const fp, const char *ProgramName, int lessmode)
{                       /* print long syntax help */
    lame_version_print(fp);
    fprintf(fp,
            "usage: %s [options] <infile> [outfile]\n"
            "\n"
            "    <infile> must be a WAVE file with 32, 44.1 or 48 kHz. Reading from\n"
            "    stdin and writing to stdout are not supported. LAME encodes MPEG-1\n"
            "    Layer III VBR (the \"new\" VBR routine) only.\n"
            "\n" "RECOMMENDED:\n" "    lame -V2 input.wav output.mp3\n" "\n", ProgramName);
    fprintf(fp,
            "OPTIONS:\n"
            "  Input options:\n"
            "    --scale <arg>   scale input (multiply PCM data) by <arg>\n"
            "    --scale-l <arg> scale channel 0 (left) input (multiply PCM data) by <arg>\n"
            "    --scale-r <arg> scale channel 1 (right) input (multiply PCM data) by <arg>\n"
            "    --swap-channel  swap L/R channels\n"
            "    --ignorelength  ignore file length in WAV header\n"
            "    --gain <arg>    apply Gain adjustment in decibels, range -20.0 to +12.0\n"
            "    -a              downmix from stereo to mono file for mono encoding\n"
            "\n"
           );

    wait_for(fp, lessmode);
    fprintf(fp,
            "  Operational options:\n"
            "    -m <mode>       (j)oint, (s)imple, (f)orce, (d)ual-mono, (m)ono (l)eft (r)ight\n"
            "                    default is (j)\n"
            "                    joint  = Uses the best possible of MS and LR stereo\n"
            "                    simple = force LR stereo on all frames\n"
            "                    force  = force MS stereo on all frames.\n"
	   );
    fprintf(fp,
            "    --preset type   type must be \"medium\", \"standard\" or \"extreme\"\n"
            "                    \"--preset help\" gives more info on these\n"
            "    --r3mix         the tuning of the former r3mix preset, like -V 3\n"
            "    --flush         flush output stream as soon as possible\n");

    wait_for(fp, lessmode);
    fprintf(fp,
            "  Verbosity:\n"
            "    -S              don't print the LAME tag message\n"
            "    --quiet         don't print anything on screen\n"
            "    --silent        don't print anything on screen, but fatal errors\n"
            "    --brief         print more useful information\n"
            "    --verbose       print a lot of useful information\n" "\n");
    fprintf(fp,
            "  Noise shaping & psycho acoustic algorithms:\n"
            "    -q <arg>        <arg> = 0...9.  Default  -q 3 \n"
            "                    -q 0:  Highest quality, very slow \n"
            "                    -q 9:  Poor quality, but fast \n"
            "    -h              Same as -q 2.   \n"
            "    -f              Same as -q 7.   Fast, ok quality\n"
            "    --athaa-sensitivity x  activation offset in -/+ dB for ATH auto-adjustment\n"
            "    -Z [n]          always do calculate short block maskings\n");

    wait_for(fp, lessmode);
    fprintf(fp,
            "  VBR options:\n"
            "    -V n            quality setting for VBR.  default n=%i\n"
            "                    0=high quality,bigger files. 9.999=smaller files\n"
            "    -Y              lets LAME ignore noise in sfb21\n"
            "                    (Default for V3 to V9.999)\n"
            ,
            lame_get_VBR_q(gfp));
    fprintf(fp,
            "    -b <bitrate>    specify minimum allowed bitrate, default  32 kbps\n"
            "    -B <bitrate>    specify maximum allowed bitrate, default 320 kbps\n"
            "    -F              strictly enforce the -b option, for use with players that\n"
            "                    do not support low bitrate mp3\n"
            "    -t              disable writing LAME Tag\n"
            "    -T              enable and force writing LAME Tag\n");

    wait_for(fp, lessmode);
    fprintf(fp,
            "  MP3 header/stream options:\n"
            "    -e <emp>        de-emphasis n/5/c  (obsolete)\n"
            "    -c              mark as copyright\n"
            "    -o              mark as non-original\n"
            "    -p              error protection.  adds 16 bit checksum to every frame\n"
            "                    (the checksum is computed correctly)\n"
            "    --nores         disable the bit reservoir\n"
            "    --strictly-enforce-ISO   comply as much as possible to ISO MPEG spec\n");
    fprintf(fp,
            "    --buffer-constraint <constraint> available values for constraint:\n"
            "                                     default, strict, maximum\n"
            "\n"
            );
    fprintf(fp,
            "  Filter options:\n"
            "  --lowpass <freq>        frequency(kHz), lowpass filter cutoff above freq\n"
            "  --lowpass-width <freq>  frequency(kHz) - default 15%% of lowpass freq\n"
            "  --highpass <freq>       frequency(kHz), highpass filter cutoff below freq\n"
            "  --highpass-width <freq> frequency(kHz) - default 15%% of highpass freq\n");

    wait_for(fp, lessmode);
    fprintf(fp,
#if defined(WIN32)
            "\n\nMS-Windows-specific options:\n"
            "    --priority <type>  sets the process priority:\n"
            "                         0,1 = Low priority (IDLE_PRIORITY_CLASS)\n"
            "                         2 = normal priority (NORMAL_PRIORITY_CLASS, default)\n"
            "                         3,4 = High priority (HIGH_PRIORITY_CLASS))\n"
            "    Note: Calling '--priority' without a parameter will select priority 0.\n"
#endif
#if defined(__OS2__)
            "\n\nOS/2-specific options:\n"
            "    --priority <type>  sets the process priority:\n"
            "                         0 = Low priority (IDLE, delta = 0)\n"
            "                         1 = Medium priority (IDLE, delta = +31)\n"
            "                         2 = Regular priority (REGULAR, delta = -31)\n"
            "                         3 = High priority (REGULAR, delta = 0)\n"
            "                         4 = Maximum priority (REGULAR, delta = +31)\n"
            "    Note: Calling '--priority' without a parameter will select priority 0.\n"
#endif
            "\nMisc:\n    --license       print License information\n\n"
        );

    display_bitrates(fp);

    return 0;
}

static void
display_bitrate(FILE * const fp)
{
    int     i;

    fprintf(fp,
            "\nMPEG-1   layer III sample frequencies (kHz):  32  48  44.1\n"
            "bitrates (kbps):");
    for (i = 1; i <= 14; i++)
        fprintf(fp, " %2i", lame_get_bitrate(i));
    fprintf(fp, "\n");
}

int
display_bitrates(FILE * const fp)
{
    display_bitrate(fp);
    fprintf(fp, "\n");
    fflush(fp);
    return 0;
}


/*  note: for presets it would be better to externalize them in a file.
    suggestion:  lame --preset <file-name> ...
            or:  lame --preset my-setting  ... and my-setting is defined in lame.ini
 */

/************************************************************************
*
* usage
*
* PURPOSE:  Writes presetting info to #stdout#
*
************************************************************************/


static void
presets_longinfo_dm(FILE * msgfp)
{
    fprintf(msgfp,
            "\n"
            "The --preset switches are aliases over LAME settings.\n"
            "\n" "\n");
    fprintf(msgfp,
            "     --preset medium      (-V 4) This preset should provide near transparency\n"
            "                          to most people on most music.\n"
            "\n"
            "     --preset standard    (-V 2) This preset should generally be transparent\n"
            "                          to most people on most music and is already quite\n"
            "                          high in quality.\n" "\n");
    fprintf(msgfp,
            "     --preset extreme     (-V 0) If you have extremely good hearing and similar\n"
            "                          equipment, this preset will generally provide\n"
            "                          slightly higher quality than the \"standard\" mode.\n" "\n");
    fprintf(msgfp,
            "    For example:\n"
            "\n"
            "    --preset standard <input file> <output file>\n"
            " or --preset extreme <input file> <output file>\n" "\n");
}


static int
presets_set(lame_t gfp, const char *preset_name, const char *ProgramName)
{
    if (strcmp(preset_name, "help") == 0) {
        lame_version_print(stdout);
        presets_longinfo_dm(stdout);
        return -1;
    }

    if (strcmp(preset_name, "medium") == 0) {
        lame_set_VBR_q(gfp, 4);
        return 0;
    }

    if (strcmp(preset_name, "standard") == 0) {
        lame_set_VBR_q(gfp, 2);
        return 0;
    }

    else if (strcmp(preset_name, "extreme") == 0) {
        lame_set_VBR_q(gfp, 0);
        return 0;
    }

    lame_version_print(Console_IO.Error_fp);
    error_printf("Error: You did not enter a valid profile and/or options with --preset\n"
                 "\n"
                 "Available profiles are:\n"
                 "\n"
                 "                 medium\n"
                 "                 standard\n"
                 "                 extreme\n"
                 "\n"
                 "For further information try: \"%s --preset help\"\n", ProgramName);
    return -1;
}


/************************************************************************
*
* parse_args
*
* PURPOSE:  Sets encoding parameters to the specifications of the
* command line.  Default settings are used for parameters
* not specified in the command line.
*
* If the input file is in WAVE or AIFF format, the sampling frequency is read
* from the AIFF header.
*
* The input and output filenames are read into #inpath# and #outpath#.
*
************************************************************************/

/* would use real "strcasecmp" but it isn't portable */
static int
local_strcasecmp(const char *s1, const char *s2)
{
    unsigned char c1;
    unsigned char c2;

    do {
        c1 = (unsigned char) tolower(*s1);
        c2 = (unsigned char) tolower(*s2);
        if (!c1) {
            break;
        }
        ++s1;
        ++s2;
    } while (c1 == c2);
    return c1 - c2;
}

static int
local_strncasecmp(const char *s1, const char *s2, int n)
{
    unsigned char c1 = 0;
    unsigned char c2 = 0;
    int     cnt = 0;

    do {
        if (cnt == n) {
            break;
        }
        c1 = (unsigned char) tolower(*s1);
        c2 = (unsigned char) tolower(*s2);
        if (!c1) {
            break;
        }
        ++s1;
        ++s2;
        ++cnt;
    } while (c1 == c2);
    return c1 - c2;
}



#ifdef _WIN32
#define SLASH '\\'
#define COLON ':'
#elif __OS2__
#define SLASH '\\'
#else
#define SLASH '/'
#endif

static
size_t scanPath(char const* s, char const** a, char const** b)
{
    char const* s1 = s;
    char const* s2 = s;
    if (s != 0) {
        for (; *s; ++s) {
            switch (*s) {
            case SLASH:
#ifdef _WIN32
            case COLON:
#endif
                s2 = s;
                break;
            }
        }
#ifdef _WIN32
        if (*s2 == COLON) {
            ++s2;
        }
#endif
    }
    if (a) {
        *a = s1;
    }
    if (b) {
        *b = s2;
    }
    return s2-s1;
}

static
size_t scanBasename(char const* s, char const** a, char const** b)
{
    char const* s1 = s;
    char const* s2 = s;
    if (s != 0) {
        for (; *s; ++s) {
            switch (*s) {
            case SLASH:
#ifdef _WIN32
            case COLON:
#endif
                s1 = s2 = s;
                break;
            case '.':
                s2 = s;
                break;
            }
        }
        if (s2 == s1) {
            s2 = s;
        }
        if (*s1 == SLASH 
#ifdef _WIN32
          || *s1 == COLON
#endif
           ) {
            ++s1;
        }
    }
    if (a != 0) {
        *a = s1;
    }
    if (b != 0) {
        *b = s2;
    }
    return s2-s1;
}

static 
int isCommonSuffix(char const* s_ext)
{
    char const* suffixes[] = 
    { ".WAV", ".RAW", ".MP1", ".MP2"
    , ".MP3", ".MPG", ".MPA", ".CDA"
    , ".OGG", ".AIF", ".AIFF", ".AU"
    , ".SND", ".FLAC", ".WV", ".OFR"
    , ".TAK", ".MP4", ".M4A", ".PCM"
    , ".W64"
    };
    size_t i;
    for (i = 0; i < dimension_of(suffixes); ++i) {
        if (local_strcasecmp(s_ext, suffixes[i]) == 0) {
            return 1;
        }
    }
    return 0;
}


int generateOutPath(char const* inPath, char const* outDir, char const* s_ext, char* outPath)
{
    size_t const max_path = PATH_MAX;
#if 1
    size_t i = 0;
    int out_dir_used = 0;

    if (outDir != 0 && outDir[0] != 0) {
        out_dir_used = 1;
        while (*outDir) {
            outPath[i++] = *outDir++;
            if (i >= max_path) {
                goto err_generateOutPath;
            }
        }
        if (i > 0 && outPath[i-1] != SLASH) {
            outPath[i++] = SLASH;
            if (i >= max_path) {
                goto err_generateOutPath;
            }
        }
        outPath[i] = 0;
    }
    else {
        char const* pa;
        char const* pb;
        size_t j, n = scanPath(inPath, &pa, &pb);
        if (i+n >= max_path) {
            goto err_generateOutPath;
        }
        for (j = 0; j < n; ++j) {
            outPath[i++] = pa[j];
        }
        if (n > 0) {
            outPath[i++] = SLASH;
            if (i >= max_path) {
                goto err_generateOutPath;
            }
        }
        outPath[i] = 0;
    }
    {
        int replace_suffix = 0;
        char const* na;
        char const* nb;
        size_t j, n = scanBasename(inPath, &na, &nb);
        if (i+n >= max_path) {
            goto err_generateOutPath;
        }
        for (j = 0; j < n; ++j) {
            outPath[i++] = na[j];
        }
        outPath[i] = 0;
        if (isCommonSuffix(nb) == 1) {
            replace_suffix = 1;
            if (out_dir_used == 0) {
                if (local_strcasecmp(nb, s_ext) == 0) {
                    replace_suffix = 0;
                }
            }
        }
        if (replace_suffix == 0) {
            while (*nb) {
                outPath[i++] = *nb++;
                if (i >= max_path) {
                    goto err_generateOutPath;
                }
            }
            outPath[i] = 0;
        }
    }
    if (i+5 >= max_path) {
        goto err_generateOutPath;
    }
    while (*s_ext) {
        outPath[i++] = *s_ext++;
    }
    outPath[i] = 0;
    return 0;
err_generateOutPath:
    error_printf( "error: output file name too long\n" );
    return 1;
#else
    strncpy(outPath, inPath, PATH_MAX + 1 - 4);
    strncat(outPath, s_ext, 4);
    return 0;
#endif
}


/* Ugly, NOT final version */

#define T_IF(str)          if ( 0 == local_strcasecmp (token,str) ) {
#define T_ELIF(str)        } else if ( 0 == local_strcasecmp (token,str) ) {
#define T_ELIF2(str1,str2) } else if ( 0 == local_strcasecmp (token,str1)  ||  0 == local_strcasecmp (token,str2) ) {
#define T_ELSE             } else {
#define T_END              }


static int
parse_args_(lame_global_flags * gfp, int argc, char **argv,
           char *const inPath, char *const outPath)
{
    int     input_file = 0;  /* set to 1 if we parse an input file name  */
    int     i;
    int     autoconvert = 0;
    const char *ProgramName = argv[0];

    inPath[0] = '\0';
    outPath[0] = '\0';
    /* turn on display options. user settings may turn them off below */
    global_ui_config.silent = 0; /* default */

    /* process args */
    for (i = 0; ++i < argc;) {
        char   *token;
        int     argUsed;

        token = argv[i];
        if (*token++ == '-') {
            char   *nextArg = i + 1 < argc ? argv[i + 1] : "";
            argUsed = 0;
            if (!*token) { /* "-" would mean stdin or stdout */
                error_printf("Reading from stdin and writing to stdout are not supported.\n");
                return -1;
            }
            if (*token == '-') { /* GNU style */
                double  double_value = 0;
                int     int_value = 0;
                token++;

                T_IF("r3mix")
                    lame_set_preset(gfp, R3MIX);

                T_ELIF("flush")
                    global_writer.flush_write = 1;

                T_ELIF("nores")
                    lame_set_disable_reservoir(gfp, 1);

                T_ELIF("strictly-enforce-ISO")
                    lame_set_strict_ISO(gfp, MDB_STRICT_ISO);

                T_ELIF("buffer-constraint")
                  argUsed = 1;
                if (strcmp(nextArg, "default") == 0)
                  (void) lame_set_strict_ISO(gfp, MDB_DEFAULT);
                else if (strcmp(nextArg, "strict") == 0)
                  (void) lame_set_strict_ISO(gfp, MDB_STRICT_ISO);
                else if (strcmp(nextArg, "maximum") == 0)
                  (void) lame_set_strict_ISO(gfp, MDB_MAXIMUM);
                else {
                    error_printf("unknown buffer constraint '%s'\n", nextArg);
                    return -1;
                }

                T_ELIF("scale")
                    argUsed = getDoubleValue(token, nextArg, &double_value);
                    if (argUsed)
                        (void) lame_set_scale(gfp, (float) double_value);

                T_ELIF("scale-l")
                    argUsed = getDoubleValue(token, nextArg, &double_value);
                    if (argUsed)
                        (void) lame_set_scale_left(gfp, (float) double_value);

                T_ELIF("scale-r")
                    argUsed = getDoubleValue(token, nextArg, &double_value);
                    if (argUsed)
                        (void) lame_set_scale_right(gfp, (float) double_value);

                T_ELIF("gain")
                    argUsed = getDoubleValue(token, nextArg, &double_value);
                    if (argUsed) {
                        double gain = double_value;
                        gain = gain > -20.f ? gain : -20.f;
                        gain = gain < 12.f ? gain : 12.f;
                        gain = pow(10.f, gain*0.05);
                        (void) lame_set_scale(gfp, (float) gain);
                    }


#if defined(__OS2__) || defined(WIN32)
                T_ELIF("priority")
                    argUsed = getIntValue(token, nextArg, &int_value);
                    if (argUsed)
                        setProcessPriority(int_value);
#endif

                T_ELIF("lowpass")
                    argUsed = getDoubleValue(token, nextArg, &double_value);
                    if (argUsed) {
                        if (double_value < 0) {
                            lame_set_lowpassfreq(gfp, -1);
                        }
                        else {
                            /* useful are 0.001 kHz...50 kHz, 50 Hz...50000 Hz */
                            if (double_value < 0.001 || double_value > 50000.) {
                                error_printf("Must specify lowpass with --lowpass freq, freq >= 0.001 kHz\n");
                                return -1;
                            }
                            lame_set_lowpassfreq(gfp, (int) (double_value * (double_value < 50. ? 1.e3 : 1.e0) + 0.5));
                        }
                    }
                
                T_ELIF("lowpass-width")
                    argUsed = getDoubleValue(token, nextArg, &double_value);
                    if (argUsed) {
                        /* useful are 0.001 kHz...16 kHz, 16 Hz...50000 Hz */
                        if (double_value < 0.001 || double_value > 50000.) {
                            error_printf
                                ("Must specify lowpass width with --lowpass-width freq, freq >= 0.001 kHz\n");
                            return -1;
                        }
                        lame_set_lowpasswidth(gfp, (int) (double_value * (double_value < 16. ? 1.e3 : 1.e0) + 0.5));
                    }

                T_ELIF("highpass")
                    argUsed = getDoubleValue(token, nextArg, &double_value);
                    if (argUsed) {
                        if (double_value < 0.0) {
                            lame_set_highpassfreq(gfp, -1);
                        }
                        else {
                            /* useful are 0.001 kHz...16 kHz, 16 Hz...50000 Hz */
                            if (double_value < 0.001 || double_value > 50000.) {
                                error_printf("Must specify highpass with --highpass freq, freq >= 0.001 kHz\n");
                                return -1;
                            }
                            lame_set_highpassfreq(gfp, (int) (double_value * (double_value < 16. ? 1.e3 : 1.e0) + 0.5));
                        }
                    }
                    
                T_ELIF("highpass-width")
                    argUsed = getDoubleValue(token, nextArg, &double_value);
                    if (argUsed) {
                        /* useful are 0.001 kHz...16 kHz, 16 Hz...50000 Hz */
                        if (double_value < 0.001 || double_value > 50000.) {
                            error_printf
                                ("Must specify highpass width with --highpass-width freq, freq >= 0.001 kHz\n");
                            return -1;
                        }
                        lame_set_highpasswidth(gfp, (int) double_value);
                    }

    
                /* some more GNU-ish options could be added
                 * brief         => few messages on screen (name, status report)
                 * o/output file => specifies output filename
                 * O             => stdout
                 * i/input file  => specifies input filename
                 * I             => stdin
                 */
                T_ELIF("quiet")
                    global_ui_config.silent = 10; /* on a scale from 1 to 10 be very silent */

                T_ELIF("silent")
                    global_ui_config.silent = 9;

                T_ELIF("brief")
                    global_ui_config.silent = -5; /* print few info on screen */

                T_ELIF("verbose")
                    global_ui_config.silent = -10; /* print a lot on screen */
                
                T_ELIF2("version", "license")
                    print_license(stdout);
                return -2;

                T_ELIF2("help", "usage")
                    short_help(gfp, stdout, ProgramName);
                return -2;

                T_ELIF("longhelp")
                    long_help(gfp, stdout, ProgramName, 0 /* lessmode=NO */ );
                return -2;

                T_ELIF("?")
#ifdef __unix__
                    FILE   *fp = popen("less -Mqc", "w");
                    long_help(gfp, fp, ProgramName, 0 /* lessmode=NO */ );
                    pclose(fp);
#else
                    long_help(gfp, stdout, ProgramName, 1 /* lessmode=YES */ );
#endif
                return -2;

                T_ELIF("preset")
                    argUsed = 1;
                    if (presets_set(gfp, nextArg, ProgramName) < 0)
                        return -1;

                T_ELIF("swap-channel")
                    global_reader.swap_channel = 1;

                T_ELIF("ignorelength")
                    global_reader.ignorewavlength = 1;

                T_ELIF ("athaa-sensitivity")
                    argUsed = getDoubleValue(token, nextArg, &double_value);
                    if (argUsed)
                        lame_set_athaa_sensitivity(gfp, (float) double_value);

                T_ELSE {
                    error_printf("%s: unrecognized option --%s\n", ProgramName, token);
                    return -1;
                }
                T_END   i += argUsed;

            }
            else {
                char    c;
                while ((c = *token++) != '\0') {
                    double double_value = 0;
                    int int_value = 0;
                    char const *arg = *token ? token : nextArg;
                    switch (c) {
                    case 'm':
                        argUsed = 1;

                        switch (*arg) {
                        case 's':
                            (void) lame_set_mode(gfp, STEREO);
                            break;
                        case 'd':
                            (void) lame_set_mode(gfp, DUAL_CHANNEL);
                            break;
                        case 'f':
                            lame_set_force_ms(gfp, 1);
                            (void) lame_set_mode(gfp, JOINT_STEREO);
                            break;
                        case 'j':
                            lame_set_force_ms(gfp, 0);
                            (void) lame_set_mode(gfp, JOINT_STEREO);
                            break;
                        case 'm':
                            (void) lame_set_mode(gfp, MONO);
                            break;
                        case 'l':
                            (void) lame_set_mode(gfp, MONO);
                            (void) lame_set_scale_left(gfp, 2);
                            (void) lame_set_scale_right(gfp, 0);
                            break;
                        case 'r':
                            (void) lame_set_mode(gfp, MONO);
                            (void) lame_set_scale_left(gfp, 0);
                            (void) lame_set_scale_right(gfp, 2);
                            break;
                        case 'a': /* same as 'j' ??? */
                            lame_set_force_ms(gfp, 0);
                            (void) lame_set_mode(gfp, JOINT_STEREO);
                            break;
                        default:
                            error_printf("%s: -m mode must be s/d/f/j/m/l/r not %s\n", ProgramName,
                                         arg);
                            return -1;
                        }
                        break;

                    case 'V':
                        argUsed = getDoubleValue("V", arg, &double_value);
                        if (argUsed) {
                            lame_set_VBR_quality(gfp, (float) double_value);
                        }
                        break;

                    case 'q':
                        argUsed = getIntValue("q", arg, &int_value);
                        if (argUsed) 
                            (void) lame_set_quality(gfp, int_value);
                        break;
                    case 'f':
                        (void) lame_set_quality(gfp, 7);
                        break;
                    case 'h':
                        (void) lame_set_quality(gfp, 2);
                        break;

                    case 'b':
                        argUsed = getIntValue("b", arg, &int_value);
                        if (argUsed) {
                            /* the unmodified LAME also set this as CBR bitrate, and its
                               lame_set_brate() turned the bit reservoir off above 320 kbps */
                            if (int_value > 320)
                                lame_set_disable_reservoir(gfp, 1);
                            lame_set_VBR_min_bitrate_kbps(gfp, int_value);
                        }
                        break;
                    case 'B':
                        argUsed = getIntValue("B", arg, &int_value);
                        if (argUsed) {
                            lame_set_VBR_max_bitrate_kbps(gfp, int_value);
                        }
                        break;
                    case 'F':
                        lame_set_VBR_hard_min(gfp, 1);
                        break;
                    case 't': /* dont write VBR tag */
                        (void) lame_set_bWriteVbrTag(gfp, 0);
                        break;
                    case 'T': /* do write VBR tag */
                        (void) lame_set_bWriteVbrTag(gfp, 1);
                        break;
                    case 'p': /* (jo) error_protection: add crc16 information to stream */
                        lame_set_error_protection(gfp, 1);
                        break;
                    case 'a': /* autoconvert input file from stereo to mono - for mono mp3 encoding */
                        autoconvert = 1;
                        (void) lame_set_mode(gfp, MONO);
                        break;
                    case 'S':
                        global_ui_config.silent = 5;
                        break;
                    case 'Y':
                        lame_set_experimentalY(gfp, 1);
                        break;
                    case 'Z':
                        /*  experimental switch -Z:
                         */
                        {
                            int     n = 1;
                            argUsed = sscanf(arg, "%d", &n);
                            {
                                lame_set_experimentalZ(gfp, n);
                            }
                        }
                        break;
                    case 'e':
                        argUsed = 1;

                        switch (*arg) {
                        case 'n':
                            lame_set_emphasis(gfp, 0);
                            break;
                        case '5':
                            lame_set_emphasis(gfp, 1);
                            break;
                        case 'c':
                            lame_set_emphasis(gfp, 3);
                            break;
                        default:
                            error_printf("%s: -e emp must be n/5/c not %s\n", ProgramName, arg);
                            return -1;
                        }
                        break;
                    case 'c':
                        lame_set_copyright(gfp, 1);
                        break;
                    case 'o':
                        lame_set_original(gfp, 0);
                        break;

                    case '?':
                        long_help(gfp, stdout, ProgramName, 0 /* LESSMODE=NO */ );
                        return -1;

                    default:
                        error_printf("%s: unrecognized option -%c\n", ProgramName, c);
                        return -1;
                    }
                    if (argUsed) {
                        if (arg == token)
                            token = ""; /* no more from token */
                        else
                            ++i; /* skip arg we used */
                        arg = "";
                        argUsed = 0;
                    }
                }
            }
        }
        else {
            {
                /* normal options:   inputfile  [outputfile] */
                if (inPath[0] == '\0') {
                    strncpy(inPath, argv[i], PATH_MAX + 1);
                    input_file = 1;
                }
                else {
                    if (outPath[0] == '\0')
                        strncpy(outPath, argv[i], PATH_MAX + 1);
                    else {
                        error_printf("%s: excess arg %s\n", ProgramName, argv[i]);
                        return -1;
                    }
                }
            }
        }
    }                   /* loop over args */

    if (!input_file) {
        usage(Console_IO.Console_fp, ProgramName);
        return -1;
    }

#ifdef WIN32
    dosToLongFileName(inPath);
#endif

    if (outPath[0] == '\0') { /* no explicit output file */
        if (generateOutPath(inPath, "", ".mp3", outPath) != 0) {
            return -1;
        }
    }

    /* default guess for number of channels */
    if (autoconvert)
        (void) lame_set_num_channels(gfp, 2);
    else if (MONO == lame_get_mode(gfp))
        (void) lame_set_num_channels(gfp, 1);
    else
        (void) lame_set_num_channels(gfp, 2);

    return 0;
}

static int
string_to_argv(char* str, char** argv, int N)
{
    int     argc = 0;
    if (str == 0) return argc;
    argv[argc++] = "lhama";
    for (;;) {
        int     quoted = 0;
        while (isspace(*str)) { /* skip blanks */
            ++str;
        }
        if (*str == '\"') { /* is quoted argument ? */
            quoted = 1;
            ++str;
        }
        if (*str == '\0') { /* end of string reached */
            break;
        }
        /* found beginning of some argument */
        if (argc < N) {
            argv[argc++] = str;
        }
        /* look out for end of argument, either end of string, blank or quote */
        for(; *str != '\0'; ++str) {
            if (quoted) {
                if (*str == '\"') { /* end of quotation reached */
                    *str++ = '\0';
                    break;
                }
            }
            else {
                if (isspace(*str)) { /* parameter separator reached */
                    *str++ = '\0';
                    break;
                }
            }
        }
    }
    return argc;
}

static int
merge_argv(int argc, char** argv, int str_argc, char** str_argv, int N)
{
    int     i;
    if (argc > 0) {
        str_argv[0] = argv[0];
        if (str_argc < 1) str_argc = 1;
    }
    for (i = 1; i < argc; ++i) {
        int     j = str_argc + i - 1;
        if (j < N) {
            str_argv[j] = argv[i];
        }
    }
    return argc + str_argc - 1;
}

#ifdef DEBUG
static void
dump_argv(int argc, char** argv)
{
    int     i;
    for (i = 0; i < argc; ++i) {
        printf("%d: \"%s\"\n",i,argv[i]);
    }
}
#endif


int parse_args(lame_t gfp, int argc, char **argv, char *const inPath, char *const outPath)
{
    char   *str_argv[512], *str;
    int     str_argc, ret;
    str = lame_getenv("LAMEOPT");
    str_argc = string_to_argv(str, str_argv, dimension_of(str_argv));
    str_argc = merge_argv(argc, argv, str_argc, str_argv, dimension_of(str_argv));
#ifdef DEBUG
    dump_argv(str_argc, str_argv);
#endif
    ret = parse_args_(gfp, str_argc, str_argv, inPath, outPath);
    free(str);
    return ret;
}

/* end of parse.c */
