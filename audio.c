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
#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>
#include <ao/ao.h>
#include "main.h"

/* sample formatting helpers ********************************************************/

static inline int host_is_big_endian() {
  union {
    int32_t pattern;
    unsigned char bytewise[4];
  } m;
  m.pattern = 0xfeedface; /* deadbeef */
  if (m.bytewise[0] == 0xfe) return 1;
  return 0;
}

static inline float triangle_ditherval(float *save){
  float r = rand()/(float)RAND_MAX-.5f;
  float ret = *save-r;
  *save = r;
  return ret;
}

static void float32_to_24(pcm_t *pcm){
  unsigned char *d = pcm->data;
  off_t j;
  if(sb_verbose)
    fprintf(stderr,"\rConverting %s to 24 bit... ",pcm->path);
  for(j=0;j<pcm->size/4;j++){
    int val=0;
    int mantissa = d[j*4] | (d[j*4+1]<<8) | ((d[j*4+2]&0x7f)<<16) | (1<<23);
    int exponent = 127 - ((d[j*4+2]>>7) | ((d[j*4+3]&0x7f)<<1));
    int sign = d[j*4+3]>>7;
    if(exponent <= 0){
      if(exponent == -128){
        fprintf(stderr,"%s: Input file contains invalid floating point values.\n",pcm->path);
        exit(6);
      }
      if(sign)
        val = 8388608;
      else
        val = 8388607;
    }else if(exponent <= 24){
      val = mantissa>>exponent;
      /* round with tiebreaks toward even */
      if(((mantissa<<(24-exponent))&0xffffff) + (val&1) > 0x800000) ++val;
    }
    if(sign) val= -val;

    d[j*3]=val&0xff;
    d[j*3+1]=(val>>8)&0xff;
    d[j*3+2]=(val>>16)&0xff;
  }
  if(sb_verbose)
    fprintf(stderr,"...done.\n");
  pcm->bits=24;
  pcm->size/=4;
  pcm->size*=3;
}

static void float32_to_16(pcm_t *pcm){
  unsigned char *d = pcm->data;
  off_t j;
  union {
    float f;
    unsigned char c[4];
  } m;

  if(sb_verbose)
    fprintf(stderr,"\r%s %s to 16 bit... ",
            pcm->dither?"Dithering":"Down-converting",pcm->path);

  /* again assumes IEEE754, which is not pedantically correct */
  if(sizeof(float)==4){
    float t[pcm->ch];
    int ch=0;
    memset(t,0,sizeof(t));

    for(j=0;j<pcm->size/4;j++){
      float val;
      if(host_is_big_endian()){
        m.c[0]=d[j*4+3];
        m.c[1]=d[j*4+2];
        m.c[2]=d[j*4+1];
        m.c[3]=d[j*4];
      }else{
        m.c[0]=d[j*4];
        m.c[1]=d[j*4+1];
        m.c[2]=d[j*4+2];
        m.c[3]=d[j*4+3];
      }
      if(pcm->dither){
        val = rint(m.f*32768.f + triangle_ditherval(t+ch));
        ch++;
        if(ch>pcm->ch)ch=0;
      }else{
        val = rint(m.f*32768.f);
      }

      if(val>=32767.f){
        d[j*2]=0xff;
        d[j*2+1]=0x7f;
      }else if(val<=-32768.f){
        d[j*2]=0x00;
        d[j*2+1]=0x80;
      }else{
        int iv = (int)val;
        d[j*2]=iv&0xff;
        d[j*2+1]=(iv>>8)&0xff;
      }
    }
  }else{
    fprintf(stderr,"Unhandled case: sizeof(float)!=4\n");
    exit(10);
  }

  if(sb_verbose)
    fprintf(stderr,"...done.\n");

  pcm->bits=16;
  pcm->size/=2;
}

