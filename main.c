/*
 *
 *  squishyball
 *
 *      Copyright (C) 2010-2012 Xiph.Org
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
#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>
#include <vorbis/vorbisfile.h>
#include <ao/ao.h>
#include <FLAC/stream_decoder.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <ncurses.h>
#include <poll.h>
#ifdef __APPLE__
#include <sys/select.h>
#endif
#include "mincurses.h"
#include "main.h"

#define MAXFILES 10
int sb_verbose=0;

char *short_options="abcd:De:hn:rRs:tvVxBMSg";

struct option long_options[] = {
  {"ab",no_argument,0,'a'},
  {"abx",no_argument,0,'b'},
  {"beep-flip",no_argument,0,'B'},
  {"casual",no_argument,0,'c'},
  {"device",required_argument,0,'d'},
  {"force-dither",no_argument,0,'D'},
  {"end-time",no_argument,0,'e'},
  {"gabbagabbahey",no_argument,0,'g'},
  {"score-display",no_argument,0,'g'},
  {"help",no_argument,0,'h'},
  {"mark-flip",no_argument,0,'M'},
  {"trials",required_argument,0,'n'},
  {"restart-after",no_argument,0,'r'},
  {"restart-every",no_argument,0,'R'},
  {"start-time",required_argument,0,'s'},
  {"seamless-flip",no_argument,0,'S'},
  {"force-truncate",no_argument,0,'t'},
  {"verbose",no_argument,0,'v'},
  {"version",no_argument,0,'V'},
  {"xxy",no_argument,0,'x'},
  {0,0,0,0}
};

void usage(FILE *out){
  fprintf(out,
          "\nXiph Squishyball %s\n"
          "perform sample comparison testing on the command line\n\n"
          "USAGE:\n"
          "  squishyball [options] fileA [fileB [fileN...]] [> results.txt]\n\n"
          "OPTIONS:\n"
          "  -a --ab                : Perform A/B test\n"
          "  -b --abx               : Perform A/B/X test\n"
          "  -B --beep-flip         : Mark transitions between samples with\n"
          "                           a short beep\n"
          "  -c --casual            : casual mode; load up to ten\n"
          "                           samples for non-randomized\n"
          "                           comparison without trials (default).\n"
          "  -d --device <N|dev>    : If a number, output to Nth\n"
          "                           sound device.  If a device name,\n"
          "                           use output driver/device matching\n"
          "                           that device name.\n"
          "  -D --force-dither      : Always use dither when converting\n"
          "                           to 16-bit for playback on output\n"
          "                           devices that do not support 24-bit\n"
          "                           playback. Currently only affects\n"
          "                           Vorbis and Opus playback; all other\n"
          "                           files are dithered by default during\n"
          "                           down-conversion.\n"
          "  -e --end-time <time>   : Set sample end time for playback\n"
          "  -g --gabbagabbahey     : Display a running trials score along\n"
          "                           with indicating if each trial choice\n"
          "                           was correct or incorrect.  Disables\n"
          "                           undo/redo.\n"
          "  -h --help              : Print this usage information.\n"
          "  -M --mark-flip         : Mark transitions between samples with\n"
          "                           a short period of silence\n"
          "  -n --trials <n>        : Set desired number of trials\n"
          "                           (default: 20)\n"
          "  -r --restart-after     : Restart playback from sample start\n"
          "                           after every trial.\n"
          "  -R --restart-every     : Restart playback from sample start\n"
          "                           after every 'flip' as well as after\n"
          "                           every trial.\n"
          "  -s --start-time <time> : Set start time within sample for\n"
          "                           playback\n"
          "  -S --seamless-flip     : Do not mark transitions between samples;\n"
          "                           flip with a seamless crossfade (default)\n"
          "  -t --force-truncate    : Always truncate (never dither) when\n"
          "                           down-converting samples to 16-bit for\n"
          "                           playback.\n"
          "  -v --verbose           : Produce more progress information.\n"
          "  -V --version           : Print version and exit.\n"
          "  -x --xxy               : Perform X/X/Y (triangle) test.\n"
          "\n"
          "INTERACTION:\n"
          "    a b x    : Switch playback between A, B [and X] samples.\n"
          "     A B     : Choose A or B sample for A/B[/X] trial result.\n"
          "   1 2 3...  : Switch between first, second, etc samples.\n"
          "    ! @ #    : Choose sample 1, 2, or 3 for X/X/Y trial result.\n"
          " <del> <ins> : Undo/redo last trial result selection.\n"
          "   <enter>   : Choose current sample for this trial\n"
          "    <- ->    : Seek back/forward two seconds, +shift for 10 seconds\n"
          "  <up/down>  : Select sample from list (casual mode)\n"
          "   <space>   : Pause/resume playback\n"
          "  <backspc>  : Reset playback to start point\n"
          "      e      : set end playback point to current playback time.\n"
          "      E      : reset end playback time to end of sample\n"
          "      f      : Toggle through beep-flip/mark-flip/seamless-flip modes.\n"
          "      r      : Toggle through restart-after/restart-every/no-restart.\n"
          "      s      : set start playback point to current playback time.\n"
          "      S      : reset start playback time to 0:00:00.00\n"
          "      ?      : Print this keymap\n"
          "     ^-c     : Quit\n"
          "\n"
          "SUPPORTED FILE TYPES:\n"
          "  WAV and WAVEX    : 8-, 16-, 24-bit linear integer PCM (format 1)\n"
          "                     32 bit float (format 3)\n"
          "  AIFF and AIFC    : 8-, 16-, 24-bit linear integer PCM or\n"
          "                     32-bit floating point PCM\n"
          "  FLAC and OggFLAC : 16- and 24-bit\n"
          "  SW               : mono signed 16-bit little endian raw\n"
          "  OggVorbis        : all Vorbis I files\n"
          "  OggOpus          : all Opus files\n"
          "\n"
          ,VERSION);
}

static int parse_time(char *s,double *t){
  double      secf;
  long        secl;
  const char *pos;
  char       *end;
  int         err;
  err=0;
  secl=0;
  pos=strchr(optarg,':');
  if(pos!=NULL){
    char *pos2;
    secl=strtol(optarg,&end,10)*60;
    err|=pos!=end;
    pos2=strchr(++pos,':');
    if(pos2!=NULL){
      secl=(secl+strtol(pos,&end,10))*60;
      err|=pos2!=end;
      pos=pos2+1;
    }
  }
  else pos=optarg;
  secf=strtod(pos,&end);
  if(err||*end!='\0')return -1;

  *t = secl+secf;
  return 0;
}

int randrange(int range){
  return (int)floor(rand()/(RAND_MAX+1.0)*range);
}

void randomize_samples(int *r,int *cchoice, int test_mode){
  switch(test_mode){
  case 1:
    r[0] = 0;
    r[1] = 1;
    r[2] = randrange(2);
    *cchoice = (r[1]==r[2] ? 1 : 0);
    break;
  case 0:
    r[0] = randrange(2);
    r[1] = 1-r[0];
    *cchoice = 1;
    break;
  case 2:
    r[0] = r[1] = r[2] = 0;
    r[randrange(3)] = 1;
    if (randrange(2)){
      r[0] = 1-r[0];
      r[1] = 1-r[1];
      r[2] = 1-r[2];
    }
    *cchoice = (r[0]==r[1] ? 2 : (r[1]==r[2] ? 0 : 1));
    break;
  }
}

double factorial(int x){
  double f = 1.;
  while(x>1){
    f*=x;
    x--;
  }
  return f;
}

double compute_psingle(int correct, int tests){
  int i;
  // 0.5^20*(20!/(17!*3!))
  double p=0;
  for(i=correct;i<=tests;i++)
    p += pow(.5,tests) * (factorial(tests) / (factorial(tests-i)*factorial(i)));
  return p;
}

double compute_pdual(int score, int tests){
  int i;
  double p=0;
  if(tests-score>score)
    score=tests-score;
  for(i=score;i<=tests;i++){
    double lp = pow(.5,tests) * (factorial(tests) / (factorial(tests-i)*factorial(i)));
    if(tests-i != i) lp*=2;
    p+=lp;
  }
  return p;
}

typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t main_cond;
  pthread_cond_t play_cond;
  pthread_cond_t key_cond;
  int exiting;

  ao_device *adev;
  unsigned char *fragment;
  int fragment_size;
  int key_waiting;
  int exit_fd;
} threadstate_t;

/* playback is a degenerate thread that simply allows audio output
   without blocking */
