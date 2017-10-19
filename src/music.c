/*
 * Heart of The Alien: Music player (cd and mp3)
 * Copyright (c) 2004 Gil Megidish
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include <SDL.h>
#include <SDL_mixer.h>
#include <unistd.h>
#ifdef __PSP2__
#include <sys/stat.h>
#endif
#include "debug.h"
#include "music.h"
#include "cd_iso.h"
#include "client.h"

static Mix_Music *current_track;
#ifdef __PSP2__
static FILE *f;
static char *mem;
#endif

/** (The Underdogs version of) Heart of The Alien's tracks are formatted like this */
#define ISO_PREFIX "Heart Of The Alien (U) "

/** Stops mp3 player */
static void stop_music_mp3()
{
	if (current_track != NULL)
	{
		Mix_FreeMusic(current_track);
#ifdef __PSP2__
		if (f != NULL)
		{
			fclose(f);
		}
		if (mem != NULL)
		{
			free(mem);
		}
#endif
		current_track = NULL;
	}
}

/** Stops music */
void stop_music()
{
	if (cls.nosound == 0)
	{
		stop_music_mp3();
	}
}

/** Plays an mp3 in background
    @param track    track to play (appended to PREFIX)
*/
static void play_music_track_mp3(int track, int loop)
{
	char filename[256];

	stop_music_mp3();

	if (cls.iso_prefix != NULL)
	{
#ifdef __PSP2__
		snprintf(filename, 256, "ux0:data/hota/%s %02d.ogg", cls.iso_prefix, track + 1);
#else
		snprintf(filename, 256, "%s %02d.ogg", cls.iso_prefix, track + 1);
#endif
	}
	else
	{
#ifdef __PSP2__
		sprintf(filename, "ux0:data/hota/" ISO_PREFIX "%02d.ogg", track + 1);
#else
		sprintf(filename, ISO_PREFIX "%02d.ogg", track + 1);
#endif
	}
	// if ogg file doesn't exist use mp3
	if (access(filename, R_OK))
	{
		if (cls.iso_prefix != NULL)
		{
			snprintf(filename, 256, "%s %02d.mp3", cls.iso_prefix, track + 1);
		}
		else
		{
			sprintf(filename, ISO_PREFIX "%02d.mp3", track + 1);
		}
	}
	LOG(("playing mp3 %s\n", filename));

#ifdef __PSP2__
	struct stat info;
	stat(filename, &info);
	f = fopen(filename, "rb");
	mem = (char*)malloc(info.st_size);
	fread(mem, 1, info.st_size, f);
	current_track = Mix_LoadMUS_RW(SDL_RWFromMem(mem, info.st_size));
#else
	current_track = Mix_LoadMUS(filename);
#endif
	if (!current_track) {
		fprintf(stderr, "Mix_LoadMUS(\"%s\"): %s\n", filename, Mix_GetError());
	}
	Mix_PlayMusic(current_track, loop);
}

/** Plays audio track
    @param track   track to play
    @param loop    loop count
*/
void play_music_track(int track, int loop)
{
	if (cls.nosound == 0)
	{
		stop_music();

		play_music_track_mp3(track, loop);
	}
}

/** Callback after a frame has been rendered */
void music_update()
{
}

/** Module initializer */
void music_init()
{
	if (cls.nosound == 0)
	{

	}
}
