/*
 *      Get Audio routines source file
 *
 *      Copyright (c) 1999 Albert L Faber
 *                    2008-2017 Robert Hegemann
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/* $Id$ */

/* Reduced to WAV file input: PCM (8/16/24/32 bit) and IEEE float (32 bit), plain or
 * WAVE_FORMAT_EXTENSIBLE. AIFF, raw PCM, MPEG input, libsndfile, reading from stdin and
 * writing to stdout are gone. */


#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>

#ifdef HAVE_LIMITS_H
# include <limits.h>
#endif

#include <stdio.h>

#ifdef STDC_HEADERS
# include <stdlib.h>
# include <string.h>
#endif

#ifdef HAVE_INTTYPES_H
# include <inttypes.h>
#else
# ifdef HAVE_STDINT_H
#  include <stdint.h>
# endif
#endif

#define         MAX_U_32_NUM            0xFFFFFFFF


#include <sys/types.h>
#include <sys/stat.h>

#include "lame.h"
#include "main.h"
#include "get_audio.h"
#include "console.h"
#include "machine.h"

#ifdef WITH_DMALLOC
#include <dmalloc.h>
#endif


static uint32_t uint32_high_low(unsigned char const *bytes)
{
    uint32_t const hh = bytes[0];
    uint32_t const hl = bytes[1];
    uint32_t const lh = bytes[2];
    uint32_t const ll = bytes[3];
    return (hh << 24) | (hl << 16) | (lh << 8) | ll;
}


static uint16_t
read_16_bits_low_high(FILE * fp)
{
    unsigned char bytes[2] = { 0, 0 };
    fread(bytes, 1, 2, fp);
    {
        uint16_t const l = bytes[0];
        uint16_t const h = bytes[1];
        return (h << 8) | l;
    }
}


static uint32_t
read_32_bits_low_high(FILE * fp)
{
    unsigned char bytes[4] = { 0, 0, 0, 0 };
    fread(bytes, 1, 4, fp);
    {
        uint32_t const ll = bytes[0];
        uint32_t const lh = bytes[1];
        uint32_t const hl = bytes[2];
        uint32_t const hh = bytes[3];
        return (hh << 24) | (hl << 16) | (lh << 8) | ll;
    }
}

static uint32_t
read_32_bits_high_low(FILE * fp)
{
    unsigned char bytes[4] = { 0, 0, 0, 0 };
    fread(bytes, 1, 4, fp);
    return uint32_high_low(bytes);
}


/* global data for get_audio.c. */
typedef struct get_audio_global_data_struct {
    int     count_samples_carefully;
    int     pcmbitwidth;
    int     pcm_is_ieee_float;
    unsigned int num_samples_read;
    FILE   *music_in;
} get_audio_global_data;

static get_audio_global_data global;


static int read_samples_pcm(FILE * musicin, int sample_buffer[2304], int samples_to_read);
static FILE *open_wave_file(lame_t gfp, char const *inPath);
static int close_input_file(FILE * musicin);


/* The input is always a regular file, so a forward fseek() cannot fail the way it can on a
   pipe. */
static int
fskip_uint32(FILE * fp, uint32_t offset)
{
    while (offset > INT_MAX) {
        if (fseek(fp, INT_MAX, SEEK_CUR) != 0) {
            return -1;
        }
        offset -= INT_MAX;
    }
    if (offset > 0 && fseek(fp, (long) offset, SEEK_CUR) != 0) {
        return -1;
    }
    return 0;
}

static  off_t
lame_get_file_size(FILE * fp)
{
    struct stat sb;
    int     fd = fileno(fp);

    if (0 == fstat(fd, &sb))
        return sb.st_size;
    return (off_t) - 1;
}


FILE   *
init_outfile(char const *outPath)
{
    /* open the output file */
    return lame_fopen(outPath, "w+b");
}


int
init_infile(lame_t gfp, char const *inPath)
{
    /* open the input file */
    global. count_samples_carefully = 0;
    global. num_samples_read = 0;
    global. pcmbitwidth = 16;
    global. pcm_is_ieee_float = 0;
    global. music_in = open_wave_file(gfp, inPath);
    return global.music_in != NULL ? 1 : -1;
}

