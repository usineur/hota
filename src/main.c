/*
 * Heart of The Alien: Game loop and main
 * Copyright (c) 2004-2005 Gil Megidish
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <SDL.h>
#include <SDL_mixer.h>

#include "client.h"
#include "vm.h"
#include "rooms.h"
#include "debug.h"
#include "sound.h"
#include "music.h"
#include "common.h"
#include "cd_iso.h"
#include "decode.h"
#include "render.h"
#include "screen.h"
#include "sprites.h"
#include "game2bin.h"
#include "animation.h"
#include "getopt.h"

#ifdef __SWITCH__
#define BTN_A      0
#define BTN_X      2
#define BTN_Y      3
#define BTN_L      6
#define BTN_R      7
#define BTN_ZR     9
#define BTN_PLUS  10
#define BTN_MINUS 11
#define BTN_LEFT  12
#define BTN_UP    13
#define BTN_RIGHT 14
#define BTN_DOWN  15
#endif

static char *VERSION = "1.2.4";

static char *QUICKSAVE_FILENAME = "quicksave";
static char *RECORDED_KEYS_FILENAME = "recorded-keys";
static char *SWITCH_FILENAME = "temp";

typedef struct anm_file_s
{
	int track;
	const char *filename;
	int offset;
} anm_file_t;

static anm_file_t anm_files[] =
{
	{31, "INTRO1.BIN", 0},
	{32, "INTRO2.BIN", 0},
	{33, "INTRO3.BIN", 0},
	{34, "INTRO4.BIN", 0},
	{35, "MAKE2MB.BIN", 0x109a},
	{36, "MID2.BIN", 0},
	{37, "END1.BIN", 0},
	{38, "END2.BIN", 0},
	{39, "END3.BIN", 0},
	{40, "END4.BIN", 0}
};

///////
extern int script_ptr;

int next_script;
int current_backdrop;
int current_room;

int speed_throttle = 0;

int debug_flag = 0;
int test_flag = 0;
int record_flag = 0;
int replay_flag = 0;
int fullscreen_flag = 0;
int fastest_flag = 0;

short task_pc[64];
short new_task_pc[64];
short enabled_tasks[64];
short new_enabled_tasks[64];

static int key_up, key_down, key_left, key_right, key_a, key_b, key_c, key_select;
static int key_reset_record;

static Uint32 last_tick = 0;
static Uint32 last_tick_fp = 0;

#define RECORDED_KEYS_CACHE 4096
static int cached_keys_offset = 0;
static unsigned char cached_recorded_keys[RECORDED_KEYS_CACHE];

/** file descriptor where keys are written to, or read from */
FILE *record_fp = 0;

#if SDL_VERSION_ATLEAST(2, 0, 0)
SDL_Window *window;
SDL_Renderer *renderer;
SDL_Surface *_screen;
SDL_Texture *texture;
#endif
SDL_Surface *screen;

/** scratchpad used for unpacking code */
static unsigned char scratchpad[29184];


static int load_room(int index)
{
	char filename[16];
	unsigned char *ptr;

	strcpy(filename, "ROOMS0.BIN");
	filename[5] = (index + '0');

	LOG(("loading %s\n", filename));
	ptr = get_memory_ptr(0xf900);
	if (read_file(filename, ptr) < 0)
	{
		panic("load_room failed");
	}

	script_ptr = get_long(0xf900);
	LOG(("script ptr %x\n", script_ptr));

	sound_flush_cache();

	return 0;
}

/** atexit() callback
*/
static void atexit_callback(void)
{
	stop_music();
	SDL_Quit();
}

static int initialize()
{
#ifdef __SWITCH__
	SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_JOYSTICK);
	SDL_JoystickOpen(0);
#else
	SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO);
