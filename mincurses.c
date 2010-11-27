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

/* Encapsulate the curses/terminfo calls for presenting a little panel
   display at the bottom of a terminal window. */

#define _GNU_SOURCE
#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64

#ifndef _REENTRANT
# define _REENTRANT
#endif

#include <ncurses.h>
#include <curses.h>
#include <term.h>
#include <term_entry.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
#include <poll.h>

#include "mincurses.h"

#define FIFO_SIZE    160
#define BUFF_SIZE    160

/***************************** INPUT SIDE *******************************/

struct tinfo_fkeys {
  unsigned offset;
  chtype code;
};

#if USE_FKEYSF
#define _nc_tinfo_fkeys _nc_tinfo_fkeysf()
#else
extern const struct tinfo_fkeys _nc_tinfo_fkeys[];
#endif

/* In ncurses, all the keybindings are tied to the SP. Calls into the
   keybinding code all require the SP be set. Low level SP
   management/creation is not exposed, and the high level entry points
   that initialize the SP also drool on the rest of the terminal.  For
   this reason, we duplicate some code. */

typedef struct tries TRIES;

struct tries {
  TRIES    *child;
  TRIES    *sibling;
  unsigned char    ch;
  unsigned short   value;
};

TRIES *keytree=NULL;
int fifo[FIFO_SIZE];
int key_init=0;

/*
 * Returns the keycode associated with the given string.  If none is found,
 * return OK.  If the string is only a prefix to other strings, return ERR.
 * Otherwise, return the keycode's value (neither OK/ERR).
 */

static int find_definition(TRIES * tree, const char *str){
  TRIES *ptr;
  int result = OK;

  if (str != 0 && *str != '\0') {
    for (ptr = tree; ptr != 0; ptr = ptr->sibling) {
      if ((unsigned char)(*str) == ptr->ch) {
        if (str[1] == '\0' && ptr->child != 0) {
          result = ERR;
        } else if ((result = find_definition(ptr->child, str + 1)) == OK) {
          result = ptr->value;
        } else if (str[1] == '\0') {
          result = ERR;
        }
      }
      if (result != OK)
        break;
    }
  }
  return (result);
}

extern int _nc_add_to_try(TRIES ** tree, const char *str, unsigned code);

static void minc_init_keytry(){
  size_t n;
  TERMTYPE *tp=&(cur_term->type);

  for (n = 0; _nc_tinfo_fkeys[n].code; n++) {
    if (_nc_tinfo_fkeys[n].offset < STRCOUNT) {
      _nc_add_to_try(&keytree,tp->Strings[_nc_tinfo_fkeys[n].offset],
                     _nc_tinfo_fkeys[n].code);
    }
  }
#if NCURSES_XNAMES
  for (n = STRCOUNT; n < NUM_STRINGS(tp); ++n) {
    const char *name = ExtStrname(tp, n, strnames);
    char *value = tp->Strings[n];
    if (name && *name == 'k' && value && find_definition(keytree,value) == OK) {
      _nc_add_to_try(&keytree, value, n - STRCOUNT + KEY_MAX);
    }
  }
#endif
  key_init=1;
}

static int head = -1;
static int tail = 0;
static int peek = 0;

#define h_inc() { head == FIFO_SIZE-1 ? head = 0 : head++; if (head == tail) head = -1, tail = 0;}
#define t_inc() { tail == FIFO_SIZE-1 ? tail = 0 : tail++; if (tail == head) tail = -1;}
#define t_dec() { tail == 0 ? tail = FIFO_SIZE-1 : tail--; if (head == tail) fifo_clear();}
#define p_inc() { peek == FIFO_SIZE-1 ? peek = 0 : peek++;}

#define cooked_key_in_fifo()    ((head != -1) && (peek != head))
#define raw_key_in_fifo()       ((head != -1) && (peek != tail))

static inline int fifo_peek(){
   int ch = fifo[peek];
   p_inc();
   return ch;
}

