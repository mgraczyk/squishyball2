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
#include <limits.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
#include "main.h"
#include "mincurses.h"

static int force=0;
static int p_tm,p_ch,p_b,p_r,p_fm,p_rm,pcm_n,p_tr,p_tmax,p_pau,p_pl,p_tn,p_g;
static double p_st,p_cur,p_end,p_len;
static char p_tl[MAXTRIALS],p_tc[MAXTRIALS];
static pcm_t **pcm_p;

static char timebuffer[80];
char *make_time_string(double is,int pad){
  double s = rint(is*100)/100.+1.e-6;
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

static int timerow;
static int playrow;
static int toprow;
static int boxrow;
static int fliprow;

static int draw_topbar(int row){
  char buf[columns+1];
  int i=0,j;
  toprow=row;

  min_mvcur(0,row);
  min_gfx(1);
  min_fg(COLOR_CYAN);
  min_putchar(ACS_VLINE);
  min_unset();
  i++;

  switch(p_tm){
  case 0:
    min_putstr(" A/B TEST MODE ");
    i+=j=15;
    break;
  case 1:
    i+=j=17;
    min_putstr(" A/B/X TEST MODE ");
    break;
  case 2:
    i+=j=17;
    min_putstr(" X/X/Y TEST MODE ");
    break;
  case 3:
    i+=j=24;
    min_putstr(" CASUAL COMPARISON MODE ");
    break;
  }
  min_gfx(1);
  min_fg(COLOR_CYAN);
  min_putchar(ACS_VLINE);
  min_unset();
  i++;

  sprintf(buf," %dch %dbit %dHz ",p_ch,p_b,p_r);
  for(;i<columns-strlen(buf);i++)
    min_putchar(' ');
  min_putstr(buf);

  min_mvcur(0,row-1);
  min_gfx(1);
  min_fg(COLOR_CYAN);
  min_putchar(ACS_ULCORNER);
  for(i=0;i<j;i++)
    min_putchar(ACS_HLINE);
  min_putchar(ACS_URCORNER);

  min_mvcur(0,row+1);
  min_putchar(ACS_VLINE);
  min_fg(COLOR_CYAN);
  for(i=0;i<j;i++)
    min_putchar(ACS_HLINE);
  min_putchar(ACS_LLCORNER);
  min_unset();
  return 2;
}

static int draw_timebar(int row){
  char buf[columns+1];
  timerow=row;

  fill(buf,ACS_HLINE,columns);
  min_mvcur(0,row);
  min_gfx(1);
  min_fg(COLOR_CYAN);
  min_putstr(buf);
  min_unset();

  min_mvcur(columns-12,row);
  min_putchar(' ');
  {
    char *time=make_time_string(p_len,1);
    min_putstr(time);
  }
  return 1;
}

static int draw_playbar(int row){
  int pre = floor(p_st/p_len*columns);
  int post = columns-floor(p_end/p_len*columns+1.e-6f);
  int i;
  playrow=row;

  i=0;
  min_mvcur(0,row);
  min_bg(COLOR_CYAN);
  while(i<pre){
    min_putchar(' ');
    i++;
  }
  min_bg(COLOR_BLACK);
  while(i<(columns-post)){
    min_putchar(' ');
    i++;
  }
  min_bg(COLOR_CYAN);
  while(i<columns){
    min_putchar(' ');
    i++;
  }
  min_unset();
  return 1;
}

static int draw_trials_box(int row){
  int i;
  char buf[columns+1];
  boxrow=row;

  /* top line of box */
  fill(buf,ACS_HLINE,columns);
  buf[0]=ACS_ULCORNER;
  buf[columns-1]=ACS_URCORNER;
  min_mvcur(0,row);
  min_gfx(1);
  min_fg(COLOR_CYAN);
  min_putstr(buf);
  row++;

  /* trials line[s] */
  for(i=0;i<1+(p_g?1:0);i++){
    min_mvcur(0,row);
    min_putchar(ACS_VLINE);
    min_mvcur(columns-1,row);
    min_putchar(ACS_VLINE);
    row++;
  }

  fliprow=row;
  min_mvcur(0,row);
  fill(buf,ACS_HLINE,columns);
  buf[0]=ACS_LLCORNER;
  buf[columns-1]=ACS_LRCORNER;
  min_putstr(buf);
  min_unset();
  return (p_g?1:0)+3;
}

static int draw_samples_box(int row){
  char buf[columns+1];
  int i;
  boxrow=row;
  fliprow=row+pcm_n+1;

  /* top line of box */
  fill(buf,ACS_HLINE,columns);
  buf[0]=ACS_ULCORNER;
  buf[columns-1]=ACS_URCORNER;
  min_mvcur(0,row);
  min_gfx(1);
  min_fg(COLOR_CYAN);
  min_putstr(buf);

  /* one line per sample, highlight the currently played sample */
  for(i=0;i<pcm_n;i++){
    min_mvcur(0,row+i+1);
    min_putchar(ACS_VLINE);
    min_unset();
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
    min_gfx(1);
    min_fg(COLOR_CYAN);
    min_putchar(ACS_VLINE);
  }
  min_mvcur(0,row+pcm_n+1);
  fill(buf,ACS_HLINE,columns);
  buf[0]=ACS_LLCORNER;
  buf[columns-1]=ACS_LRCORNER;
  min_putstr(buf);
  min_unset();
  return pcm_n+2;
}

void panel_redraw_full(void){
  int i=2;

  if(p_tm==3){
    i+=draw_samples_box(i);
  }else{
    i+=draw_trials_box(i);
  }
  i+=draw_playbar(i);
  i+=draw_timebar(i);
  draw_topbar(1);
  force=1;
  panel_update_pause(p_pau);
  panel_update_playing(p_pl);
  panel_update_start(p_st);
  panel_update_current(p_cur);
  panel_update_end(p_end);
  panel_update_repeat_mode(p_rm);
  panel_update_flip_mode(p_fm);
  if(p_tm!=3)
    panel_update_trials(p_tl,p_tc,p_tn);
  force=0;
  min_flush();
}

void panel_init(pcm_t **pcm, int test_files, int test_mode, double start, double end, double size,
                int flip_mode,int repeat_mode,int trials,int gabba){

  if(min_panel_init((test_mode==3 ? test_files+6:7) + (gabba ? 1:0))){
    fprintf(stderr,"Unable to initialize terminal (possibly insufficient lines)\n");
    exit(101);
  }

  if(columns<70){
    fprintf(stderr,"Squisyhball requires a >=70 column terminal to run.\n");
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
  p_tmax=trials;
  p_tn=0;
  pcm_n=test_files;
  pcm_p=pcm;
  p_pau=0;
  p_g=gabba;

  min_hidecur();
  panel_redraw_full();
}

void panel_update_start(double time){
  if(force || p_st!=time){
    p_st=time;
    min_mvcur(columns/2-21,timerow);
    min_putchar(' ');
    if(p_st<=0.f){
      min_fg(COLOR_CYAN);
      min_putstr("xx:xx:xx.xx");
      min_fg(-1);
    }else{
      char *time=make_time_string(p_st,1);
      min_putstr(time);
    }
    min_putchar(' ');
    draw_playbar(playrow);
    {
      int temp=force;
      force=1;
      panel_update_current(p_cur);
      force=temp;
    }
  }
}

static int was=-1;
void panel_update_current(double time){
  int now = floor(time/p_len*columns);
  if(now>=columns)now=columns-1;
  if(force || p_cur!=time){

    p_cur=time;
    min_mvcur(columns/2-7,timerow);
    min_putchar(' ');
    {
      char *time=make_time_string(p_cur,1);
      min_putstr(time);
    }
    min_putchar(' ');

    if(was!=now || force){
      int pre = floor(p_st/p_len*columns);
      int post = columns-floor(p_end/p_len*columns+1.e-6f);

      min_bold(1);
      min_gfx(1);

      if(was>=0){
        min_mvcur(was,playrow);
        if(was<pre || (columns-was)<post){
          min_color(COLOR_YELLOW,COLOR_CYAN);
        }else{
          min_color(COLOR_YELLOW,COLOR_BLACK);
        }
        min_putchar(' ');
      }
      was=now;

      min_mvcur(now,playrow);
      if(now<pre || (columns-now)<post){
        min_bg(COLOR_CYAN);
      }else{
        min_bg(COLOR_BLACK);
      }
      min_putchar(ACS_VLINE);
      min_unset();
    }
    min_flush();
  }
}

void panel_update_end(double time){
  if(force || p_end!=time){
    p_end=time;
    min_mvcur(columns/2+7,timerow);
    min_putchar(' ');
    if(p_end+1.e-6>=p_len){
      min_fg(COLOR_CYAN);
      min_putstr("xx:xx:xx.xx");
      min_fg(-1);
    }else{
      char *time=make_time_string(p_end,1);
      min_putstr(time);
    }
    min_putchar(' ');
    draw_playbar(playrow);
    {
      int temp=force;
      force=1;
      panel_update_current(p_cur);
      force=temp;
    }
  }
}

void panel_update_repeat_mode(int mode){
  if(p_rm!=mode){
    int i;
    min_mvcur(columns-30,fliprow);
    p_rm=mode;
    switch(p_rm){
    case 0:
      min_fg(COLOR_CYAN);
      min_gfx(1);
      for(i=0;i<15;i++)
        min_putchar(ACS_HLINE);
      min_unset();
      break;
    case 1:
      min_putstr(" RESTART AFTER ");
      break;
    case 2:
      min_putstr(" RESTART EVERY ");
      break;
    }
  }
}

void panel_update_flip_mode(int mode){
  if(force || p_fm!=mode){
    min_mvcur(columns-14,fliprow);
    min_fg(COLOR_CYAN);
    min_gfx(1);
    min_putchar(ACS_HLINE);
    min_putchar(ACS_HLINE);
    min_unset();

    p_fm=mode;
    switch(p_fm){
    case 1:
      min_mvcur(columns-12,fliprow);
      min_putstr(" MARK FLIP ");
      break;
    case 2:
      min_mvcur(columns-12,fliprow);
      min_putstr(" BEEP FLIP ");
      break;
    case 3:
      min_mvcur(columns-14,fliprow);
      min_putstr(" SILENT FLIP ");
      break;
    }
  }
}

char *dottrim(char *in, int l){
  int m=strlen(in);
  if(m>l){
    if(l<1)return "";
    if(l<2)return ".";
    if(l<3)return "..";
    if(l<4)return "...";
    in+=m-l;
    in[0]=in[1]=in[2]='.';
  }
  return in;
}

void panel_update_trials(char *choices, char *correct, int n){
  if(force || n!=p_tn || memcmp(p_tl,choices,n)){
    char buf[columns+1];
    int i;
    p_tn=n;

    min_mvcur(1,boxrow+1);
    sprintf(buf," %d/%d trials: ",p_tn,p_tmax);
    min_putstr(buf);

    memcpy(p_tl,choices,n);
    memcpy(p_tc,correct,n);

    if(n>columns-strlen(buf)-3){
      min_fg(COLOR_CYAN);
      min_putstr("...");
      i=n-columns+strlen(buf)+6;
    }else{
      i=0;
    }
    for(;i<n;i++){
      if(p_g){
        if(correct[i]==0){
          if(p_tm==0){
            min_fg(COLOR_MAGENTA);
          }else{
            min_fg(COLOR_RED);
          }
        }else{
          if(p_tm==0){
            min_fg(COLOR_CYAN);
          }else{
            min_fg(COLOR_GREEN);
          }
        }
      }

      if(p_tm<2){
        min_putchar(p_tl[i]+'A');
      }else{
        min_putchar(p_tl[i]+'1');
      }
    }
    min_unset();
    i+=strlen(buf);
    for(;i<columns-2;i++)
      min_putchar(' ');

    if(p_g){
      int count=0;
      for(i=0;i<n;i++)
        count+=correct[i];

      min_mvcur(1,boxrow+2);
      if(p_tm==0){
        /* A/B */
        int col = columns - 4, Ac, Bc;
        char bufA[PATH_MAX];
        char bufB[PATH_MAX];
        char bufAn[10];
        char bufBn[10];
        char *Ap=bufA,*Bp=bufB;
        double p=compute_pdual(count,n);
        snprintf(bufA,PATH_MAX,"%s: ",pcm_p[0]->path);
        snprintf(bufB,PATH_MAX,"%s: ",pcm_p[1]->path);
        snprintf(bufAn,10,"%d ",n-count);
        snprintf(bufBn,10,"%d ",count);
        if(n>1)
          sprintf(buf," p': %.2g ",(float)p);
        else
          sprintf(buf," p': --- ");
        col-=strlen(buf);
        Ac=strlen(bufA)+strlen(bufAn);
        Bc=strlen(bufB)+strlen(bufBn);

        if(Ac+Bc > col){
          if(Ac<=col/2){
            Bp = dottrim(bufB, col-Ac-strlen(bufBn));
          }else if(Bc<=col/2){
            Ap = dottrim(bufA, col-Bc-strlen(bufAn));
          }else{
            Ap = dottrim(bufA, col/2-strlen(bufAn));
            Bp = dottrim(bufB, col-col/2-strlen(bufBn));
          }
        }

        min_putchar(' ');
        min_putstr(Ap);
        min_fg(COLOR_MAGENTA);
        min_putstr(bufAn);
        min_unset();
        min_putchar(' ');
        min_putstr(Bp);
        min_fg(COLOR_CYAN);
        min_putstr(bufBn);
        min_unset();
        min_putstr(" p': ");
        if(p<.01)
          min_fg(COLOR_GREEN);
        min_putstr(buf+5);
     }else{
        /* A/B/X, X/X/Y */
        double p=compute_psingle(count,n);
        sprintf(buf," Score: %d/%d  p': ",count,n);
        min_putstr(buf);
        if(n){
          sprintf(buf,"%.2g   ",(float)p);
          if(p<.01)
            min_fg(COLOR_GREEN);
        }else{
          min_fg(COLOR_CYAN);
          sprintf(buf,"---    ");
        }
        min_putstr(buf);
      }
    }
    min_unset();
  }
}

void panel_update_playing(int n){
  if(force || n!=p_pl){
    if(p_tm==3){
      min_mvcur(1,boxrow+1+p_pl);
      min_putchar(' ');
      if(strlen(pcm_p[p_pl]->path)>columns-4)
        min_putstr(pcm_p[p_pl]->path+strlen(pcm_p[p_pl]->path)-columns+4);
      else
        min_putstr(pcm_p[p_pl]->path);

      min_mvcur(1,boxrow+1+n);
      min_putchar('>');
      min_bold(1);
      if(strlen(pcm_p[n]->path)>columns-4)
        min_putstr(pcm_p[n]->path+strlen(pcm_p[n]->path)-columns+4);
      else
        min_putstr(pcm_p[n]->path);
      min_unset();
    }

    p_pl=n;
    if(!p_pau){
      min_mvcur(8,timerow);
      switch(p_tm){
      case 0: /* AB */
      case 1: /* ABX */
        switch(p_pl){
        case 0:
          min_putchar('A');
          break;
        case 1:
          min_putchar('B');
          break;
        case 2:
          min_putchar('X');
          break;
        }
        break;
      case 2: /* XXY*/
      case 3: /* Casual */
        min_putchar(p_pl+'1');
        break;
      }
    }
  }
}

void panel_update_pause(int flag){
  if(flag!=p_pau || force){
    p_pau=flag;
    min_mvcur(0,timerow);
    if(p_pau){
      min_blink(1);
      min_putstr("PAUSED ");
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
      min_putchar(' ');
    }

    min_gfx(1);
    min_fg(COLOR_CYAN);
    min_putchar(ACS_HLINE);
    min_putchar(ACS_HLINE);
    min_putchar(ACS_HLINE);
    min_unset();
  }
}

static void min_putstrb(char *s){
  min_bold(1);
  min_putstr(s);
  min_bold(0);
}

static int p_keymap=0;
void panel_toggle_keymap(){
  int l=8;
  int o=1;
  int x=(columns-70)/2;
  if(!p_keymap){
    if(min_panel_expand(l,0))return;
    p_keymap = !p_keymap;
    timerow+=l;
    playrow+=l;
    toprow+=l;
    boxrow+=l;
    fliprow+=l;
    min_fg(COLOR_CYAN);
    min_mvcur(x,o++);
    min_putstrb(" a b x 1 2 3... ");
    min_putstr (": Flip sample    ");
    min_putstrb("      A B ! @ # ");
    min_putstr (": Choose sample  ");
    min_mvcur(x,o++);
    min_putstrb("        <enter> ");
    min_putstr (": Choose current ");
    if(!p_g){
      min_putstrb("      <ins/del> ");
      min_putstr (": Undo/redo trial");
    }
    min_mvcur(x,o++);
    min_putstrb("   <left/right> ");
    min_putstr (": Seek           ");
    min_putstrb("      <up/down> ");
    min_putstr (": Flip (casual)  ");
    min_mvcur(x,o++);
    min_putstrb("        <space> ");
    min_putstr (": Pause/resume   ");
    min_putstrb("      <backspc> ");
    min_putstr (": Seek to start  ");
    min_mvcur(x,o++);
    min_putstrb("        s S e E ");
    min_putstr (": set start/end  ");
    min_putstrb("            f r ");
    min_putstr (": Toggle modes   ");
    min_mvcur(x,o++);
    min_putstrb("              ? ");
    min_putstr (": Toggle keymap  ");
    min_putstrb("      Control-c ");
    min_putstr (": Quit           ");
    min_unset();
  }else{
    p_keymap = !p_keymap;
    min_panel_contract(l,0);
    timerow-=l;
    playrow-=l;
    toprow-=l;
    boxrow-=l;
    fliprow-=l;
  }
}
