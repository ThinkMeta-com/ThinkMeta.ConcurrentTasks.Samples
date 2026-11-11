/*
 *      Command line frontend program
 *
 *      Copyright (c) 1999 Mark Taylor
 *                    2000 Takehiro TOMINAGA
 *                    2010-2017 Robert Hegemann
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
#include <stdio.h>

#ifdef STDC_HEADERS
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

#ifdef HAVE_FCNTL_H
# include <fcntl.h>
#endif

#ifdef __sun__
/* woraround for SunOS 4.x, it has SEEK_* defined here */
#include <unistd.h>
#endif

#if defined(_WIN32)
# include <windows.h>
#endif


/*
 main.c is example code for how to use libmp3lame.a.  To use this library,
 you only need the library and lame.h.  All other .h files are private
 to the library.
*/
#include "lame.h"

#include "console.h"
#include "parse.h"
#include "main.h"
#include "get_audio.h"
#include "timestatus.h"

/* PLL 14/04/2000 */
#if macintosh
#include <console.h>
#endif

#ifdef WITH_DMALLOC
#include <dmalloc.h>
#endif




/************************************************************************
*
* main
*
* PURPOSE:  MPEG-1,2 Layer III encoder with GPSYCHO
* psychoacoustic model.
*
************************************************************************/


static FILE *
init_files(lame_global_flags * gf, char const *inPath, char const *outPath)
{
    FILE   *outf;
    /* Mostly it is not useful to use the same input and output name.
       This test is very easy and buggy and don't recognize different names
       assigning the same file
     */
    if (0 == strcmp(inPath, outPath)) {
        error_printf("Input file and Output file are the same. Abort.\n");
        return NULL;
    }

    /* open the wav input file.  This call will
     * open the file, try to parse the headers and
     * set gf.samplerate, gf.num_channels, gf.num_samples.
     * if you want to do your own file input, skip this call and set
     * samplerate, num_channels and num_samples yourself.
     */
    if (init_infile(gf, inPath) < 0) {
        error_printf("Can't init infile '%s'\n", inPath);
        return NULL;
    }
    if ((outf = init_outfile(outPath)) == NULL) {
        error_printf("Can't init outfile '%s'\n", outPath);
        return NULL;
    }

    return outf;
}


static void
print_trailing_info(lame_global_flags * gf)
{
    if (lame_get_findReplayGain(gf)) {
        int     RadioGain = lame_get_RadioGain(gf);
        console_printf("ReplayGain: %s%.1fdB\n", RadioGain > 0 ? "+" : "",
                       ((float) RadioGain) / 10.0);
        if (RadioGain > 0x1FE || RadioGain < -0x1FE)
            error_printf
                ("WARNING: ReplayGain exceeds the -51dB to +51dB range. Such a result is too\n"
                 "         high to be stored in the header.\n");
    }

}


static int
write_xing_frame(lame_global_flags * gf, FILE * outf, size_t offset)
{
    unsigned char mp3buffer[LAME_MAXMP3BUFFER];
    size_t  imp3, owrite;

    imp3 = lame_get_lametag_frame(gf, mp3buffer, sizeof(mp3buffer));
    if (imp3 == 0) {
        return 0;       /* nothing to do */
    }
    if (global_ui_config.silent <= 0) {
        console_printf("Writing LAME Tag...");
    }
    if (imp3 > sizeof(mp3buffer)) {
        error_printf
            ("Error writing LAME-tag frame: buffer too small: buffer size=%d  frame size=%d\n",
             sizeof(mp3buffer), imp3);
        return -1;
    }
    assert( offset <= LONG_MAX );
    if (fseek(outf, (long) offset, SEEK_SET) != 0) {
        error_printf("fatal error: can't update LAME-tag frame!\n");
        return -1;
    }
    owrite = fwrite(mp3buffer, 1, imp3, outf);
    if (owrite != imp3) {
        error_printf("Error writing LAME-tag \n");
        return -1;
    }
    if (global_ui_config.silent <= 0) {
        console_printf("done\n");
    }
    assert( imp3 <= INT_MAX );
    return (int) imp3;
}


static int
write_id3v1_tag(lame_t gf, FILE * outf)
{
    unsigned char mp3buffer[128];
    size_t  imp3, owrite;

    imp3 = lame_get_id3v1_tag(gf, mp3buffer, sizeof(mp3buffer));
    if (imp3 == 0) {
        return 0;
    }
    if (imp3 > sizeof(mp3buffer)) {
        error_printf("Error writing ID3v1 tag: buffer too small: buffer size=%d  ID3v1 size=%d\n",
                     sizeof(mp3buffer), imp3);
        return 0;       /* not critical */
    }
    owrite = fwrite(mp3buffer, 1, imp3, outf);
    if (owrite != imp3) {
        error_printf("Error writing ID3v1 tag \n");
        return 1;
    }
    return 0;
}


