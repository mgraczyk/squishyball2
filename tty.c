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
#include "main.h"
#include "mincurses.h"

/*

- CASUAL COMPARISON MODE ------------------------------------- 2ch 24bit 44100 -

PLAYING A           00:00:00.00 | 00:00:00.00 | 00:00:00.00          00:00:00.00
OOOOOOOOOOOOOOOOOOOHHHHHHHHHHHHH|HHHHHHHHHHHHHHHHHHHHH00000000000000000000000000
                                ^
+------------------------------------------------- REPEAT AFTER - SILENT FLIP -+
| 6/10 trials: ABABAA                                                          |
+------------------------------------------------------------------------------+

*/

static int p_tm,p_ch,p_b,p_r,p_fm,p_rm,pcm_n,p_tr,p_tn,p_pau;
static double p_st,p_cur,p_end,p_len;
static char p_pl, *p_tl;
static pcm_t **pcm_p;

static char timebuffer[80];
char *make_time_string(double s,int pad){
  long hrs=s/60/60;
  long min=s/60-hrs*60;
  long sec=s-hrs*60*60-min*60;
  long hsec=(s-(int)s)*100;
  if(pad){
    snprintf(timebuffer,80,"%02ld:%02ld:%02ld.%02ld",hrs,min,sec,hsec);
  }else if(hrs>0){
    snprintf(timebuffer,80,"%ld:%02ld:%02ld.%02ld",hrs,min,sec,hsec);
  }else if(min>0){
    snprintf(timebuffer,80,"%ld:%02ld.%02ld",min,sec,hsec);
  }else{
    snprintf(timebuffer,80,"%ld.%02ld",sec,hsec);
  }
  return timebuffer;
}

void fill(char *buf,char c,int cols){
  int i;
  for(i=0;i<cols;i++)
    buf[i]=c;
  buf[i]=0;
}

void print_into(char *buf,int pos, char *s){
  int len = strlen(buf);
  int len2 = strlen(s);
  int i;
  for(i=0; i+pos<len && i<len2; i++)
    buf[i+pos]=s[i];
}

void panel_update_playing(char c){
}

void panel_update_start(double time){

}
void panel_update_current(double time){

}
void panel_update_end(double time){

}

void panel_update_repeat_mode(int mode){

}

void panel_update_flip_mode(int mode){

}

void panel_update_trials(char *trial_list){

}

void panel_update_sample(int n){

}
void panel_update_pause(int flag){

}

static void draw_topbar(void){
  char buf[columns+1];
  char buf2[columns+1];

  fill(buf,ACS_HLINE,columns);
  min_mvcur(0,1);
  min_gfxmode();
  min_putstr(buf);
  min_textmode();

  min_mvcur(1,1);
  switch(p_tm){
  case 0:
    min_putstr(" A/B MODE ");
    break;
  case 1:
    min_putstr(" A/B/X MODE ");
    break;
  case 2:
    min_putstr(" X/X/Y MODE ");
    break;
  case 3:
    min_putstr(" CASUAL COMPARISON MODE ");
    break;
  }

  sprintf(buf2," %dch %dbit %dHz ",p_ch,p_b,p_r);

  min_mvcur(columns-strlen(buf2)-1,1);
  min_putstr(buf2);
}

static void draw_timebar(void){
  int pre = rint(p_st/p_len*columns);
  int post = rint((p_len-p_end)/p_len*columns);
  int cur = rint(p_cur/(p_len-1)*columns);
  char buf[columns+1];
  int i;

  /* first line: spacing */
  min_mvcur(0,2);
  fill(buf,' ',columns);
  min_putstr(buf);

  /* second line: timing */
  min_mvcur(0,3);
  if(p_pau){
    min_blink(1);
    min_putstr("PAUSED   ");
    min_blink(0);
  }else{
    min_putstr("PLAYING ");
    switch(p_tm){
    case 0: /* AB */
    case 1: /* ABX */
      min_putchar(p_pl+'A');
      break;
    case 2: /* XXY*/
    case 3: /* Casual */
      min_putchar(p_pl+'1');
      break;
    }
  }

  for(i=9;i<columns/2-19;i++)
    min_putchar(' ');

  if(p_st<=0.f){
    min_fg(COLOR_CYAN);
    min_putstr("xx:xx:xx.xx");
    min_fg(-1);
  }else{
    char *time=make_time_string(p_st,1);
    min_putstr(time);
  }
  min_putstr(" | ");
  {
    char *time=make_time_string(p_cur,1);
    min_putstr(time);
  }
  min_putstr(" | ");
  if(p_end>=p_len){
    min_fg(COLOR_CYAN);
    min_putstr("xx:xx:xx.xx");
    min_fg(-1);
  }else{
    char *time=make_time_string(p_end,1);
    min_putstr(time);
  }

  for(i=columns/2+20;i<columns-11;i++)
    min_putchar(' ');
  {
    char *time=make_time_string(p_len,1);
    min_putstr(time);
  }

  /* third line: indicator */
  i=0;
  min_mvcur(0,4);
  if(pre>0){
    min_bg(COLOR_CYAN);
    while(i<pre){
      min_putchar(' ');
      i++;
    }
  }
  min_color(COLOR_YELLOW,COLOR_BLACK);
  min_bold(1);
  while(i<cur){
    min_putchar(' ');
    i++;
  }
  min_putchar(ACS_VLINE);
  i++;
  while(i<(columns-post)){
    min_putchar(' ');
    i++;
  }
  min_bg(COLOR_CYAN);
  while(i<columns){
    min_putchar(' ');
    i++;
  }
  min_bg(-1);

  /* fourth line: spacing with indicator */
  min_mvcur(0,5);
  min_fg(COLOR_YELLOW);
  min_bold(1);
  i=0;
  while(i<cur){
    min_putchar(' ');
    i++;
  }
  min_putchar(ACS_TTEE);
  i++;
  while(i<post){
    min_putchar(' ');
    i++;
  }
  min_unset();
}