#endif
	atexit(atexit_callback);

	if (cls.nosound == 0)
	{
		if (Mix_OpenAudio(44100, AUDIO_S16, 2, 4096) < 0)
		{
			panic("Mix_OpenAudio failed\n");
		}

		music_init();
		sound_init();
	}

	if (render_init() < 0)
	{
		panic("failed to initialize renderer module");
	}

	if (game2bin_init() < 0)
	{
		panic("can't read GAME2.BIN file");
	}

	screen_init();

	vm_reset();
	set_variable(227, 1);

	if (render_create_surface() < 0)
	{
		panic("failed to create video surface");
	}

	return 0;
}

/** Loads a screen from room file
    @param room    suffix for room%d.bin file
    @param index   screen number
*/
void load_room_screen(int room, int index)
{
	int i;
	unsigned char *pixels;

	LOG(("loading room screen %d from room file %d\n", index - 1, room));

	unpack_room(scratchpad, index - 1);

	/* convert 4bpp -> 8bpp */
	pixels = (unsigned char *) get_screen_ptr(0);
	for (i=0; i<304*192/2; i++)
	{
		pixels[i*2+0] = scratchpad[i] >> 4;
		pixels[i*2+1] = scratchpad[i] & 0xf;
	}

	current_backdrop = index;
}

/** Rewinds (clears) the recorded-keys cache
*/
void rewind_recorded_keys()
{
	cached_keys_offset = 0;
}

/** Writes down all the recored keys, and rewinds
*/
void flush_recorded_keys()
{
	fwrite(cached_recorded_keys, 1, cached_keys_offset, record_fp);
	cached_keys_offset = 0;
}

/** Reads keystate from record file

    Will read information sufficient for one rendered frame; if the
    record file ended already, all keys will be considered 'released'
*/
void read_keys_from_record()
{
	int c = fgetc(record_fp);

	if (c == EOF)
	{
		c = 0;
		LOG(("ERROR: record file ended!\n"));
	}

	key_up = (c >> 7) & 1;
	key_down = (c >> 6) & 1;
	key_left = (c >> 5) & 1;
	key_right = (c >> 4) & 1;
	key_a = (c >> 3) & 1;
	key_b = (c >> 2) & 1;
	key_c = (c >> 1) & 1;
	key_select = (c >> 0) & 1;
}

/** Adds a single key to the record file

    Adds a key to the cache array of recorded keys; if the array
    is full, it will force flushing
*/
void add_keys_to_record()
{
	int c;

	c = (key_up << 7) | (key_down << 6);
	c = c | (key_left << 5) | (key_right << 4);
	c = c | (key_a << 3) | (key_b << 2);
	c = c | (key_c << 1) | key_select;

 	cached_recorded_keys[cached_keys_offset++] = c;
 	if (cached_keys_offset == sizeof(cached_recorded_keys))
	{
 		/* if the player never quicksaves or quickloads */
		flush_recorded_keys();
	}
}

/** Translates gamepad presses and updates variables

    Entry at b6c4
*/
void update_keys()
{
	short flags;

	toggle_aux(0);           /* me and my paranoia */
	set_variable(253, 0);
	set_variable(252, 0);
	set_variable(229, 0);
	set_variable(251, 0);

	flags = 0;

	if (key_right)
	{
		set_variable(252, 1);
		flags |= 1;
	}
	else if (key_left)
	{
		set_variable(252, -1);
		flags |= 2;
	}
	else if (key_down)
	{
		set_variable(251, 1);
		set_variable(229, 1);
		flags |= 4;
	}

	if (key_up)
	{
		set_variable(229, -1);
		flags |= 8;
	}

	if (key_c)
	{
		set_variable(251, -1);
		flags |= 8;
		/* some if  here! */
	}

	set_variable(253, flags);

	/* b748 */
	set_variable(250, 0);
	set_variable(254, get_variable(253));

	if (key_b)
	{
		set_variable(254, (unsigned short)(get_variable(254) | 0x40));
	}
	else if (key_a)
	{
		set_variable(250, 1);
		set_variable(254, (unsigned short)(get_variable(254) | 0x80));
	}
}