void *playback_thread(void *arg){
  threadstate_t *s = (threadstate_t *)arg;
  ao_device *adev = s->adev;

  pthread_mutex_lock(&s->mutex);
  while(1){
    if(s->exiting)break;
    if(s->fragment_size){
      int ret;
      unsigned char *data=s->fragment;
      int n=s->fragment_size;
      pthread_mutex_unlock(&s->mutex);
      ret=ao_play(adev, (void *)data, n);
      pthread_mutex_lock(&s->mutex);
      if(ret==0)s->exiting=1;
      s->fragment_size=0;
      s->fragment=0;
      pthread_cond_signal(&s->main_cond);
      if(s->exiting)break;
    }

    pthread_cond_wait(&s->play_cond,&s->mutex);
  }
  pthread_mutex_unlock(&s->mutex);
  ao_close(adev);
  ao_shutdown();
  return NULL;
}

/* keyboard is a degenerate thread that wakes the main thread when
   keyboard input [may] be available */
void *key_thread(void *arg){
  threadstate_t *s = (threadstate_t *)arg;

  pthread_mutex_lock(&s->mutex);
  while(1){
    int ret=ERR;
    if(s->exiting) break;
    if(s->key_waiting==0){
      pthread_mutex_unlock(&s->mutex);

      {
#ifdef __APPLE__
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        FD_SET(s->exit_fd, &fds);
        ret=select(s->exit_fd+1,&fds,NULL,NULL,NULL);
        if(FD_ISSET(s->exit_fd,&fds))
          break;
        if(FD_ISSET(STDIN_FILENO,&fds))
          ret=min_getch(1);
#else
        struct pollfd fds[2]={
          {STDIN_FILENO,POLLIN,0},
          {s->exit_fd,POLLIN,0}
        };

        ret=poll(fds, 2, -1);
        if(fds[1].revents&(POLLIN))
          break;
        if(fds[0].revents&(POLLIN))
          ret=min_getch(1);
#endif
      }
      pthread_mutex_lock(&s->mutex);
    }

    if(s->exiting)break;

    if(ret!=ERR){
      s->key_waiting=ret;
      pthread_cond_signal(&s->main_cond);
      pthread_cond_wait(&s->key_cond,&s->mutex);
    }
  }
  pthread_mutex_unlock(&s->mutex);
  return NULL;
}