void
close_infile(void)
{
    close_input_file(global.music_in);
    global. music_in = 0;
}


/************************************************************************
*
* get_audio()
*
* PURPOSE:  reads a frame of audio data from a file to the buffer,
*   aligns the data for future processing, and separates the
*   left and right channels
*
* returns: samples read
*
************************************************************************/
int
get_audio(lame_t gfp, int buffer[2][1152])
{
    const int num_channels = lame_get_num_channels(gfp);
    const int framesize = lame_get_framesize(gfp);
    int     insamp[2 * 1152];
    int     samples_read;
    int     samples_to_read;
    int     i;
    int    *p;

    /* sanity checks, that's what we expect to be true */
    if ((num_channels < 1 || 2 < num_channels)
      ||(framesize < 1 || 1152 < framesize)) {
        if (global_ui_config.silent < 10) {
            error_printf("Error: internal problem!\n");
        }
        return -1;
    }

    /*
     * NOTE: LAME can now handle arbritray size input data packets,
     * so there is no reason to read the input data in chuncks of
     * size "framesize".  EXCEPT:  the LAME graphical frame analyzer
     * will get out of sync if we read more than framesize worth of data.
     */

    samples_to_read = framesize;

    /* if this flag has been set, then we are carefull to read
     * exactly num_samples and no more.  This is useful for .wav
     * files which have id3 or other tags at the end.
     */
    if (global.count_samples_carefully) {
        unsigned int tmp_num_samples, remaining;
        /* get num_samples */
        tmp_num_samples = lame_get_num_samples(gfp);
        if (global.num_samples_read < tmp_num_samples) {
            remaining = tmp_num_samples - global.num_samples_read;
        }
        else {
            remaining = 0;
        }
        if (remaining < (unsigned int) framesize && 0 != tmp_num_samples)
            samples_to_read = remaining;
    }

    samples_read = read_samples_pcm(global.music_in, insamp, num_channels * samples_to_read);
    if (samples_read < 0) {
        return samples_read;
    }
    p = insamp + samples_read;
    samples_read /= num_channels;
    if (num_channels == 2) {
        for (i = samples_read; --i >= 0;) {
            buffer[1][i] = *--p;
            buffer[0][i] = *--p;
        }
    }
    else if (num_channels == 1) {
        memset(buffer[1], 0, samples_read * sizeof(int));
        for (i = samples_read; --i >= 0;) {
            buffer[0][i] = *--p;
        }
    }
    else
        assert(0);

    if (global_reader.swap_channel) {
        for (i = 0; i < samples_read; ++i) {
            int const tmp = buffer[0][i];
            buffer[0][i] = buffer[1][i];
            buffer[1][i] = tmp;
        }
    }

    /* if ... then it is considered infinitely long.
       Don't count the samples */
    if (global.count_samples_carefully)
        global. num_samples_read += samples_read;

    return samples_read;
}


static
int set_input_num_channels(lame_t gfp, int num_channels)
{
    if (gfp) {
        if (-1 == lame_set_num_channels(gfp, num_channels)) {
            if (global_ui_config.silent < 10) {
                error_printf("Unsupported number of channels: %d\n", num_channels);
            }
            return 0;
        }
    }
    return 1;
}

static
int set_input_samplerate(lame_t gfp, int input_samplerate)
{
    if (gfp) {
        /* the input is never resampled, so only the MPEG-1 sample rates can be encoded */
        if (input_samplerate != 32000 && input_samplerate != 44100 && input_samplerate != 48000) {
            if (global_ui_config.silent < 10) {
                error_printf("Unsupported sample rate: %d Hz (supported: 32, 44.1 and 48 kHz)\n",
                             input_samplerate);
            }
            return 0;
        }
        if (-1 == lame_set_in_samplerate(gfp, input_samplerate)) {
            if (global_ui_config.silent < 10) {
                error_printf("Unsupported sample rate: %d\n", input_samplerate);
            }
            return 0;
        }
    }
    return 1;
}