static void draw_trials_box(void){
  char buf[columns+1];

  /* top line of box */
  fill(buf,ACS_HLINE,columns);
  buf[0]=ACS_ULCORNER;
  buf[columns-1]=ACS_URCORNER;

  switch(p_rm){
  case 0:
    break;
  case 1:
    print_into(buf,columns-31," RESTART AFTER ");
    break;
  case 2:
    print_into(buf,columns-31," RESTART EVERY ");
    break;
  }
  switch(p_fm){
  case 1:
    print_into(buf,columns-12," MARK FLIP ");
    break;
  case 2:
    print_into(buf,columns-12," BEEP FLIP ");
    break;
  case 3:
    print_into(buf,columns-14," SILENT FLIP ");
    break;
  }
  min_mvcur(0,6);
  min_putstr(buf);

  /* trials line */
  min_mvcur(0,7);
  min_putchar(ACS_VLINE);
  {
    int l = strlen(p_tl);
    sprintf(buf," %d/%d trials: ",l,p_tn);
    min_putstr(buf);

    l+=strlen(buf);
    if(l>columns-4){
      min_putstr("...");
      min_putstr(p_tl+l-columns-7);
      min_putchar(' ');
    }else{
      int i;
      min_putstr(p_tl);
      for(i=strlen(buf)+strlen(p_tl);i<columns-2;i++)
        min_putchar(' ');
    }
  }
  min_putchar(ACS_VLINE);

  min_mvcur(0,8);
  fill(buf,ACS_HLINE,columns);
  buf[0]=ACS_LLCORNER;
  buf[columns-1]=ACS_LRCORNER;
  min_putstr(buf);
}

static void draw_samples_box(void){
  char buf[columns+1];
  int i;

  /* top line of box */
  fill(buf,ACS_HLINE,columns);
  buf[0]=ACS_ULCORNER;
  buf[columns-1]=ACS_URCORNER;

  switch(p_rm){
  case 0:
    break;
  case 1:
    print_into(buf,columns-31," RESTART AFTER ");
    break;
  case 2:
    print_into(buf,columns-31," RESTART EVERY ");
    break;
  }
  switch(p_fm){
  case 1:
    print_into(buf,columns-12," MARK FLIP ");
    break;
  case 2:
    print_into(buf,columns-12," BEEP FLIP ");
    break;
  case 3:
    print_into(buf,columns-14," SILENT FLIP ");
    break;
  }
  min_mvcur(0,6);
  min_putstr(buf);

  /* one line per sample, highlight the currently played sample */
  for(i=0;i<pcm_n;i++){
    min_mvcur(0,7+i);
    min_putchar(ACS_VLINE);
    fill(buf,' ',columns-3);
    if(i==p_pl){
      min_putchar('>');
      min_bold(1);
    }else
      min_putchar(' ');

    if(strlen(pcm_p[i]->path)>columns-4)
      print_into(buf,0,pcm_p[i]->path+strlen(pcm_p[i]->path)-columns+4);
    else
      print_into(buf,0,pcm_p[i]->path);
    min_putstr(buf);
    if(i==p_pl)
      min_bold(0);
    min_putchar(ACS_VLINE);
  }
  min_mvcur(0,7+pcm_n);
  fill(buf,ACS_HLINE,columns);
  buf[0]=ACS_LLCORNER;
  buf[columns-1]=ACS_LRCORNER;
  min_putstr(buf);
}

void panel_redraw_full(void){
  draw_topbar();
  draw_timebar();

  if(p_tm==3)
    draw_samples_box();
  else
    draw_trials_box();
  min_flush();
}

void panel_init(pcm_t **pcm, int test_files, int test_mode, double start, double end, double size,
                int flip_mode,int repeat_mode,int trials,char *trial_list){
  if(min_init_panel(test_mode==3 ? test_files+8:9)){
    fprintf(stderr,"Unable to initialize terminal\n");
    exit(101);
  }

  if(columns<70){
    fprintf(stderr,"Squishball requires a >=70 column terminal to run.\n");
    exit(102);
  }

  p_tm=test_mode;
  p_ch=pcm[0]->ch;
  p_b=pcm[0]->bits;
  p_r=pcm[0]->rate;
  p_pl=0;
  p_st=start;
  p_cur=start;
  p_end=end;
  p_len=size;
  p_fm=flip_mode;
  p_rm=repeat_mode;
  p_tr=0;
  p_tn=trials;
  p_tl=strdup(trial_list);
  pcm_n=test_files;
   pcm_p=pcm;
  p_pau=0;

  min_hidecur();
  panel_redraw_full();
}