/** Loads a quicksave file
*/
void quickload(char *filename)
{
	int i, j;
	int palette_used;
	FILE *fp;

	if (current_room == 0)
	{
		return;
	}

	fp = fopen(filename, "rb");
	if (fp == NULL)
	{
		if (filename == SWITCH_FILENAME)
		{
			return;
		}
		LOG(("failed to load 'quicksave' file\n"));
		return;
	}

	current_room = fgetc(fp);
	current_backdrop = fgetc(fp);
	palette_used = fgetc(fp);

	load_room(current_room);
	load_room_screen(0, current_backdrop);
	set_palette(palette_used);

	/* must be ran out of thread loop, so no active thread */
	toggle_aux(0);
	for (i=0; i<256; i++)
	{
		set_variable(i, fgetw(fp));
	}

	toggle_aux(1);
	for (j=0; j<MAX_TASKS; j++)
	{
		set_aux_bank(j);
		for (i=0; i<32; i++)
		{
			set_variable(i, fgetw(fp));
		}
	}

	/* when frame ends, aux is always zero anyway */
	toggle_aux(0);

	for (i=0; i<MAX_TASKS; i++)
	{
		task_pc[i] = fgetw(fp);
		new_task_pc[i] = fgetw(fp);
		enabled_tasks[i] = fgetw(fp);
		new_enabled_tasks[i] = fgetw(fp);
	}

	quickload_sprites(fp);

	fclose(fp);
}

/** Creates a quicksave file

    Quicksave file includes all that is required so later on a user can
    use the quickload and be provided with the exact same game state.
*/
void quicksave(char *filename)
{
	int i, j;
	FILE *fp;

	if (current_room == 0)
	{
		return;
	}

	fp = fopen(filename, "wb");
	if (fp == NULL)
	{
		if (filename == SWITCH_FILENAME)
		{
			return;
		}
		LOG(("failed to create 'quicksave' file\n"));
		return;
	}

	fputc(current_room, fp);
	fputc(current_backdrop, fp);
	fputc(get_current_palette(), fp);

	/* must be ran out of thread loop, so no active thread */
	toggle_aux(0);
	for (i=0; i<256; i++)
	{
		fputw(get_variable(i), fp);
	}

	toggle_aux(1);
	for (j=0; j<MAX_TASKS; j++)
	{
		set_aux_bank(j);
		for (i=0; i<32; i++)
		{
			fputw(get_variable(i), fp);
		}
	}

	toggle_aux(0);

	for (i=0; i<MAX_TASKS; i++)
	{
		fputw(task_pc[i], fp);
		fputw(new_task_pc[i], fp);
		fputw(enabled_tasks[i], fp);
		fputw(new_enabled_tasks[i], fp);
	}

	quicksave_sprites(fp);
	fclose(fp);
}

void leave_game()
{
	flush_recorded_keys();
	exit(0);
}

