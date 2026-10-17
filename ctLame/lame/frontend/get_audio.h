/*
 *    Get Audio routines include file
 *
 *    Copyright (c) 1999 Albert L Faber
 *                  2010 Robert Hegemann
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


#ifndef LAME_GET_AUDIO_H
#define LAME_GET_AUDIO_H
#include "lame.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

int     init_infile(lame_t gfp, char const * inPath);
void    close_infile(void);
int     get_audio(lame_t gfp, int buffer[2][1152]);

FILE   *init_outfile(char const *outPath);

#ifdef __cplusplus
}
#endif

#endif /* ifndef LAME_GET_AUDIO_H */