static void demote_24_to_16(pcm_t *pcm){
  float t[pcm->ch];
  unsigned char *d = pcm->data;
  off_t i;
  int ch=0;

  if(sb_verbose)
    fprintf(stderr,"\r%s %s to 16 bit... ",
            pcm->dither?"Dithering":"Down-converting",pcm->path);

  memset(t,0,sizeof(t));

  for(i=0;i<pcm->size/3;i++){
    int val = ((d[i*3+2]<<24) | (d[i*3+1]<<16) | (d[i*3]<<8))>>8;
    if(pcm->dither){
      val = rint (val*(1.f/256.f)+triangle_ditherval(t+ch));
      ch++;
      if(ch>pcm->ch)ch=0;
    }else
      val = rint (val*(1.f/256.f));

    if(val>32767){
      d[i*2]=0xff;
      d[i*2+1]=0x7f;
    }else if(val<-32768){
      d[i*2]=0x00;
      d[i*2+1]=0x80;
    }else{
      d[i*2]=val&0xff;
      d[i*2+1]=(val>>8)&0xff;
    }
  }

  if(sb_verbose)
    fprintf(stderr,"...done.\n");

  pcm->bits=16;
  pcm->size/=3;
  pcm->size*=2;
}

static void promote_to_24(pcm_t *pcm){
  off_t i;

  if(sb_verbose)
    fprintf(stderr,"\rPromoting %s to 24 bit... ",pcm->path);

  {
    unsigned char *ret=realloc(pcm->data,pcm->size*3/2);
    if(!ret){
      fprintf(stderr,"Unable to allocate memory while promoting file to 24-bit\n");
      exit(5);
    }
    pcm->data=ret;
    for(i=pcm->size/2-1;i>=0;i--){
      ret[i*3+2]=ret[i*2+1];
      ret[i*3+1]=ret[i*2];
      ret[i*3]=0;
    }
  }
  if(sb_verbose)
    fprintf(stderr,"...done.\n");

  pcm->bits=24;
  pcm->size/=2;
  pcm->size*=3;
}

void convert_to_16(pcm_t *pcm){
  switch(pcm->bits){
  case 16:
    break;
  case 24:
    demote_24_to_16(pcm);
    break;
  case -32:
    float32_to_16(pcm);
    break;
  default:
    fprintf(stderr,"%s: Unsupported sample format.\n",pcm->path);
    exit(6);
  }
}

void convert_to_24(pcm_t *pcm){
  switch(pcm->bits){
  case 16:
    promote_to_24(pcm);
    break;
  case 24:
    break;
  case -32:
    float32_to_24(pcm);
    break;
  default:
    fprintf(stderr,"%s: Unsupported sample format.\n",pcm->path);
    exit(6);
  }
}

/* Channel map reconciliation helpers *********************************/

static const char *chlist[]={"X","M","L","R","C","LFE","SL","SR","BC","BL","BR","CL","CR",NULL};

static void tokenize_channels(char *matrix,int *out,int n){
  int i=0;
  char *t=strtok(matrix,",");
  memset(out,0,sizeof(*out)*n);

  while(t){
    int j=0;
    while(chlist[j]){
      if(!strcmp(chlist[j],t))break;
      j++;
    }
    out[i]=j;
    i++;
    t=strtok(NULL,",");
  }
}

/* pre-permute sample ordering so that playback incurs ~equal
   CPU/memory/etc load during playback */
/* A and B must have machine sample formats */
void reconcile_channel_maps(pcm_t *A, pcm_t *B){
  /* arbitrary; match B to A */
  int ai[A->ch],bi[A->ch];
  int i,j,k;
  off_t o;
  int bps = (B->bits+7)/8;
  int bpf = B->ch*bps;
  int p[bpf];
  unsigned char temp[bpf];
  unsigned char *d;

  tokenize_channels(A->matrix,ai,A->ch);
  tokenize_channels(B->matrix,bi,A->ch);

  for(i=0;i<A->ch;i++){
    for(j=0;j<A->ch;j++){
      if(bi[i]==ai[j]){
        for(k=0;k<bps;k++)
          p[i*bps+k]=j*bps+k;
        break;
      }
    }
  }

  d=B->data;
  for(o=0;o<B->size;){
    for(i=0;i<bpf;i++)
      temp[p[i]]=d[i];
    memcpy(d,temp,bpf);
    d+=bpf;
  }

  free(B->matrix);
  B->matrix = strdup(A->matrix);
}

/* fade and beep function generation **************************************/

void put_val(unsigned char *d,int bps,float v){
  int i = rint(v);
  d[0]=i&0xff;
  d[1]=(i>>8)&0xff;
  if(bps==3)
    d[2]=(i>>16)&0xff;
}