/************************************************************************
unpack_read_samples - read and unpack little-endian signed or unsigned
                      single byte (8 bit) input. (used for read_samples function)
                      Output integers are stored in the native byte order.  -jd
  in: samples_to_read
      bytes_per_sample
 i/o: pcm_in
 out: sample_buffer  (must be allocated up to samples_to_read upon call)
returns: number of samples read
*/
static int
unpack_read_samples(const int samples_to_read, const int bytes_per_sample,
                    int *sample_buffer, FILE * pcm_in)
{
    int     samples_read;
    int     i;
    int    *op;              /* output pointer */
    unsigned char *ip = (unsigned char *) sample_buffer; /* input pointer */
    const int b = sizeof(int) * 8;

    {
        size_t  samples_read_ = fread(sample_buffer, bytes_per_sample, samples_to_read, pcm_in);
        assert( samples_read_ <= INT_MAX );
        samples_read = (int) samples_read_;
    }
    op = sample_buffer + samples_read;

#define GA_URS_IFLOOP( ga_urs_bps ) \
    if( bytes_per_sample == ga_urs_bps ) \
      for( i = samples_read * bytes_per_sample; (i -= bytes_per_sample) >=0;)

    GA_URS_IFLOOP(1)
        * --op = (ip[i] ^ 0x80) << (b - 8) | 0x7f << (b - 16); /* convert from unsigned */
    GA_URS_IFLOOP(2)
        * --op = ip[i] << (b - 16) | ip[i + 1] << (b - 8);
    GA_URS_IFLOOP(3)
        * --op = ip[i] << (b - 24) | ip[i + 1] << (b - 16) | ip[i + 2] << (b - 8);
    GA_URS_IFLOOP(4)
        * --op =
        ip[i] << (b - 32) | ip[i + 1] << (b - 24) | ip[i + 2] << (b - 16) | ip[i + 3] << (b -
                                                                                          8);
#undef GA_URS_IFLOOP
    if (global.pcm_is_ieee_float) {
        ieee754_float32_t const m_max = INT_MAX;
        ieee754_float32_t const m_min = -(ieee754_float32_t) INT_MIN;
        ieee754_float32_t *x = (ieee754_float32_t *) sample_buffer;
        assert(sizeof(ieee754_float32_t) == sizeof(int));
        for (i = 0; i < samples_to_read; ++i) {
            ieee754_float32_t const u = x[i];
            int     v;
            if (u >= 1) {
                v = INT_MAX;
            }
            else if (u <= -1) {
                v = INT_MIN;
            }
            else if (u >= 0) {
                v = (int) (u * m_max + 0.5f);
            }
            else {
                v = (int) (u * m_min - 0.5f);
            }
            sample_buffer[i] = v;
        }
    }
    return (samples_read);
}



/************************************************************************
*
* read_samples()
*
* PURPOSE:  reads the PCM samples from a file to the buffer
*
*  SEMANTICS:
* Reads #samples_read# number of shorts from #musicin# filepointer
* into #sample_buffer[]#.  Returns the number of samples read.
*
************************************************************************/

static int
read_samples_pcm(FILE * musicin, int sample_buffer[2304], int samples_to_read)
{
    int     samples_read;
    int     bytes_per_sample = global.pcmbitwidth / 8;

    switch (global.pcmbitwidth) {
    case 32:
    case 24:
    case 16:
    case 8:             /* WAV stores 8 bit samples unsigned */
        break;

    default:
        if (global_ui_config.silent < 10) {
            error_printf("Only 8, 16, 24 and 32 bit input files supported \n");
        }
        return -1;
    }
    if (samples_to_read < 0 || samples_to_read > 2304) {
        if (global_ui_config.silent < 10) {
            error_printf("Error: unexpected number of samples to read: %d\n", samples_to_read);
        }
        return -1;
    }
    samples_read = unpack_read_samples(samples_to_read, bytes_per_sample, sample_buffer, musicin);
    if (ferror(musicin)) {
        if (global_ui_config.silent < 10) {
            error_printf("Error reading input file\n");
        }
        return -1;
    }

    return samples_read;
}



static uint32_t const WAV_ID_RIFF = 0x52494646; /* "RIFF" */
static uint32_t const WAV_ID_WAVE = 0x57415645; /* "WAVE" */
static uint32_t const WAV_ID_FMT = 0x666d7420; /* "fmt " */
static uint32_t const WAV_ID_DATA = 0x64617461; /* "data" */