/** Processes SDL events

    This processes windows messages, keyboard pressed, joystick moves,
    and general SDL events
*/
void check_events()
{
	SDL_Event event;

	while (SDL_PollEvent(&event))
	{
	        switch (event.type)
		{
			case SDL_KEYUP:
			switch(event.key.keysym.sym)
			{
				case SDLK_RIGHT:
				key_right = 0;
				break;

				case SDLK_LEFT:
				key_left = 0;
				break;

				case SDLK_UP:
				key_up = 0;
				break;

				case SDLK_DOWN:
				key_down = 0;
				break;

				case SDLK_z:
				case SDLK_a:
				key_a = 0;
				break;

				case SDLK_x:
				case SDLK_s:
				key_b = 0;
				break;

				case SDLK_c:
				case SDLK_d:
				key_c = 0;
				break;

				case SDLK_PAGEDOWN:
				if (cls.pandora)
				{
					key_a = 0;
				}
				break;

				case SDLK_END:
				if (cls.pandora)
				{
					key_b = 0;
				}
				break;

				case SDLK_HOME:
				if (cls.pandora)
				{
					key_c = 0;
				}
				break;

				case SDLK_p:
				if (current_room == 7)
				{
					quickload(SWITCH_FILENAME);
				} else if (current_room != 0)
				{
					quicksave(SWITCH_FILENAME);
					next_script = 7;
				}
				break;

				case SDLK_q:
				key_a = 0;
				key_reset_record = 0;
				break;

				case SDLK_SPACE:
				speed_throttle = 0;
				break;

				default:
				/* keep -Wall happy */
				break;
			}
			break;

	        	case SDL_KEYDOWN:
			switch(event.key.keysym.sym)
			{
				#ifdef ENABLE_DEBUG
				/* enable/disable sprites */
				case SDLK_1:
				case SDLK_2:
				case SDLK_3:
				case SDLK_4:
				case SDLK_5:
				case SDLK_6:
				case SDLK_7:
				case SDLK_8:
				case SDLK_9:
				{
					int tmp = event.key.keysym.sym - SDLK_1 + 1;

					if (event.key.keysym.mod & KMOD_SHIFT)
					{
						tmp = tmp + 10;
					}

					sprites[tmp].u1 ^= 0x80;
				}
				break;
				#endif

				case SDLK_ESCAPE:
				cls.quit = 1;
				break;

				case SDLK_RIGHT:
				key_right = 1;
				break;

				case SDLK_LEFT:
				key_left = 1;
				break;

				case SDLK_UP:
				key_up = 1;
				break;

				case SDLK_DOWN:
				key_down = 1;
				break;

				case SDLK_z:
				case SDLK_a:
				key_a = 1;
				break;

				case SDLK_x:
				case SDLK_s:
				key_b = 1;
				break;

				case SDLK_c:
				case SDLK_d:
				key_c = 1;
				break;

				case SDLK_PAGEDOWN:
				if (cls.pandora)
				{
					key_a = 1;
				}
				break;

				case SDLK_END:
				if (cls.pandora)
				{
					key_b = 1;
				}
				break;

				case SDLK_HOME:
				if (cls.pandora)
				{
					key_c = 1;
				}
				break;

				#ifdef ENABLE_DEBUG
				case SDLK_g:
				debug_flag ^= 1;
				break;
				#endif

				case SDLK_F5:
				quicksave(QUICKSAVE_FILENAME);
				break;

				case SDLK_F7:
				quickload(QUICKSAVE_FILENAME);
				break;

				case SDLK_RETURN:
				if (event.key.keysym.mod & KMOD_ALT)
				{
					if (!cls.pandora)
					{
						toggle_fullscreen();
					}
				}
				break;

				case SDLK_q:
				key_a = 1;
				key_reset_record = 1;
				break;

				case SDLK_SPACE:
				speed_throttle = 1;
				break;

				default:
				/* keep -Wall happy */
				break;
			}
			break;

			case SDL_QUIT:
			leave_game();
			break;

#ifdef __SWITCH__
			case SDL_JOYBUTTONDOWN:
			switch (event.jbutton.button)
			{
				case BTN_LEFT:  key_left = 1; break;
				case BTN_RIGHT: key_right = 1; break;
				case BTN_UP:    key_up = 1; break;
				case BTN_DOWN:  key_down = 1; break;
				case BTN_A:     key_a = 1; break; // run
				case BTN_ZR:    key_a = 1; break; // run (alter)
				case BTN_Y:     key_b = 1; break; // whip
				case BTN_X:     key_c = 1; break; // jump
				case BTN_L:     quickload(QUICKSAVE_FILENAME); break;
				case BTN_R:     quicksave(QUICKSAVE_FILENAME); break;
				case BTN_PLUS:
				if (current_room == 7)
				{
					quickload(SWITCH_FILENAME);
				} else if (current_room != 0)
				{
					quicksave(SWITCH_FILENAME);
					next_script = 7;
				}
				break;
				case BTN_MINUS: cls.filtered = !cls.filtered; break;
			}
			break;

			case SDL_JOYBUTTONUP:
			switch (event.jbutton.button)
			{
				case BTN_LEFT:  key_left = 0; break;
				case BTN_RIGHT: key_right = 0; break;
				case BTN_UP:    key_up = 0; break;
				case BTN_DOWN:  key_down = 0; break;
				case BTN_A:     key_a = 0; break;
				case BTN_ZR:    key_a = 0; break;
				case BTN_Y:     key_b = 0; break;
				case BTN_X:     key_c = 0; break;
			}
			break;
#endif
		}
	}
}