float get_val(unsigned char *d, int bps){
  if(bps==2){
    short i = d[0] | (d[1]<<8);
    return (float)i;
  }else{
    int32_t i = ((d[0]<<8) | (d[1]<<16) | (d[2]<<24))>>8;
    return (float)i;
  }
}

int setup_windows(pcm_t **pcm, int test_files,
                         float **fw1, float **fw2, float **fw3,
                         float **b1, float **b2){
  int i;
  int fragsamples = pcm[0]->rate/10;  /* 100ms */
  float mul = (pcm[0]->bits==24 ? 8388608.f : 32768.f) * .0625;
  /* precompute the fades/beeps */
  float *fadewindow1 = *fw1 = calloc(fragsamples,sizeof(*fadewindow1));
  float *fadewindow2 = *fw2 = calloc(fragsamples,sizeof(*fadewindow2));
  float *fadewindow3 = *fw3 = calloc(fragsamples,sizeof(*fadewindow3));
  float *beep1 = *b1 = calloc(fragsamples,sizeof(*beep1));
  float *beep2 = *b2 = calloc(fragsamples,sizeof(*beep2));

  if(!fadewindow1 ||
     !fadewindow2 ||
     !fadewindow3 ||
     !beep1 ||
     !beep2)
    exit(9);

  /* fadewindow1 is a larger simple crossfade */
  for(i=0;i<fragsamples;i++){
    float val = cosf(M_PI*.5f*(i+.5f)/fragsamples);
    fadewindow1[i] = val*val;
  }

  /* fadewindow2 goes to silence and back */
  for(i=0;i<fragsamples/3;i++){
    float val = cosf(M_PI*1.5f*(i+.5f)/fragsamples);
    fadewindow2[i] = val*val;
  }
  for(;i<fragsamples;i++)
    fadewindow2[i] = 0.f;

  /* fadewindow3 is like fadewindow 2 but with briefer silence */
  for(i=0;i<fragsamples/2;i++){
    float val = cosf(M_PI*(i+.5f)/fragsamples);
    fadewindow3[i] = val*val;
  }
  for(;i<fragsamples;i++)
    fadewindow3[i] = 0.f;

  /* Single beep for flipping */
  for(i=0;i<fragsamples/4;i++){
    beep1[i]=0.f;
    beep1[fragsamples-i-1]=0.f;
  }
  float base = 3.14159f*2.f*1000./pcm[0]->rate;
  for(;i<fragsamples*3/4;i++){
    float f = i-fragsamples/4+.5f;
    float w = cosf(3.14159f*f/fragsamples);
    float b =
      sinf(f*base)+
      sinf(f*base*3)*.33f+
      sinf(f*base*5)*.2f+
      sinf(f*base*7)*.14f+
      sinf(f*base*9)*.11f;
    w*=w;
    beep1[i] = w*b*mul;
  }

  /* Double beep for selection */
  for(i=0;i<fragsamples/4;i++){
    beep2[i]=0.f;
    beep2[fragsamples-i-1]=0.f;
  }
  for(;i<fragsamples/2;i++){
    float f = i-fragsamples/4+.5f;
    float w = cosf(3.14159f*2.f*f/fragsamples);
    float b =
      sinf(f*base)+
      sinf(f*base*3)*.33f+
      sinf(f*base*5)*.2f+
      sinf(f*base*7)*.14f+
      sinf(f*base*9)*.11f;
    w*=w;
    beep2[i] = w*b*mul;
  }
  base = 3.14159f*2.f*1500./pcm[0]->rate;
  for(;i<fragsamples*3/4;i++){
    float f = i-fragsamples/2+.5f;
    float w = cosf(3.14159f*2.f*f/fragsamples);
    float b =
      sinf(f*base)+
      sinf(f*base*3)*.33f+
      sinf(f*base*5)*.2f+
      sinf(f*base*7)*.14f+
      sinf(f*base*9)*.11f;
    w*=w;
    beep2[i] = w*b*mul*2;
  }

  /* make sure that the samples are at least fragsamples*3 in length! If they're not, extend... */
  {
    int bps = (pcm[0]->bits+7)/8;
    int ch = pcm[0]->ch;
    int bpf = bps*ch;

    if(pcm[0]->size<fragsamples*bpf*3){
      int fadesize = pcm[0]->size/4;

      for(i=0;i<test_files;i++){
        int j,k;
        unsigned char *newd=calloc(fragsamples*3,bpf);
        if(!newd){
          fprintf(stderr,"Unable to allocate memory to extend sample to minimum length.\n");
          exit(5);
        }
        memcpy(newd,pcm[i]->data,fragsamples*3*bpf);
        free(pcm[i]->data);
        pcm[i]->data=newd;

        newd+=pcm[i]->size-fadesize;
        for(j=0;j<fadesize;j++){
          float v = cosf(M_PI*.5f*(i+.5f)/fadesize);
          for(k=0;k<ch;k++){
            put_val(newd,bps,v * get_val(newd,bps));
            newd+=bps;
          }
        }
        pcm[i]->size=fragsamples*3;
      }
    }
  }
  return fragsamples;
}

