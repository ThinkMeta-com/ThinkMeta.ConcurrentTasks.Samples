/*
 *      Command line frontend program
 *
 *      Copyright (c) 1999 Mark Taylor
 *                    2000 Takehiro TOMIANGA
 *                    2010-2011 Robert Hegemann
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

#ifndef MAIN_H_INCLUDED
#define MAIN_H_INCLUDED

#ifdef HAVE_LIMITS_H
# include <limits.h>
#endif

#include "get_audio.h"

#if defined(__cplusplus)
extern "C" {
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif


/* GLOBAL VARIABLES used by parse.c and main.c.  
   instantiated in parce.c.  ugly, ugly */

typedef struct ReaderConfig
{
    int   swap_channel;             /* 0: no-op, 1: swaps input channels */
    int   ignorewavlength;
} ReaderConfig;

typedef struct WriterConfig
{
    int   flush_write;
} WriterConfig;

typedef struct UiConfig
{
    int   silent;                   /* Verbosity */
} UiConfig;

extern ReaderConfig global_reader;
extern WriterConfig global_writer;
extern UiConfig global_ui_config;


extern FILE* lame_fopen(char const* file, char const* mode);
extern char* utf8ToConsole8Bit(const char* str);
#ifdef _WIN32
extern wchar_t* utf8ToUnicode(char const* mbstr);
extern char *unicodeToUtf8(const wchar_t *wstr);
#endif

extern void dosToLongFileName(char* filename);
extern void setProcessPriority(int priority);

extern int lame_main(lame_t gf, int argc, char** argv);
extern char* lame_getenv(char const* var);

#if defined(__cplusplus)
}
#endif

#endif