void rest(int fps)
{
	if (fastest_flag == 0)
	{
		if (fps == 0)
		{
			last_tick = SDL_GetTicks();
			last_tick_fp = 0;
			return;
		}

		if (speed_throttle == 1)
		{
			/* 10 times faster */
			fps = fps*10;
		}

		Uint32 diff = ((1000 << 16) / fps) + last_tick_fp;
		last_tick_fp = diff & 0xffff;
		diff = diff >> 16;
		Uint32 current_tick = SDL_GetTicks();
		while (current_tick - last_tick < diff)
		{
			SDL_Delay(1);
			current_tick = SDL_GetTicks();
		}
		last_tick += diff;
	}
}

/** Initializes all tasks to stopped state
*/
void init_tasks()
{
	int i;

	for (i=0; i<MAX_TASKS; i++)
	{
		task_pc[i] = -1;
		new_task_pc[i] = -1;

		enabled_tasks[i] = 0;
		new_enabled_tasks[i] = 0;
	}

	toggle_aux(0);
	task_pc[0] = 0;
}

/** Plays several animations at once
    @param anm        pointer to an array of anm_file_t entries
    @param n          number of elements in array
    @param skippable  skip to next animation if key pressed (otherwise breaks)
    @returns zero on completion of all videos, 1 if skipped, negative on error

    Note that cls.quit might be true; in that case, return value of one will
    be returned
*/
int play_anm(anm_file_t *anm, int n, int skippable)
{
	int seq;
	int ret;

	ret = 0;
	for (seq = 0; seq < n; seq++)
	{
		if (cls.quit == 0)
		{
			int ok;

			play_music_track(anm[seq].track, 0);
			ok = play_animation(anm[seq].filename, anm[seq].offset);
			if (ok < 0)
			{
				ret = ok;
				break;
			}

			if (ok == 1 && skippable == 0)
			{
				ret = 1;
				break;
			}
		}
	}

	stop_music();
	return ret;
}

void draw_screen()
{
#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_BlitSurface(screen, NULL, _screen, NULL);
	SDL_UpdateTexture(texture, NULL, _screen->pixels, _screen->pitch);
	SDL_RenderClear(renderer);
#ifdef __SWITCH__
	SDL_Rect src;
	src.x = 0;
	src.y = 0;
	src.w = _screen->w;
	src.h = _screen->h;

	SDL_Rect dst;
	dst.h = 720;
	dst.w = (float)src.w * ((float)dst.h / (float)src.h);
	dst.y = 0;
	dst.x = (1280 - dst.w) / 2;

	SDL_RenderCopy(renderer, texture, &src, &dst);
#else
	SDL_RenderCopy(renderer, texture, NULL, NULL);
#endif
	SDL_RenderPresent(renderer);
#else
	SDL_UpdateRect(screen, 0, 0, 0, 0);
#endif
}

/** Plays the introduction to the game

    Introduction is split into 4 files, since sega-cd was limited with
    512 KB of ram, and video is loaded into memory before it can be
    played. also, each such sequence comes with it's own audio track
*/
static void play_intro()
{
	play_anm(anm_files, 4, 0);
}

