/*
 *
 *  mincurses
 *
 *      Copyright (C) 2010 Xiph.Org
 *      Portions Copyright (C) 1998-2009 Free Software Foundation, Inc.
 *
 *  mincurses is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  mincurses is distributed in the hope that it will be useful,
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

#ifndef _MINCURSES_H_
#define _MINCURSES_H_

extern int min_flush();
extern int min_getch(int nonblock);
extern int min_putchar(int c);
extern int min_putp(const char *str);
extern int min_write(const char *str,int len);
extern int min_putstr(const char *str);
extern void min_mvcur(int x, int y);
extern int min_init_panel(int pl);
extern void min_remove_panel(void);
extern int min_hidecur(void);
extern int min_showcur(void);
extern int min_clreol(void);
extern int min_clrbol(void);

extern int min_unset(void);
extern int min_fg(int c);
extern int min_bg(int c);
extern int min_color(int f,int b);
extern int min_bold(int flag);
extern int min_blink(int flag);
extern int min_underline(int flag);
extern int min_reverse(int flag);
extern int min_gfx(int flag);
#endif