static inline int fifo_pull(){
  int ch;
  ch = fifo[head];

  if (peek == head) {
    h_inc();
    peek = head;
  } else
    h_inc();

  return ch;
}

static inline int fifo_push(int nonblock){
  int n;
  int ch = 0;
  unsigned char c2 = 0;

  if (tail == -1)
    return ERR;

  if(nonblock){
    int ret;
    struct pollfd fds={STDIN_FILENO,POLLIN,0};
    ret=poll(&fds, 1, 0);
    if(!fds.revents&(POLLIN)){
      return ERR;
    }
  }
  n = read(STDIN_FILENO, &c2, 1);
  ch = c2;
  if (n<=0) ch = ERR;
  fifo[tail] = ch;
  if (head == -1)
    head = peek = tail;
  t_inc();
  return ch;
}

static inline void fifo_clear(){
  memset(fifo, 0, sizeof(fifo));
  head = -1;
  tail = peek = 0;
}

int min_getch(int nonblock){
  TRIES *ptr;
  int ch = 0;

  if(!key_init) minc_init_keytry();
  ptr=keytree;

  while (1) {
    if (cooked_key_in_fifo() && fifo[head] >= KEY_MIN) {
      break;
    } else if (!raw_key_in_fifo()) {
      ch = fifo_push(nonblock);
      if (ch == ERR) {
        peek = head;
        return ERR;
      }
    }

    ch = fifo_peek();
    if (ch >= KEY_MIN) {
      peek = head;
      t_dec();
      return ch;
    }

    while ((ptr != NULL) && (ptr->ch != (unsigned char) ch))
      ptr = ptr->sibling;

    if (ptr == NULL) break;
    if (ptr->value != 0) {
      if (peek == tail)
        fifo_clear();
      else
        head = peek;
      return (ptr->value);
    }
    ptr = ptr->child;
  }
  ch = fifo_pull();
  peek = head;
  return ch;
}

/***************************** OUTPUT SIDE ******************************/

TTY orig;
TTY term;

static int outfd;
static int initted=0;
static int cursor_line_offset=0;
static int panel_lines=0;
static char outbuf[BUFF_SIZE];
static int buf_fill=0;

int min_flush(){
  int len = buf_fill;
  char *b = outbuf;
  while(len){
    int ret;
    errno=0;
    ret=write(outfd,b,len);
    if(ret<=0){
      if(ret==0 || errno==EINTR)continue;
      return EOF;
    }else{
      b+=ret;
      len-=ret;
    }
  }
  buf_fill=0;
  return 0;
}

int min_putchar(int c){
  outbuf[buf_fill]=c;
  buf_fill++;
  if(buf_fill==BUFF_SIZE)
    return min_flush();
  return 0;
}

int min_putp(const char *str){
  return tputs(str, 1, min_putchar);
}

int min_write(const char *str,int len){
  int ret=0;
  while(len){
    int bytes = (buf_fill+len>BUFF_SIZE ? BUFF_SIZE-buf_fill : len);
    memcpy(outbuf+buf_fill,str,bytes);
    buf_fill+=bytes;
    if(buf_fill==BUFF_SIZE){
      ret=min_flush();
      if(ret)return ret;
    }
    str+=bytes;
    len-=bytes;
  }
  return 0;
}

int min_putstr(const char *str){
  int len=strlen(str);
  return min_write(str,len);
}

/************** high level operations ***********************/

void min_mvcur(int x, int y){
  int yoff = y - cursor_line_offset;

  while(yoff<0){
    if(cursor_up)min_putp(cursor_up);
    cursor_line_offset--;
    yoff++;
  }
  while(yoff>0){
    if(cursor_down)min_putp(cursor_down);
    cursor_line_offset++;
    yoff--;
  }

  if(column_address)min_putp(tparm(column_address,x));
}