/** Main game loop

    This is where all the magic happens!
*/
static void run()
{
	cls.quit = 0;
	init_tasks();

	if (next_script == 0)
	{
		/* if no room specified, then follow the original flow:
		 * first play intro, then jump to code entry script.
		 */
		play_intro();
		next_script = 7;
	}

	rest(0);

	while (cls.quit == 0)
	{
		int i;

		if (next_script != 0)
		{
			current_room = next_script;
			reset_sprite_list();
			init_tasks();
			LOG(("loading room %d\n", current_room));
			load_room(current_room);
			next_script = 0;
		}

		check_events();

		if (replay_flag)
		{
			read_keys_from_record();
		}

		update_keys();

		if (record_flag)
		{
			add_keys_to_record();
		}

		LOG(("*new frame*\n"));

		for (i=0; i<MAX_TASKS; i++)
		{
			int d0;

			/* 70d0 */
			enabled_tasks[i] = new_enabled_tasks[i];

			d0 = new_task_pc[i];
			if (d0 == -1)
			{
				continue;
			}

			if (d0 == -2)
			{
				d0 = -1;
			}

			task_pc[i] = d0;
			new_task_pc[i] = -1;
		}

		for (i=0; i<MAX_TASKS; i++)
		{
			int pc = task_pc[i];
			if (pc != INVALID_PC && enabled_tasks[i] == 0)
			{
				toggle_aux(0);
				set_aux_bank(i);
				LOG(("task %d starts at 0x%x\n", i, pc));
				task_pc[i] = decode(i, pc);
				LOG(("task %d ended at 0x%x\n", i, pc));
			}

			if (next_script != 0)
			{
				/* script has been changed */
				break;
			}
		}

		draw_screen();
		music_update();

		rest(12);
	}
}

/** Runs the animation-test, enabled with --animation-test
*/
static void animation_test()
{
	int files = sizeof(anm_files) / sizeof(anm_files[0]);
	play_anm(anm_files, files, 1);
}

/** Runs the sprite-test

    Interactively show the sprites in this 'room' file. Use the --room
    parameter to view sprites in other rooms
*/
void sprite_test()
{
	int redraw;

	load_room(next_script);

	sprites[0].index = 0;
	sprites[0].frame = 0;
	sprites[0].x = 10;
	sprites[0].y = 10;

	cls.quit = 0;

	redraw = 1;
	set_palette(0x11);
	rest(0);
	while (cls.quit == 0)
	{
		int a4;

		a4 = get_long(0xf904) + (sprites[0].index << 2);
		a4 = get_long(a4);

		if (redraw)
		{
			int selected_screen;
			void *background;

			selected_screen = get_selected_screen();
			background = get_screen_ptr(selected_screen);
			memset(background, 0xff, 304*192);

			render_sprite(0);
			render(background);
			draw_screen();
			redraw = 0;
			print_sprite(0);
		}

		check_events();
		rest(12);

		update_keys();

		if (get_variable(252) == 1)
		{
			int i = (sprites[0].frame & 0x7f) + 1;
			sprites[0].frame = (sprites[0].frame & 0x80) | i;
			if (sprites[0].frame > get_byte(a4))
			{
				sprites[0].frame &= 0x80;
			}

			redraw = 1;
		}

		if (get_variable(252) == -1)
		{
			if (sprites[0].frame > 0)
			{
				int i = (sprites[0].frame & 0x7f) - 1;
				redraw = 1;
				sprites[0].frame = (sprites[0].frame & 0x80) | i;
			}
		}

		if (get_variable(229) == 1)
		{
			redraw = 1;
			sprites[0].index++;
			sprites[0].frame &= 0x80;
		}

		if (get_variable(229) == -1)
		{
			if (sprites[0].index > 0)
			{
				redraw = 1;
				sprites[0].index--;
				sprites[0].frame &= 0x80;
			}
		}

		if (get_variable(250))
		{
			/* flip */
			sprites[0].frame ^= 0x80;
		}

		set_variable(229, 0);
		set_variable(252, 0);
	}
}