int main(int argc, char **argv){
  float *fadewindow1;
  float *fadewindow2;
  float *fadewindow3;
  float *beep1;
  float *beep2;
  int fragsamples;
  int fragsize;
  unsigned char *fragmentA;
  unsigned char *fragmentB;
  pthread_t playback_handle;
  pthread_t fd_handle;
  threadstate_t state;
  int exit_fds[2];
  int c,long_option_index;
  pcm_t *pcm[MAXFILES];
  int test_mode=3;
  int test_files;
  char *device=NULL;
  int force_dither=0;
  int force_truncate=0;
  int restart_mode=0;
  int beep_mode=3;
  int tests=20;
  double start=0;
  double end=-1;
  int outbits=0;
  ao_device *adev=NULL;
  int randomize[MAXFILES];
  int i;

  int  cchoice=-1;
  char choice_list[MAXTRIALS];
  char sample_list[MAXTRIALS];
  int  tests_cursor=0;
  int  tests_total=0;
  int  running_score=0;

  int flips[3]={0,0,0}; /* count for each flip mode */
  int undos=0;
  int seeks=0;
  size_t fragments_played=0;

  /* parse options */

  while((c=getopt_long(argc,argv,short_options,long_options,&long_option_index))!=EOF){
    switch(c){
    case 'h':
      usage(stdout);
      return 0;
    case 'a':
      test_mode=0;
      break;
    case 'b':
      test_mode=1;
      break;
    case 'c':
      test_mode=3;
      break;
    case 'x':
      test_mode=2;
      break;
    case 'M':
      beep_mode=1;
      break;
    case 'B':
      beep_mode=2;
      break;
    case 'S':
      beep_mode=3;
      break;
    case 'd':
      device=strdup(optarg);
      break;
    case 'D':
      force_dither=1;
      force_truncate=0;
      break;
    case 't':
      force_dither=0;
      force_truncate=1;
      break;
    case 'n':
      tests=atoi(optarg);
      if(tests<1){
        fprintf(stderr,"Error parsing argument to -n\n");
        exit(1);
      }
      if(tests>MAXTRIALS){
        fprintf(stderr,"Error parsing argument to -n (max %d trials)\n",MAXTRIALS);
        exit(1);
      }
      break;
    case 'e':
      parse_time(optarg,&end);
      if(end<=0){
        fprintf(stderr,"Error parsing argument to -e\n");
        exit(1);
      }
      break;
    case 's':
      parse_time(optarg,&start);
      if(start<=0){
        fprintf(stderr,"Error parsing argument to -s\n");
        exit(1);
      }
      break;
    case 'r':
      restart_mode=1;
      break;
    case 'R':
      restart_mode=2;
      break;
    case 'v':
      sb_verbose=1;
      break;
    case 'V':
      fprintf(stdout,"Xiph.Org Squishyball %s\n",VERSION);
      exit(0);
    case 'g':
      running_score=1;
      break;
    default:
      usage(stderr);
      exit(1);
    }
  }

  if(running_score && test_mode==3){
    if(sb_verbose)
      fprintf(stderr,"-g is meaningless in casual comparison mode.\n");
    running_score=0;
  }

  /* Verify stdin is a tty! */

  test_files=argc-optind;
  if(test_mode==3){
    if(test_files<1 || test_files>MAXFILES){
      usage(stderr);
      exit(1);
    }
  }else{
    if(test_files!=2){
      usage(stderr);
      exit(1);
    }
  }

  if(pipe(exit_fds)){
    fprintf(stderr,"Failed to create interthread pipe.\n");
    exit(11);
  }

  outbits=16;
  for(i=0;i<test_files;i++){
    pcm[i]=load_audio_file(argv[optind+i]);
    if(!pcm[i])exit(2);
    check_warn_clipping(pcm[i]);

    if(!pcm[i]->dither && force_dither)pcm[i]->dither=1;
    if(pcm[i]->bits!=16 && force_truncate)pcm[i]->dither=0;

    /* Are all samples the same rate?  If not, bail. */
    if(pcm[0]->rate != pcm[i]->rate){
      fprintf(stderr,"Input sample rates do not match!\n"
              "\t%s: %dHz\n"
              "\t%s: %dHz\n"
              "Aborting\n",pcm[0]->name,pcm[0]->rate,pcm[i]->name,pcm[i]->rate);
      exit(3);
    }

    /* Are all samples the same number of channels?  If not, bail. */
    if(pcm[0]->ch != pcm[i]->ch){
      fprintf(stderr,"Input channel counts do not match!\n"
              "\t%s: %d channels\n"
              "\t%s: %d channels\n"
              "Aborting\n",pcm[0]->name,pcm[0]->ch,pcm[i]->name,pcm[i]->ch);
      exit(3);
    }

    if(abs(pcm[i]->bits)>outbits)outbits=abs(pcm[i]->bits);
  }

  /* before proceeding, make sure we can open up playback for the
     requested number of channels and max bit depth; if not, we may
     need to downconvert. */
  if(outbits==32)outbits=24;
  ao_initialize();
  if((adev=setup_playback(pcm[0]->rate,pcm[0]->ch,outbits,pcm[0]->matrix,device))==NULL){
    /* If opening playback failed for 24-bit, try for 16 */
    if(outbits>16){
      if((adev=setup_playback(pcm[0]->rate,pcm[0]->ch,16,pcm[0]->matrix,device))==NULL){
        fprintf(stderr,"Unable to open audio device for playback.\n");
        exit(4);
      }else{
        if(sb_verbose)
          fprintf(stderr,"24-bit playback unavailable; down-converting to 16-bit\n");
        outbits=16;
      }
    }else{
      fprintf(stderr,"Unable to open audio device for playback.\n");
      exit(4);
    }
  }

  /* reconcile sample depths */
  for(i=0;i<test_files;i++){
    if(outbits==16){
      convert_to_16(pcm[i]);
    }else{
      convert_to_24(pcm[i]);
    }
  }

  /* permute/reconcile the matrices before playback begins */
  /* Invariant: all loaded files have a channel map */
  for(i=1;i<test_files;i++)
    if(strcmp(pcm[0]->matrix,pcm[i]->matrix))
      reconcile_channel_maps(pcm[0],pcm[i]);

  /* Are the samples the same length?  If not, warn and choose the shortest. */
  {
    off_t n=pcm[0]->size;
    int flag=0;
    for(i=1;i<test_files;i++){
      if(pcm[i]->size!=n)flag=1;
      if(pcm[i]->size<n)n=pcm[i]->size;
    }

    if(flag){
      if(sb_verbose)
        fprintf(stderr,"Input sample lengths do not match!\n");

      for(i=0;i<test_files;i++){
        if(sb_verbose)
        fprintf(stderr,"\t%s: %s\n",pcm[i]->name,
                make_time_string((double)pcm[i]->size/pcm[i]->ch/((pcm[i]->bits+7)/8)/pcm[i]->rate,0));
        pcm[i]->size=n;
      }
      if(sb_verbose)
        fprintf(stderr,"Using the shortest sample for playback length...\n");
    }
  }

  /* set up various transition windows/beeps */
  fragsamples=setup_windows(pcm,test_files,
                            &fadewindow1,&fadewindow2,&fadewindow3,&beep1,&beep2);


  /* casual mode is not randomized */
  for(i=0;i<MAXFILES;i++)
    randomize[i]=i;

  /* randomize samples for first trial */
  srandom(time(NULL)+getpid());
  randomize_samples(randomize,&cchoice,test_mode);

  {
    int current_sample=randomize[0];
    int current_choice=0;
    int flip_to=0;
    int do_flip=0;
    int do_select=0;
    int do_pause=0;
    int do_seek=0;
    int loop=0;
    off_t seek_to=0;
    int bps=(pcm[0]->bits+7)/8;
    int ch=pcm[0]->ch;
    int bpf=ch*bps;
    int rate=pcm[0]->rate;
    int size=pcm[0]->size;
    off_t start_pos=rint(start*rate*bpf);
    off_t end_pos=(end>0?rint(end*rate*bpf):size);
    off_t current_pos;
    int paused=0;
    double base = 1.f/(rate*bpf);
    double len = pcm[0]->size*base;
    fragsize=fragsamples*bpf;

    /* guard start/end params */
    if(end>=0 && start>=end){
      fprintf(stderr,"Specified start and end times must not overlap.\n");
      exit(1);
    }

    if(end>len){
      fprintf(stderr,"Specified end time beyond end of playback.\n");
      exit(1);
    }

    if(start>=len){
      fprintf(stderr,"Specified start time beyond end of playback.\n");
      exit(1);
    }

    /* set up terminal */
    atexit(min_panel_remove);
    panel_init(pcm, test_files, test_mode, start, end>0 ? end : len, len,
               beep_mode, restart_mode, tests, running_score);

    /* set up shared state */
    memset(&state,0,sizeof(state));
    pthread_mutex_init(&state.mutex,NULL);
    pthread_cond_init(&state.main_cond,NULL);
    pthread_cond_init(&state.play_cond,NULL);
    pthread_cond_init(&state.key_cond,NULL);
    state.adev=adev;
    state.exit_fd=exit_fds[0];

    fragmentA=calloc(fragsize,1);
    fragmentB=calloc(fragsize,1);
    if(!fragmentA || !fragmentB){
      fprintf(stderr,"Failed to allocate internal fragment memory\n");
      exit(5);
    }
    if(start_pos<0)start_pos=0;
    if(start_pos>size-fragsize)start_pos=size-fragsize;
    if(end_pos<fragsize)end_pos=fragsize;
    if(end_pos>size)end_pos=size;
    current_pos=start_pos;

    /* fire off helper threads */
    if(pthread_create(&playback_handle,NULL,playback_thread,&state)){
      fprintf(stderr,"Failed to create playback thread.\n");
      exit(7);
    }
    if(pthread_create(&fd_handle,NULL,key_thread,&state)){
      fprintf(stderr,"Failed to create playback thread.\n");
      exit(7);
    }

    /* prepare playback loop */
    pthread_mutex_lock(&state.mutex);

    while(1){

      int c;
      if(state.exiting) break;

      if((!state.key_waiting || do_flip || do_pause || do_select) && state.fragment_size>0)
        pthread_cond_wait(&state.main_cond,&state.mutex);

      /* seeks and some other ops are batched */
      if(state.key_waiting && !do_flip && !do_pause && !do_select){
        /* service keyboard */
        c=state.key_waiting;
        pthread_mutex_unlock(&state.mutex);
        switch(c){
        case ERR:
          break;
        case 3:
          /* control-c == quit */
          pthread_mutex_lock(&state.mutex);
          state.exiting=1;
          pthread_mutex_unlock(&state.mutex);
          break;
        case KEY_UP:
          if(current_choice>0){
            flip_to=current_choice-1;
            do_flip=1;
          }
          break;
        case KEY_DOWN:
          flip_to=current_choice+1;
          do_flip=1;
          /* range checking enforced later */
          break;
        case 10: /* enter */
        case 13: /* return */
          /* guarded below */
          flip_to = current_choice;
          do_select=1;
          break;
        case '0':
          flip_to=9;
          do_flip=1;
          break;
          break;
        case '9': case '8': case '7': case '6':
        case '5': case '4': case '3': case '2': case '1':
          flip_to=c-'1';
          do_flip=1;
          break;
        case 'a':
          flip_to=0;
          do_flip=1;
          break;
        case 'b':
          flip_to=1;
          do_flip=1;
          break;
        case 'x':
          flip_to=2;
          do_flip=1;
          break;
        case 'A':
          flip_to=0;
          do_select=1;
          break;
        case 'B':
          flip_to=1;
          do_select=1;
          break;
        case 'X':
          flip_to=2;
          do_select=1;
          break;
        case '!':
          flip_to=0;
          do_select=1;
          break;
        case '@':
          flip_to=1;
          do_select=1;
          break;
        case '#':
          flip_to=2;
          do_select=1;
          break;
        case ' ':
          do_pause=1;
          break;
        case KEY_LEFT:
          seek_to-=pcm[0]->rate*bpf*2;
          do_seek=1;
          break;
        case KEY_RIGHT:
          seek_to+=pcm[0]->rate*bpf*2;
          do_seek=1;
          break;
        case KEY_SLEFT:
          seek_to-=pcm[0]->rate*bpf*10;
          do_seek=1;
          break;
        case KEY_SRIGHT:
          seek_to+=pcm[0]->rate*bpf*10;
          do_seek=1;
          break;
        case KEY_BACKSPACE:
          seek_to=start_pos-current_pos;
          do_seek=1;
          break;
        case 'f':
          beep_mode++;
          if(beep_mode>3)beep_mode=1;
          break;
        case 'r':
          restart_mode++;
          if(test_mode==3 && restart_mode==1)restart_mode++;
          if(restart_mode>2)restart_mode=0;
          break;
        case 's':
          start_pos=current_pos;
          if(start_pos<0)start_pos=0;
          if(start_pos>pcm[0]->size-fragsize*3)
            start_pos=pcm[0]->size-fragsize*3;
          break;
        case 'S':
          start_pos=0;
          break;
        case 'e':
          end_pos=current_pos;
          if(end_pos<fragsize*3)end_pos=fragsize*3;
          if(end_pos>pcm[0]->size)end_pos=pcm[0]->size;
          break;
        case 'E':
          end_pos=pcm[0]->size;
          break;
        case '?':
          panel_toggle_keymap();
          break;
        case 331:
          if(tests_cursor<tests_total && !running_score)
            tests_cursor++;
          break;
        case 330:
          if(tests_cursor>0 && !running_score){
            tests_cursor--;
            undos++;
          }
          break;
        }

        if(do_flip && flip_to==current_choice) do_flip=0;

        switch(test_mode){
        case 0:
          if(flip_to>1){
            do_flip=0;
            do_select=0;
          }
          break;
        case 1:
          if(do_select && flip_to>1){
            do_select=0;
          }
        case 2:
          if(flip_to>2){
            do_flip=0;
            do_select=0;
          }
          break;
        case 3:
          if(flip_to>=test_files)
            do_flip=0;
          if(do_select)
            do_select=0;
          break;
        }

        while(current_pos + seek_to>end_pos)seek_to-=(end_pos-start_pos);
        while(current_pos + seek_to<start_pos)seek_to+=(end_pos-start_pos);

        pthread_mutex_lock(&state.mutex);
        state.key_waiting=0;
        pthread_cond_signal(&state.key_cond);
      }

      /* update terminal */
      {
        double current = current_pos*base;
        double start = start_pos*base;
        double end = end_pos>0?end_pos*base:len;

        pthread_mutex_unlock(&state.mutex);
        panel_update_start(start);
        panel_update_current(current);
        panel_update_end(end);
        panel_update_pause(paused);
        panel_update_playing(current_choice);
        panel_update_repeat_mode(restart_mode);
        panel_update_flip_mode(beep_mode);
        panel_update_trials(choice_list,sample_list,tests_cursor);
        min_flush();
        pthread_mutex_lock(&state.mutex);
      }

      if(state.fragment_size==0 && !state.exiting){
        /* fill audio output */
        off_t save_pos=current_pos;
        int save_loop=loop;
        pthread_mutex_unlock(&state.mutex);

        if(do_flip){
          current_choice=flip_to;
          if(restart_mode==2){
            seek_to += start_pos-current_pos;
            do_seek=1;
          }
        }

        if(do_select){

          /* record choice; during selection, flip_to contains the test choice  */
          choice_list[tests_cursor] = flip_to;
          sample_list[tests_cursor] = (randomize[flip_to] == cchoice);
          tests_cursor++;
          tests_total=tests_cursor;

          /* randomize now so we can fill in fragmentB */
          randomize_samples(randomize,&cchoice,test_mode);
          if(restart_mode){
            seek_to += start_pos-current_pos;
            do_seek=1;
          }

          if(tests_cursor==tests){
            pthread_mutex_lock(&state.mutex);
            state.exiting=1;
            pthread_mutex_unlock(&state.mutex);
            break;
          }
        }

        if(paused && !do_pause){
          current_sample=randomize[current_choice];
          memset(fragmentA,0,fragsize);
          if(do_seek){
            current_pos+=seek_to;
            seek_to=0;
            do_seek=0;
            loop=0;
          }
        }else{
          fragments_played++;
          fill_fragment1(fragmentA, pcm[current_sample], start_pos, &current_pos, end_pos, &loop,
                         fragsamples, fadewindow1);
          if(do_flip || do_seek || do_select){
            current_sample=randomize[current_choice];
            if(do_seek){
              current_pos=save_pos+seek_to;
              fill_fragment2(fragmentB, pcm[current_sample], start_pos, &current_pos, end_pos, &loop,
                             fragsamples, fadewindow1);
              seek_to=0;
              seeks++;
            }else{
              fill_fragment1(fragmentB, pcm[current_sample], start_pos, &save_pos, end_pos, &save_loop,
                             fragsamples, fadewindow1);
            }
          }
        }

        if(do_flip || do_select || do_seek){
          int j;
          unsigned char *A=fragmentA;
          unsigned char *B=fragmentB;
          float *wA=fadewindow1, *wB, *beep=0;
          if(do_select){
            wA=fadewindow3;
            beep=beep2;
          }
          if(do_flip){
            /* A and B are crossfaded according to beep mode */
            beep=0;
            switch(beep_mode){
            case 1: /* mark, fadewindow 2 */
              wA=fadewindow2;
              flips[0]++;
              break;
            case 2:
              wA=fadewindow3;
              beep=beep1;
              flips[1]++;
              break;
            case 3:
              wA=fadewindow1;
              flips[2]++;
              break;
            }
          }
          wB=wA+fragsamples-1;
          for(i=0;i<fragsamples;i++){
            for(j=0;j<ch;j++){
              put_val(A,bps,get_val(A,bps)**(wA+i) + get_val(B,bps)**(wB-i) + (beep?beep[i]:0.f));
              A+=bps;
              B+=bps;
            }
          }
          do_flip=0;
          do_select=0;
          do_seek=0;
        }else if(do_pause){
          unsigned char *A=fragmentA;
          int j;
          if(paused){
            float *wA=fadewindow1+fragsamples-1;
            for(i=0;i<fragsamples;i++){
              for(j=0;j<ch;j++){
                put_val(A,bps,get_val(A,bps)**(wA-i));
                A+=bps;
              }
            }

          }else{
            float *wA=fadewindow1;
            for(i=0;i<fragsamples;i++){
              for(j=0;j<ch;j++){
                put_val(A,bps,get_val(A,bps)**(wA+i));
                A+=bps;
              }
            }
          }
          paused = !paused;
          do_pause=0;
          memset(fragmentB,0,fragsize);
        }

        pthread_mutex_lock(&state.mutex);
        state.fragment=fragmentA;
        state.fragment_size=fragsize;
        pthread_cond_signal(&state.play_cond);
      }
    }
  }

  /* tear down terminal */
  min_panel_remove();

  /* join */
  write(exit_fds[1]," ",1);
  pthread_cond_signal(&state.play_cond);
  pthread_cond_signal(&state.key_cond);
  pthread_mutex_unlock(&state.mutex);

  if(test_mode!=3 && tests_cursor>0){
    int total1=0;

    for(i=0;i<tests_cursor;i++)
      total1+=sample_list[i];
    switch(test_mode){
    case 0:
      fprintf(stdout, "\nA/B test results:\n");
      fprintf(stdout, "\tSample 1 (%s): %d/%d trials.\n",pcm[0]->name,tests_cursor-total1,tests_cursor);
      fprintf(stdout, "\tSample 2 (%s): %d/%d trials.\n",pcm[1]->name,total1,tests_cursor);
      break;
    case 1:
      fprintf(stdout, "\nA/B/X test results:\n");
      fprintf(stdout, "\tCorrect sample identified %d/%d trials.\n",total1,tests_cursor);
      break;
    case 2:
      fprintf(stdout, "\nX/X/Y test results:\n");
      fprintf(stdout, "\tCorrect sample identified %d/%d trials.\n",total1,tests_cursor);
      break;
    }

    if(test_mode==0){
      double p = compute_pdual(total1,tests_cursor);
      if(total1>0 && total1<tests_cursor)
        fprintf(stdout, "\tProbability of equal/more significant result via random chance: %.2f%%\n",p*100);
      else
        fprintf(stdout, "\tProbability of equally significant result via random chance: %.2f%%\n",p*100);
      if(p<.01)
        fprintf(stdout,"\tStatistically significant result (>=99%% confidence).\n");
    }else{
      // 0.5^20*(20!/(17!*3!))
      double p = compute_psingle(total1,tests_cursor);
      if(total1<tests_cursor)
        fprintf(stdout, "\tProbability of %d or better correct via random chance: %.2f%%\n",total1,p*100);
      else
        fprintf(stdout, "\tProbability of %d correct via random chance: %.2f%%\n",total1,p*100);
      if(p<.01)
        fprintf(stdout,"\tStatistically significant result (>=99%% confidence).\n");
    }

    fprintf(stdout,"\nTesting metadata:\n");

    if(tests_cursor<tests)
      fprintf(stdout,"\tTest was aborted early (%d/%d trials).\n",tests_cursor,tests);
    fprintf(stdout,"\tTotal time spent testing: %s\n",make_time_string(fragments_played*.1,0));
    fprintf(stdout,"\tTotal seeks: %d\n",seeks);
    if(flips[0])
      fprintf(stdout,"\tMark flip used %d times.\n",flips[0]);
    if(flips[1])
      fprintf(stdout,"\tBeep flip used %d times.\n",flips[1]);
    if(flips[2])
      fprintf(stdout,"\tSilent flip used %d times.\n",flips[2]);


    if(running_score){
      fprintf(stdout,"\tRunning totals (-g) displayed during test.\n");
    }else{
      if(undos)
        fprintf(stdout,"\tUndo was used %d time[s].\n",undos);
      else
        fprintf(stdout,"\tUndo was not used.\n");
    }
    fprintf(stdout,"\n");
  }

  if(sb_verbose)
    fprintf(stderr,"\nWaiting on keyboard thread...");
  pthread_join(fd_handle,NULL);
  if(sb_verbose)
    fprintf(stderr," joined.\nWaiting on playback thread...");
  pthread_join(playback_handle,NULL);
  if(sb_verbose)
    fprintf(stderr," joined.\n");
  free(fadewindow1);
  free(fadewindow2);
  free(fadewindow3);
  free(beep1);
  free(beep2);
  free(fragmentA);
  free(fragmentB);
  for(i=0;i<test_files;i++)
    free_pcm(pcm[i]);
  if(sb_verbose)
    fprintf(stderr,"Done.\n");
  return 0;
}