static void setup_term_customize(void){
  if (cur_term != 0) {
    term = cur_term->Nttyb;
#ifdef TERMIOS
    term.c_lflag &= ~ICANON;
    term.c_iflag &= ~ICRNL;
    term.c_lflag &= ~(ECHO | ECHONL);
    term.c_iflag &= ~(ICRNL | INLCR | IGNCR);
    term.c_oflag &= ~(ONLCR);
    term.c_lflag |= ISIG;
    term.c_cc[VMIN] = 1;
    term.c_cc[VTIME] = 0;
#else
    term.sg_flags |= CBREAK;
    term.sg_flags &= ~(ECHO | CRMOD);
#endif
    SET_TTY(outfd,&term);
    cur_term->Nttyb = term;
  }
}

extern void _nc_init_acs(void);
int min_panel_init(int pl){
  int i,ret = OK;
  if(!initted){

    if(isatty(STDOUT_FILENO))
      outfd=STDOUT_FILENO;
    else
      outfd=STDERR_FILENO;

    /* save original terminal setup for purposes of restoring later */
    GET_TTY(outfd,&orig);

    /* low level terminfo setup with defaults and no error return */
    setupterm(0,outfd,0);

    /* terminal settings now in a known state; further configure */
    setup_term_customize();

    /* enable graphics character set */
    //if(ena_acs)min_putp(ena_acs);
    _nc_init_acs();

    if(!cursor_up || !cursor_down || !column_address || pl>lines){
      SET_TTY(outfd,&orig);
      return ERR;
    }

    /* set up keytables */
    if(keypad_xmit)min_putp(tparm(keypad_xmit,1));
    minc_init_keytry();

    panel_lines=pl;
    for(i=0;i<pl-1;i++)
      min_putstr("\r\n");
    cursor_line_offset=pl-1;
    initted=1;
    min_flush();
  }
  return ret;
}

int min_panel_expand(int l,int bottomp){
  int i,ret=0;

  if(panel_lines+l>lines)return 1;

  if(bottomp){
    min_mvcur(0,panel_lines);
    for(i=0;i<l-1;i++)
      ret|=min_putstr("\n\r");
    panel_lines+=l;
    cursor_line_offset=panel_lines-1;
  }else{
    min_mvcur(0,panel_lines+l-1);
    min_mvcur(0,0);
    if(parm_insert_line){
      ret|=min_putp(tparm(parm_insert_line,l));
    }else
      ret=1;
    panel_lines+=l;
    cursor_line_offset=0;
  }
  return ret;
}

int min_panel_contract(int l,int bottomp){
  int ret=0;
  if(l>0){
    if(l>panel_lines)l=panel_lines;
    if(bottomp){
      min_mvcur(0,panel_lines-l);
      if(parm_delete_line){
        ret|=min_putp(tparm(parm_delete_line,l));
      }else
        ret=1;
    }else{
      min_mvcur(0,0);
      if(parm_delete_line){
        ret|=min_putp(tparm(parm_delete_line,l));
      }else
        ret=1;
    }
    panel_lines-=l;
  }
  return ret;
}

void min_panel_remove(){
  if(initted){
    if(parm_delete_line){
      min_mvcur(0,0);
      min_putp(tparm(parm_delete_line,panel_lines));
    }
    min_unset();
    min_showcur();
    min_flush();
    SET_TTY(outfd,&orig);
    initted=0;
  }
}

int min_hidecur(){
  if(cursor_invisible){
    min_putp(cursor_invisible);
    return 0;
  }else
    return 1;
}

int min_showcur(){
  if(cursor_visible){
    min_putp(cursor_visible);
    return 0;
  }else
    return 1;
}

int min_clreol(void){
  if(clr_eol){
    min_putp(clr_eol);
    return 0;
  }else
    return 1;
}

int min_clrbol(void){
  if(clr_bol){
    min_putp(clr_bol);
    return 0;
  }else
    return 1;
}

static int acs=0;
static int fg=-1;
static int bg=-1;
static int bold=0;
static int blink=0;
static int rev=0;
static int ul=0;