#ifndef WAVE_FORMAT_PCM
static uint16_t const WAVE_FORMAT_PCM = 0x0001;
#endif
#ifndef WAVE_FORMAT_IEEE_FLOAT
static uint16_t const WAVE_FORMAT_IEEE_FLOAT = 0x0003;
#endif
#ifndef WAVE_FORMAT_EXTENSIBLE
static uint16_t const WAVE_FORMAT_EXTENSIBLE = 0xFFFE;
#endif


static uint32_t
make_even_number_of_bytes_in_length(uint32_t x)
{
    return x + (x & 0x01);
}


/*****************************************************************************
 *
 *	Read Microsoft Wave headers
 *
 *	By the time we get here the first 32-bits of the file have already been
 *	read, and we're pretty sure that we're looking at a WAV file.
 *
 *****************************************************************************/

static int
parse_wave_header(lame_global_flags * gfp, FILE * sf)
{
    uint32_t ui32_nSamplesPerSec = 0;
    uint32_t ui32_DataChunkSize = 0;
    uint16_t ui16_wFormatTag = 0;
    uint16_t ui16_nChannels = 0;
    uint16_t ui16_wBitsPerSample = 0;

    int     is_wav = 0;
    int     loop_sanity = 0;

    uint32_t ui32_chunkSize = read_32_bits_high_low(sf); /* file_length */
    uint32_t ui32_WAVEID    = read_32_bits_high_low(sf);
    if (ui32_WAVEID != WAV_ID_WAVE || ui32_chunkSize < 1)
        return -1;

    for (loop_sanity = 0; loop_sanity < 20; ++loop_sanity) {
        uint32_t ui32_ckID = read_32_bits_high_low(sf);
        if (ui32_ckID == WAV_ID_FMT) {
            uint32_t ui32_nAvgBytesPerSec = 0;
            uint32_t ui32_cksize = 0;
            uint16_t ui16_nBlockAlign = 0;

            ui32_cksize = read_32_bits_low_high(sf);
            ui32_cksize = make_even_number_of_bytes_in_length(ui32_cksize);
            if (ui32_cksize < 16u) {
                /*DEBUGF("'fmt' chunk too short (only %ld bytes)!", ui32_cksize);*/
                return -1;
            }
            ui16_wFormatTag      = read_16_bits_low_high(sf);
            ui16_nChannels       = read_16_bits_low_high(sf);
            ui32_nSamplesPerSec  = read_32_bits_low_high(sf);
            ui32_nAvgBytesPerSec = read_32_bits_low_high(sf);
            ui16_nBlockAlign     = read_16_bits_low_high(sf);
            ui16_wBitsPerSample  = read_16_bits_low_high(sf);
            ui32_cksize -= 16u;
            /* WAVE_FORMAT_EXTENSIBLE support */
            if ((ui32_cksize > 9u) && (ui16_wFormatTag == WAVE_FORMAT_EXTENSIBLE)) {
                uint16_t ui16_cbSize              = read_16_bits_low_high(sf);
                uint16_t ui16_wValidBitsPerSample = read_16_bits_low_high(sf);
                uint32_t ui32_dwChannelMask       = read_32_bits_low_high(sf);
                uint16_t ui16_SubFormat           = read_16_bits_low_high(sf);
                ui32_cksize -= 10u;
                ui16_wFormatTag = ui16_SubFormat; /* SubType coincident with format_tag for PCM int or float */
                (void) (ui16_cbSize, ui16_wValidBitsPerSample, ui32_dwChannelMask); /* unused */
            }
            /* DEBUGF("   skipping %d bytes\n", ui32_cksize); */
            if (ui32_cksize > 0) {
                if (fskip_uint32(sf, ui32_cksize) != 0)
                    return -1;
            };
        }
        else if (ui32_ckID == WAV_ID_DATA) {
            ui32_DataChunkSize = read_32_bits_low_high(sf);
            is_wav = 1;
            /* We've found the audio data. Read no further! */
            break;
        }
        else {
            uint32_t ui32_cksize = read_32_bits_low_high(sf);
            ui32_cksize = make_even_number_of_bytes_in_length(ui32_cksize);
            if (fskip_uint32(sf, ui32_cksize) != 0) {
                return -1;
            }
        }
    }
    if (is_wav) {
        if (ui16_wFormatTag != WAVE_FORMAT_PCM && ui16_wFormatTag != WAVE_FORMAT_IEEE_FLOAT) {
            if (global_ui_config.silent < 10) {
                error_printf("Unsupported data format: 0x%04X\n", ui16_wFormatTag);
            }
            return 0;   /* oh no! non-supported format  */
        }

        /* make sure the header is sane */
        if (!set_input_num_channels(gfp, ui16_nChannels))
            return 0;
        if (!set_input_samplerate(gfp, ui32_nSamplesPerSec))
            return 0;
        /* avoid division by zero */
        if (ui16_wBitsPerSample < 1) {
            if (global_ui_config.silent < 10)
                error_printf("Unsupported bits per sample: %d\n", ui16_wBitsPerSample);
            return -1;
        }
        global. pcmbitwidth = ui16_wBitsPerSample;
        global. pcm_is_ieee_float = (ui16_wFormatTag == WAVE_FORMAT_IEEE_FLOAT ? 1 : 0);
        if (ui32_DataChunkSize == MAX_U_32_NUM)
            (void) lame_set_num_samples(gfp, MAX_U_32_NUM);
        else
            (void) lame_set_num_samples(gfp, ui32_DataChunkSize / (ui16_nChannels * ((ui16_wBitsPerSample + 7u) / 8u)));
        return 1;
    }
    return -1;
}



