/*
 *      time status related function source file
 *
 *      Copyright (c) 1999 Mark Taylor
 *                    2010 Robert Hegemann
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


#include <string.h>
#include <stdlib.h>

#include "lame.h"
#include "main.h"
#include "timestatus.h"
#include "console.h"

#ifdef WITH_DMALLOC
#include <dmalloc.h>
#endif


/* The progress report and the VBR histogram were removed (ctLame plan step 5); only the
 * information printed before encoding starts remains. */

void
encoder_progress_begin( lame_global_flags const* gf
                      , char              const* inPath
                      , char              const* outPath
                      )
{
    if (global_ui_config.silent < 9) {
        size_t pw = console_getwidth()-14;
        char* i_file = 0;
        char* o_file = 0;
#if defined( _WIN32 ) && !defined(__MINGW32__)
        inPath = i_file = utf8ToConsole8Bit(inPath);
        outPath = o_file = utf8ToConsole8Bit(outPath);
#endif
        lame_print_config(gf); /* print useful information about options being used */

        console_printf("Encoding %s%s to %s\n",
                       inPath,
                       strlen(inPath) + strlen(outPath) < pw ? "" : "\n     ",
                       outPath);

        free(i_file);
        free(o_file);

        console_printf("Encoding as %g kHz ", 1.e-3 * lame_get_out_samplerate(gf));

        {
            static const char *mode_names[2][4] = {
                {"stereo", "j-stereo", "dual-ch", "single-ch"},
                {"stereo", "force-ms", "dual-ch", "single-ch"}
            };
            console_printf("%s MPEG-1 Layer III VBR(q=%g)\n",
                           mode_names[lame_get_force_ms(gf)][lame_get_mode(gf)],
                           lame_get_VBR_quality(gf));
        }

        if (global_ui_config.silent <= -10) {
            lame_print_internals(gf);
        }
    }
}
