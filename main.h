/*
 *
 *  squishyball
 *
 *      Copyright (C) 2010 Xiph.Org
 *
 *  squishyball is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  squishyball is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with rtrecord; see the file COPYING.  If not, write to the
 *  Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *
 */

#ifndef _SB__H_
#define _SB__H_

typedef struct pcm_struct pcm_t;

struct pcm_struct {
  char *path;
  int rate;
  int bits; /* negative indicates IEEE754 float */
  int ch;
  char *matrix;
  unsigned char *data;
  off_t size;
  int dither;
};


extern char *make_time_string(double s,int pad);
extern void panel_init(pcm_t **pcm, int test_files, int test_mode, double start, double end, double size,
                       int flip_mode,int repeat_mode,int trials,char *trial_list);
extern void panel_update_playing(int n);
extern void panel_update_start(double time);
extern void panel_update_current(double time);
extern void panel_update_end(double time);
extern void panel_update_repeat_mode(int mode);
extern void panel_update_flip_mode(int mode);
extern void panel_update_trials(char *trial_list);
extern void panel_update_pause(int flag);
extern void panel_toggle_keymap(void);
#endif
