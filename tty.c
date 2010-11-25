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

#define _GNU_SOURCE
#include <ncurses.h>
#include <curses.h>
#include <term.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
#include "mincurses.h"

#if 0
void fill(char *buf,char c,int cols){
  int i;
  for(i=0;i<cols;i++)
    buf[i]=c;
  buf[i]=0;
}

void printover(char *buf,int pos, char *s){
  int len = strlen(buf);
  int len2 = strlen(s);
  int i;
  for(i=0; i+pos<len && i<len2; i++)
    buf[i+pos]=s[i];
}

void printhline(char *s,int textcolor){
  int pos=0,last=0;

  while(s[pos]){
    /* draw line */
    while(s[pos] && s[pos]=='_')pos++;
    if(pos>last){
      if(!min_gfxmode()){
	for(;last<pos;last++)
	  min_putchar(ACS_HLINE);
	min_textmode();
      }else{
	min_write(s+last,pos-last);
      }
    }

    /* draw text */
    while(s[pos] && s[pos]!='_')pos++;
    if(pos>last){
      if(textcolor>=0)
	min_fg(textcolor);
      min_write(s+last,pos-last);
      if(textcolor>=0)
	min_color(-1,-1);
      last = pos;
    }
  }
}
#endif