/************************************************************************
*
* parse_file_header
*
* PURPOSE: Read the header from a bytestream.  Try to determine whether
*		   it's a WAV file.
*          Set parameters for the input file:
*          num_channels, samplerate, num_samples
*          returns 1 for a WAV file, 0 otherwise.
*
* When this function returns, the file offset will be positioned at the
* beginning of the sound data.
*
************************************************************************/

static int
parse_file_header(lame_global_flags * gfp, FILE * sf)
{
    uint32_t ui32_type = read_32_bits_high_low(sf);

    global. count_samples_carefully = 0;

    if (ui32_type == WAV_ID_RIFF) {
        /* It's probably a WAV file */
        int const ret = parse_wave_header(gfp, sf);
        if (ret > 0) {
            if (lame_get_num_samples(gfp) == MAX_U_32_NUM || global_reader.ignorewavlength == 1)
            {
                global. count_samples_carefully = 0;
                lame_set_num_samples(gfp, MAX_U_32_NUM);
            }
            else
                global. count_samples_carefully = 1;
            return 1;
        }
        if (ret < 0) {
            if (global_ui_config.silent < 10) {
                error_printf("Warning: corrupt or unsupported WAVE format\n");
            }
        }
    }
    else {
        if (global_ui_config.silent < 10) {
            error_printf("Warning: unsupported audio format, only WAVE files are supported\n");
        }
    }
    return 0;
}


static FILE *
open_wave_file(lame_t gfp, char const *inPath)
{
    FILE   *musicin;

    /* set the defaults from info incase we cannot determine them from file */
    lame_set_num_samples(gfp, MAX_U_32_NUM);

    if ((musicin = lame_fopen(inPath, "rb")) == NULL) {
        if (global_ui_config.silent < 10) {
            error_printf("Could not find \"%s\".\n", inPath);
        }
        return 0;
    }

    if (!parse_file_header(gfp, musicin)) {
        close_input_file(musicin);
        return 0;
    }

    if (lame_get_num_samples(gfp) == MAX_U_32_NUM) {
        int const tmp_num_channels = lame_get_num_channels(gfp);
        double const flen = lame_get_file_size(musicin); /* try to figure out num_samples */
        if (flen >= 0 && tmp_num_channels > 0 ) {
            /* try file size, assume 2 bytes per sample */
            unsigned long fsize = (unsigned long) (flen / (2 * tmp_num_channels));
            (void) lame_set_num_samples(gfp, fsize);
            global. count_samples_carefully = 0;
        }
    }
    return musicin;
}


static int
close_input_file(FILE * musicin)
{
    int     ret = 0;

    if (musicin != 0) {
        ret = fclose(musicin);
    }
    if (ret != 0) {
        if (global_ui_config.silent < 10) {
            error_printf("Could not close audio input file\n");
        }
    }
    return ret;
}

/* end of get_audio.c */