static int
lame_encoder_loop(lame_global_flags * gf, FILE * outf, char *inPath, char *outPath)
{
    unsigned char mp3buffer[LAME_MAXMP3BUFFER];
    int     Buffer[2][1152];
    int     iread, imp3, owrite, in_limit=0;
    size_t  id3v2_size;

    encoder_progress_begin(gf, inPath, outPath);

    id3v2_size = lame_get_id3v2_tag(gf, 0, 0);
    if (id3v2_size > 0) {
        unsigned char *id3v2tag = malloc(id3v2_size);
        if (id3v2tag != 0) {
            size_t  n_bytes = lame_get_id3v2_tag(gf, id3v2tag, id3v2_size);
            size_t  written = fwrite(id3v2tag, 1, n_bytes, outf);
            free(id3v2tag);
            if (written != n_bytes) {
                error_printf("Error writing ID3v2 tag \n");
                return 1;
            }
        }
    }
    if (global_writer.flush_write == 1) {
        fflush(outf);
    }

    /* do not feed more than in_limit PCM samples in one encode call
       otherwise the mp3buffer is likely too small
     */
    in_limit = lame_get_maximum_number_of_samples(gf, sizeof(mp3buffer));
    if (in_limit < 1)
        in_limit = 1;

    /* encode until we hit eof */
    do {
        /* read in 'iread' samples */
        iread = get_audio(gf, Buffer);

        if (iread >= 0) {
            const int* buffer_l = Buffer[0];
            const int* buffer_r = Buffer[1];
            int     rest = iread;
            do {
                int const chunk = rest < in_limit ? rest : in_limit;

                /* encode */

                imp3 = lame_encode_buffer_int(gf, buffer_l, buffer_r, chunk,
                                              mp3buffer, sizeof(mp3buffer));
                buffer_l += chunk;
                buffer_r += chunk;
                rest -= chunk;

                /* was our output buffer big enough? */
                if (imp3 < 0) {
                    if (imp3 == -1)
                        error_printf("mp3 buffer is not big enough... \n");
                    else
                        error_printf("mp3 internal error:  error code=%i\n", imp3);
                    return 1;
                }
                owrite = (int) fwrite(mp3buffer, 1, imp3, outf);
                if (owrite != imp3) {
                    error_printf("Error writing mp3 output \n");
                    return 1;
                }
            } while (rest > 0);
        }
        if (global_writer.flush_write == 1) {
            fflush(outf);
        }
    } while (iread > 0);

    imp3 = lame_encode_flush(gf, mp3buffer, sizeof(mp3buffer)); /* may return one more mp3 frame */

    if (imp3 < 0) {
        if (imp3 == -1)
            error_printf("mp3 buffer is not big enough... \n");
        else
            error_printf("mp3 internal error:  error code=%i\n", imp3);
        return 1;

    }

    owrite = (int) fwrite(mp3buffer, 1, imp3, outf);
    if (owrite != imp3) {
        error_printf("Error writing mp3 output \n");
        return 1;
    }
    if (global_writer.flush_write == 1) {
        fflush(outf);
    }
    imp3 = write_id3v1_tag(gf, outf);
    if (global_writer.flush_write == 1) {
        fflush(outf);
    }
    if (imp3) {
        return 1;
    }
    write_xing_frame(gf, outf, id3v2_size);
    if (global_writer.flush_write == 1) {
        fflush(outf);
    }
    if (global_ui_config.silent <= 0) {
        print_trailing_info(gf);
    }
    return 0;
}


static int
lame_encoder(lame_global_flags * gf, FILE * outf, char *inPath, char *outPath)
{
    int     ret;

    ret = lame_encoder_loop(gf, outf, inPath, outPath);
    fclose(outf);       /* close the output file */
    close_infile();     /* close the input file */
    return ret;
}


int
lame_main(lame_t gf, int argc, char **argv)
{
    char    inPath[PATH_MAX + 1];
    char    outPath[PATH_MAX + 1];

    int     ret;
    FILE   *outf = NULL;

    lame_set_msgf(gf, &frontend_msgf);
    lame_set_errorf(gf, &frontend_errorf);
    lame_set_debugf(gf, &frontend_debugf);
    if (argc <= 1) {
        usage(stderr, argv[0]); /* no command-line args, print usage, exit  */
        return 1;
    }

    memset(inPath, 0, sizeof(inPath));

    /* parse the command line arguments, setting various flags in the
     * struct 'gf'.  If you want to parse your own arguments,
     * or call libmp3lame from a program which uses a GUI to set arguments,
     * skip this call and set the values of interest in the gf struct.
     * (see the file API and lame.h for documentation about these parameters)
     */
    ret = parse_args(gf, argc, argv, inPath, outPath);
    if (ret < 0) {
        return ret == -2 ? 0 : 1;
    }

    /* initialize input file.  This also sets samplerate and as much
       other data on the input file as available in the headers */
    outf = init_files(gf, inPath, outPath);
    if (outf == NULL) {
        close_infile();
        return -1;
    }
    /* turn off automatic writing of ID3 tag data into mp3 stream 
     * we have to call it before 'lame_init_params', because that
     * function would spit out ID3v2 tag data.
     */
    lame_set_write_id3tag_automatic(gf, 0);

    /* Now that all the options are set, lame needs to analyze them and
     * set some more internal options and check for problems
     */
    ret = lame_init_params(gf);
    if (ret < 0) {
        if (ret == -1) {
            display_bitrates(stderr);
        }
        error_printf("fatal error during initialization\n");
        fclose(outf);
        close_infile();
        return ret;
    }

    ret = lame_encoder(gf, outf, inPath, outPath);
    return ret;
}