/* fragment is filled such that a crossloop never begins after
   pcm->size-fragsize, and it always begins from the start of the
   window, even if that means starting a crossloop late because the
   endpos moved. */
void fill_fragment1(unsigned char *out, pcm_t *pcm, off_t start, off_t *pos, off_t end, int *loop,
                    int fragsamples, float *fadewindow){
  int bps = (pcm->bits+7)/8;
  int cpf = pcm->ch;
  int bpf = bps*cpf;
  int fragsize = fragsamples*bpf;

  /* guard limits here */
  if(end<fragsize*3)end=fragsize*3;
  if(end>pcm->size)end=pcm->size;
  if(start<0)start=0;
  if(start>pcm->size-fragsize*3)start=pcm->size-fragsize*3;

  /* we fill a fragment from the data buffer of the passed in pcm_t.
     It's possible we'll need to crossloop from the end of the sample,
     and the start/end markers may have moved so that the cursor is
     outside the strict sample bounds. */

  /* if *loop>0, we're in the process of crosslapping at pos ><
     start+(fragsize-*loop*bpf). Stay the course. */
  if(*loop){
    int lp = *loop;
    int i,j;
    unsigned char *A = pcm->data+*pos;
    unsigned char *B = pcm->data+start+(fragsamples-lp)*bpf;
    for(i=0;i<fragsamples;i++){
      if(lp){
        float w = fadewindow[--lp];
        for(j=0;j<cpf;j++){
          float val = get_val(A,bps)*(1.f-w) + get_val(B,bps)*w;
          put_val(out,val,bps);
          A+=bps;
          B+=bps;
          out+=bps;
        }
      }else{
        /* crossloop finished, the rest is B */
        memcpy(out,B,bpf);
        B+=bpf;
        out+=bpf;
      }
    }
    *loop=0;
    *pos=B-pcm->data;
  }else{
    /* no crossloop in progress... should one be? If the cursor is
       before start, do nothing.  If it's past end-fragsize, begin a
       crossloop immediately.  If the current fragment will extend
       beyond end-fragsize, begin the crossloop at end-fragsize */
    if(*pos>pcm->size-fragsize){
      /* Error condition; should not be possible */
      fprintf(stderr,"Internal error; %ld>%ld, Monty fucked up.\n",(long)*pos,(long)pcm->size-fragsize);
      exit(100);
    }else if(*pos+fragsize>end-fragsize){
      int i,j;
      unsigned char *A = pcm->data+*pos;
      unsigned char *B = pcm->data+start;
      int lp = (end-*pos)/bpf;
      if(lp<fragsamples)lp=fragsamples; /* If we're late, start immediately, but use full window */

      for(i=0;i<fragsamples;i++){
        if(--lp>=fragsamples){
          /* still before crossloop begins */
          memcpy(out,A,bpf);
          A+=bpf;
          out+=bpf;
        }else{
          /* crosslooping */
          float w = fadewindow[lp];
          for(j=0;j<cpf;j++){
            float val = get_val(A,bps)*(1.f-w) + get_val(B,bps)*w;
            put_val(out,bps,val);
            A+=bps;
            B+=bps;
            out+=bps;
          }
        }
      }
      *loop=(lp<0?0:lp);
      *pos=(lp<=0?B-pcm->data:A-pcm->data);
    }else{
      /* no crossloop */
      unsigned char *A = pcm->data+*pos;
      memcpy(out,A,fragsize);
      *loop=0;
      *pos+=fragsize;
    }
  }
}