static int unset(){
  if(exit_attribute_mode){
    min_putp(exit_attribute_mode);
    return 0;
  }else
    return 1;
}

int min_unset(){
  fg=-1;
  bg=-1;
  bold=0;
  rev=0;
  blink=0;
  acs=0;
  ul=0;
  return unset();
}

static int setfg(int c){
  if(set_a_foreground){
    fg=c;
    min_putp(tparm(set_a_foreground,c));
    return 0;
  }
  return 1;
}

static int setbg(int c){
  if(set_a_background){
    bg=c;
    min_putp(tparm(set_a_background,c));
    return 0;
  }
  return 1;
}

static int setbold(void){
  if(enter_bold_mode){
    min_putp(enter_bold_mode);
    bold=1;
    return 0;
  }
  return 1;
}

static int setblink(void){
  if(enter_blink_mode){
    min_putp(enter_blink_mode);
    blink=1;
    return 0;
  }
  return 1;
}

static int setunderline(void){
  if(enter_underline_mode){
    ul=1;
    min_putp(enter_underline_mode);
    return 0;
  }
  return 1;
}

static int setgfx(void){
  if(enter_alt_charset_mode){
    acs=1;
    min_putp(enter_alt_charset_mode);
    return 0;
  }
  return 1;
}

static int setreverse(void){
  if(enter_reverse_mode){
    min_putp(enter_reverse_mode);
    rev=1;
    return 0;
  }
  return 1;
}

static int reset(){
  int ret=0;
  if(acs)ret|=setgfx();
  if(fg>=0)ret|=setfg(fg);
  if(bg>=0)ret|=setbg(bg);
  if(bold)ret|=setbold();
  if(blink)ret|=setblink();
  if(rev)ret|=setreverse();
  if(ul)ret|=setunderline();
  return ret;
}

int min_fg(int c){
  int ret=0;
  if(c<0){
    if(fg>=0){
      fg=-1;
      ret|=unset();
      ret|=reset();
    }
  }else{
    if(fg!=c)
      ret=setfg(c);
  }
  return ret;
}

int min_bg(int c){
  int ret=0;
  if(c<0){
    if(bg>=0){
      bg=-1;
      ret|=unset();
      ret|=reset();
    }
  }else{
    if(bg!=c)
      ret=setbg(c);
  }
  return ret;
}

int min_color(int f,int b){
  int ret=0;
  fg=-1;
  bg=-1;
  ret|=unset();
  ret|=reset();
  ret|=min_fg(f);
  ret|=min_bg(b);
  return ret;
}

int min_bold(int flag){
  int ret=0;
  if(!flag){
    if(bold){
      bold=0;
      ret|=unset();
      ret|=reset();
    }
  }else{
    if(!bold)
      ret=setbold();
  }
  return ret;
}

int min_blink(int flag){
  int ret=0;
  if(!flag){
    if(blink){
      blink=0;
      ret|=unset();
      ret|=reset();
    }
  }else{
    if(!blink)
      ret=setblink();
  }
  return ret;
}

int min_underline(int flag){
  int ret=0;
  if(!flag){
    if(ul){
      if(exit_underline_mode){
        ul=0;
        min_putp(exit_underline_mode);
      }else{
        ul=0;
        ret|=unset();
        ret|=reset();
      }
    }
  }else{
    if(!ul)
      ret=setunderline();
  }
  return ret;
}

int min_gfx(int flag){
  int ret=0;
  if(!flag){
    if(acs){
      if(exit_alt_charset_mode){
        acs=0;
        min_putp(exit_alt_charset_mode);
      }else{
        acs=0;
        ret|=unset();
        ret|=reset();
      }
    }
  }else{
    if(!acs)
      ret=setgfx();
  }
  return ret;
}

int min_reverse(int flag){
  int ret=0;
  if(!flag){
    if(rev){
      rev=0;
      ret|=unset();
      ret|=reset();
    }
  }else{
    if(!rev)
      ret=setreverse();
  }
  return ret;
}