static void help()
{
	printf("Heart of The Alien Redux %s\n", VERSION);
	puts("USAGE:");
	#ifdef ENABLE_DEBUG
	puts("\t--debug        turn on debugging");
	#endif
	puts("\t--iso          use iso and mp3s (in current directory)");
	puts("\t--double       double size window (608 x 384)");
	puts("\t--triple       triple size window (912 x 576)");
	puts("\t--scale=[2|3]  rescale using scale2x or scale3x filters");
	puts("\t--fullscreen   start in fullscreen");
	puts("\t--room n       start from a different room");
	puts("\t--sprite-test  run sprite test (use with room)");
	puts("\t--intro-test   play all animations");
	puts("\t--fastest      speed throttle");
	puts("\t--record       record keys");
	puts("\t--replay       replay keys");
	puts("\t--help         this help");
}

static struct option options[] =
{
	{"debug", no_argument, 0, 'd'},
	{"room", required_argument, 0, 'r'},
	{"sprite-test", no_argument, &test_flag, 1},
	{"intro-test", no_argument, &test_flag, 2},
	{"help", no_argument, 0, 'h'},
	{"no-sound", no_argument, 0, 'n'},
	{"fullscreen", no_argument, &fullscreen_flag, 1},
	{"record", no_argument, &record_flag, 1},
	{"replay", no_argument, &replay_flag, 1},
	{"double", no_argument, 0, '2'},
	{"triple", no_argument, 0, '3'},
	{"scale", required_argument, 0, 's'},
	{"iso", no_argument, 0, 'i'},
	{"fastest", no_argument, &fastest_flag, 1},
	{"pandora", no_argument, 0, 'p'},
	{"iso-prefix", required_argument, 0, 'e'},
	{0, no_argument, 0, 0}
};

int main(int argc, char **argv)
{
	int options_index;

	next_script = 0;

#ifdef __SWITCH__
	cls.scale = 3;
	cls.filtered = 1;
	cls.fullscreen = 1;
	cls.use_iso = 1;
#else
	cls.scale = 1;
	cls.filtered = 0;
	cls.fullscreen = 0;
	cls.use_iso = 0;
#endif
	cls.speed_throttle = 0;
	cls.paused = 0;
	cls.nosound = 0;
	cls.pandora = 0;
	cls.iso_prefix = NULL;

	options_index = 0;
	while (1)
	{
		int c = getopt_long(argc, argv, "hdr:23s:", options, &options_index);
		if (c == -1)
		{
			/* no more options */
			break;
		}

		switch(c)
		{
			/* won't do a thing if turned on without ENABLE_DEBUG */
			case 'd':
			debug_flag = 1;
			break;

			case 'r':
			next_script = atoi(optarg);
			break;

			case 'h':
			help();
			return 0;

			case '2':
			cls.scale = 2;
			break;

			case '3':
			cls.scale = 3;
			break;

			case 's':
			cls.scale = atoi(optarg);
			if (cls.scale != 2 && cls.scale != 3)
			{
				panic("invalid scaler (either 2 or 3)");
				return 1;
			}

			cls.filtered = 1;
			break;

			case 'i':
			cls.use_iso = 1;
			break;

			case 'n':
			cls.nosound = 1;
			break;

			case 'p':
			cls.pandora = 1;
			break;

			case 'e':
			cls.iso_prefix = strdup(optarg);
			break;

			case '?':
			/* invalid argument */
			return 1;
		}
	}

	if (replay_flag && record_flag)
	{
		fprintf(stderr, "cant specify both replay and record\n");
		return 1;
	}

	if (replay_flag)
	{
		record_fp = fopen(RECORDED_KEYS_FILENAME, "rb");
	}
	else if (record_flag)
	{
		record_fp = fopen(RECORDED_KEYS_FILENAME, "wb");
	}

	initialize();
	unlink(SWITCH_FILENAME);

	switch(test_flag)
	{
		case 0:
		run();
		break;

		case 1:
		sprite_test();
		break;

		case 2:
		animation_test();
		break;

		default:
		fprintf(stderr, "unknown test_flag %d\n", test_flag);
		return 1;
	}

	if (record_fp != NULL)
	{
		flush_recorded_keys();
		fclose(record_fp);
	}

	return 0;
}