/* fragment is filled such that a crossloop is always 'exactly on
   schedule' even if that means beginning partway through the window. */
void fill_fragment2(unsigned char *out, pcm_t *pcm, off_t start, off_t *pos, off_t end, int *loop,
                    int fragsamples, float *fadewindow){
  int bps = (pcm->bits+7)/8;
  int cpf = pcm->ch;
  int bpf = bps*cpf;
  int fragsize=fragsamples*bpf;

  /* guard limits here */
  if(end<fragsize*3)end=fragsize*3;
  if(end>pcm->size)end=pcm->size;
  if(start<0)start=0;
  if(start>pcm->size-fragsize*3)start=pcm->size-fragsize*3;

  /* loop is never in progress for a fill_fragment2; called only during a seek crosslap */
  unsigned char *A = pcm->data+*pos;
  if(end-*pos>=fragsize*2){
    /* no crosslap */
    memcpy(out,A,fragsize);
    *loop=0;
    *pos=A-pcm->data+fragsize;
  }else{
    /* just before crossloop, in the middle of a crossloop, or just after crossloop */
    int i,j;
    int lp = (end-*pos)/bpf;
    unsigned char *B = pcm->data+start;
    if(lp<fragsamples)B+=(fragsamples-lp)*bpf;

    for(i=0;i<fragsamples;i++){
      --lp;
      if(lp>=fragsamples){
        /* not yet crosslooping */
        memcpy(out,A,bpf);
        A+=bpf;
        out+=bpf;
      }else if (lp>=0){
        /* now crosslooping */
        float w = fadewindow[lp];
        for(j=0;j<cpf;j++){
          float val = get_val(A,bps)*(1.-w) + get_val(B,bps)*w;
          put_val(out,val,bps);
          A+=bps;
          B+=bps;
          out+=bps;
        }
      }else{
        /* after crosslap */
        memcpy(out,B,bpf);
        B+=bpf;
        out+=bpf;
       }
    }
    *loop=(lp>0?(lp<fragsamples?lp:fragsamples):0);
    *pos=(lp>0?A-pcm->data:B-pcm->data);
  }
}

/* playback ******************************************************/

ao_device *setup_playback(int rate, int ch, int bits, char *matrix, char *device){
  ao_option aoe={0,0,0};
  ao_device *ret=NULL;
  ao_sample_format sf;
  char *aname="";
  sf.rate=rate;
  sf.channels=ch;
  sf.bits=bits;
  sf.byte_format=AO_FMT_LITTLE;
  sf.matrix=(ch>2?matrix:0);
  aoe.key="quiet";

  if(!device){
    /* if we don't have an explicit device, defaults make this easy */
    int id = ao_default_driver_id();
    aname=ao_driver_info(id)->short_name;
    ret=ao_open_live(id, &sf, &aoe);
  }else{
    /* Otherwise... there's some hunting to do. */
    /* Is the passed device a number or a name? */
    char *test;
    int count;
    ao_info **info_list=ao_driver_info_list(&count);
    int number=strtol(device,&test,10);
    int i;

    if(!device[0] || test[0]) number=-1;

    /* driver info list is sorted by priority */
    for(i=0;i<count;i++){
      int j;
      ao_info *info=info_list[i];
      ao_option ao={0,0,0};
      int id = ao_driver_id(info->short_name);
      char buf[80];

      sprintf(buf,"%d",number);
        ao.key=(number>=0?"id":"dev");
      ao.value=(number>=0?buf:device);
      ao.next=&aoe;
      aname=info->short_name;

      /* don't try to open the driver if it doesn't have the device/id
         option; it will ignore the option and try to open its default */
      for(j=0;j<info->option_count;j++)
        if(!strcmp(info->options[j],ao.key))break;
      if(j<info->option_count)
        if((ret=ao_open_live(id,&sf,&ao)))break;
    }
  }
  if(ret && sb_verbose)
    fprintf(stderr,"Opened %s%s audio device %s%sfor %d bit %d channel %d Hz...\n",
            (device?"":"default "),aname,
            (device?device:""),(device?" ":""),bits,ch,rate);

  return ret;
}

