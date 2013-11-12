/*
 *
 *  squishyball
 *
 *      Copyright (C) 2010-2013 Xiph.Org
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

static float get_clamp(pcm_t *pcm){
  if(pcm->nativebits>=0 && pcm->nativebits<24)
    return 1.f - 1.f/(1<<(pcm->nativebits-1));
  else
    return 1.f - 1.f/2147483648.f;
}

float check_warn_clipping(pcm_t *pcm, int no_normalize){
  int i,j;
  int cpf = pcm->ch;
  int s = pcm->size/sizeof(float);
  float clamp;
  float min,max;
  int flag[cpf];
  size_t count=0;
  float *d = (float *)pcm->data;

  memset(flag,0,sizeof(flag));

  clamp = max = get_clamp(pcm);
  min=-1.f;

  for(i=0;i<s;i+=pcm->ch)
    for(j=0;j<pcm->ch;j++){
      if(d[i+j]<-1.f){
        if(d[i+j]<min)min=d[i+j];
        flag[j]++;
      }else if(d[i+j]>clamp){
        if(d[i+j]>max)max=d[i+j];
        flag[j]++;
      }else{
        if(flag[j]>1)count+=flag[j];
        flag[j]=0;
      }
    }
  for(j=0;j<cpf;j++)
    if(flag[j]>1)count+=flag[j];

  if(count){
    if(pcm->nativebits>0){
      fprintf(stderr,"CLIPPING WARNING: %ld probably clipped samples in %s;\n",(long)count,pcm->name);
      fprintf(stderr,"                  (can't be repaired with normalization)\n");
    }else{
      if(no_normalize){
        fprintf(stderr,"CLIPPING WARNING: %ld clipped samples in %s;\n",(long)count,pcm->name);
        fprintf(stderr,"                  normalization disabled on command line.\n");
      }else{
        float att = -1./min;
        if(clamp/max < att) att=clamp/max;
        if(sb_verbose){
          fprintf(stderr,"%ld overrange samples after decoding %s (peak %+0.1fdB)\n",(long)count,pcm->name,todB(1./att));
        }
        return att;
      }
    }
  }

  return 1.;
}

/* input must be float */
float convert_to_mono(pcm_t *pcm){
  int i,j,k;
  int cpf = pcm->ch;
  int s = pcm->size/sizeof(float);
  float *d = (float *)pcm->data;
  float max=0;
  float min=0;
  float att=1.f;
  float clamp = get_clamp(pcm);

  if(pcm->currentbits!=-32){
    fprintf(stderr,"Internal error; non-float PCM passed to convert_to_mono.\n");
    exit(10);
  }

  if(sb_verbose)
    fprintf(stderr,"Downmixing to mono... ");

  k=0;
  for(i=0;i<s;i+=cpf){
    float acc=0.f;
    for(j=0;j<cpf;j++)
      acc+=d[i+j];
    if(acc>max)max=acc;
    if(acc<min)min=acc;
    d[k++]=acc;
  }


  pcm->size/=cpf;
  pcm->ch=1;
  if(pcm->matrix)free(pcm->matrix);
  pcm->matrix=strdup("M");
  if(min<-1.f) att=-1./min;
  if(clamp/max < att) att=clamp/max;

  if(sb_verbose){
    if(att<1.){
      fprintf(stderr,"done. peak: %+0.1fdB\n",todB(1./att));
    }else{
      fprintf(stderr,"done.\n");
    }
  }
  return att;
}

/* non-normalized */
static const int left_mix[33]={
  1.,    /* A: M */
  1.,    /* B: L */
  0.,    /* C: R */
  0.707, /* D: C */
  0.707, /* E: LFE */
  0.866, /* F: BL */
  0.5,   /* G: BR */
  0.791, /* H: CL */
  0.612, /* I: CR */
  0.612, /* J: BC */
  0.866, /* K: SL */
  0.5,   /* L: SR */
  0
};

static const int right_mix[33]={
  1.,    /* A: M */
  0.,    /* B: L */
  1.,    /* C: R */
  0.707, /* D: C */
  0.707, /* E: LFE */
  0.5,   /* F: BL */
  0.866, /* G: BR */
  0.612, /* H: CL */
  0.791, /* I: CR */
  0.612, /* J: BC */
  0.5,   /* K: SL */
  0.866, /* L: SR */
  0
};

/* input must be float */
float convert_to_stereo(pcm_t *pcm){
  int i,j,k;
  int cpf = pcm->ch;
  int s = pcm->size/sizeof(float);
  float *d = (float *)pcm->data;
  float max=0;
  float min=0;
  float att=1.f;
  float clamp = get_clamp(pcm);

  float *lmix,*rmix;

  if(pcm->currentbits!=-32){
    fprintf(stderr,"Internal error; non-float PCM passed to convert_to_mono.\n");
    exit(10);
  }
  if(pcm->ch<2){
    fprintf(stderr,"Internal error; can't downmix mono to stereo.\n");
    exit(10);
  }

  if(sb_verbose)
    fprintf(stderr,"Downmixing to stereo... ");

  lmix=calloc(cpf,sizeof(*lmix));
  rmix=calloc(cpf,sizeof(*rmix));
  for(j=0;j<cpf;j++){
    lmix[j] = left_mix[pcm->mix[j]-'A'];
    rmix[j] = right_mix[pcm->mix[j]-'A'];
  }

  k=0;
  for(i=0;i<s;i+=cpf){
    float L=0.f,R=0.f;

    for(j=0;j<cpf;j++){
      L+=d[i+j]*lmix[j];
      R+=d[i+j]*rmix[j];
    }

    if(L>max)max=L;
    if(L<min)min=L;
    if(R>max)max=R;
    if(R<min)min=R;
    d[k++]=L;
    d[k++]=R;
  }

  pcm->size=pcm->size/cpf*2;
  pcm->ch=2;
  if(pcm->matrix)free(pcm->matrix);
  pcm->matrix=strdup("L,R");

  if(min<-1.f) att=-1./min;
  if(clamp/max < att) att=clamp/max;

  if(sb_verbose){
    if(att<1.f){
      fprintf(stderr,"done. peak: %+0.1fdB\n",todB(1./att));
    }else{
      fprintf(stderr,"done.\n");
    }
  }
  return att;
}

static inline float triangle_ditherval(float *save){
  float r = rand()/(float)RAND_MAX-.5f;
  float ret = *save-r;
  *save = r;
  return ret;
}

void convert_to_32(pcm_t *pcm){
  unsigned char *d = pcm->data;
  float *f = (float *)pcm->data;
  off_t j;
  if(sb_verbose)
    fprintf(stderr,"\rConverting %s to 32 bit... ",pcm->name);
  for(j=0;j<pcm->size/sizeof(float);j++){
    float val = rint(f[j]*2147483648.f);
    int iv;
    if(val<-2147483648.f) val = -2147483648.f;
    if(val> 2147483647.f) val = 2147483647.f;
    iv=(int)val;
    d[j*4]=iv&0xff;
    d[j*4+1]=(iv>>8)&0xff;
    d[j*4+2]=(iv>>16)&0xff;
    d[j*4+3]=(iv>>24)&0xff;
  }
  if(sb_verbose)
    fprintf(stderr,"done.\n");
  pcm->currentbits=32;
  pcm->size/=sizeof(float);
  pcm->size*=4;
}

void convert_to_24(pcm_t *pcm){
  unsigned char *d = pcm->data;
  float *f = (float *)pcm->data;
  off_t j;
  if(sb_verbose)
    fprintf(stderr,"\rConverting %s to 24 bit... ",pcm->name);
  for(j=0;j<pcm->size/sizeof(float);j++){
    float val = rint(f[j]*8388608.f);
    int iv;
    if(val<-8388608.f) val = -8388608.f;
    if(val> 8388607.f) val = 8388607.f;
    iv=(int)val;
    d[j*3]=iv&0xff;
    d[j*3+1]=(iv>>8)&0xff;
    d[j*3+2]=(iv>>16)&0xff;
  }
  if(sb_verbose)
    fprintf(stderr,"done.\n");
  pcm->currentbits=24;
  pcm->size/=sizeof(float);
  pcm->size*=3;
}

void convert_to_16(pcm_t *pcm, int dither){
  unsigned char *d = pcm->data;
  float *f = (float *)pcm->data;
  off_t j;
  float t[pcm->ch];
  int ch=0;
  memset(t,0,sizeof(t));

  if(sb_verbose)
    fprintf(stderr,"\r%s %s to 16 bit... ",
            dither?"Dithering":"Down-converting",pcm->name);

  for(j=0;j<pcm->size/sizeof(float);j++){
    float val;
    if(dither){
      val = rint(f[j]*32768.f + triangle_ditherval(t+ch));
      ch++;
      if(ch>pcm->ch)ch=0;
    }else{
      val = rint(f[j]*32768.f);
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

  if(sb_verbose)
    fprintf(stderr,"done.\n");

  pcm->currentbits=16;
  pcm->size/=sizeof(float);
  pcm->size*=2;
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
  int bps = (B->currentbits+7)/8;
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
  float mul = (pcm[0]->currentbits==24 ? 8388608.f : 32768.f) * .0625;
  int bps=(pcm[0]->currentbits+7)/8;
  int ch=pcm[0]->ch;
  int bpf=ch*bps;
  int maxsamples = pcm[0]->size / bpf;
  if (fragsamples * 3 > maxsamples)
    fragsamples = maxsamples / 3;
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

  return fragsamples;
}

/* fragment is filled such that a crossloop never begins after
   pcm->size-fragsize, and it always begins from the start of the
   window, even if that means starting a crossloop late because the
   endpos moved. */
void fill_fragment1(unsigned char *out, pcm_t *pcm, off_t start, off_t *pos, off_t end, int *loop,
                    int fragsamples, float *fadewindow){
  int bps = (pcm->currentbits+7)/8;
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
          put_val(out,bps,val);
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
  int bps = (pcm->currentbits+7)/8;
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
    ao_info *ai;
    if(id<0)
      return NULL;
    ai=ao_driver_info(id);
    if(!ai)
      return NULL;
    aname=ai->short_name;
    if(sb_verbose)
      fprintf(stderr,"Opening [%s] %s for %d/%d and %d channel[s]...",aname,device,bits,rate,ch);
    ret=ao_open_live(id, &sf, &aoe);
    if(sb_verbose){
      if(!ret){
        fprintf(stderr," errno %d\n",errno);
      }else{
        fprintf(stderr," ok!\n");
      }
    }
  }else{
    /* Otherwise... there's some hunting to do. */
    /* Is the passed device a number or a name? */
    char *test;
    int count;
    ao_info **info_list=ao_driver_info_list(&count);
    int number=strtol(device,&test,10);
    int i;

    if(!device[0] || test[0]) number=-1;
    if(sb_verbose)
      fprintf(stderr,"Scanning for a device driver that recognizes '%s'...\n",device);

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
      if(j<info->option_count){
        if(sb_verbose)
          fprintf(stderr,"  ...trying to open [%s] %s for %d/%d and %d channel[s]...",aname,device,bits,rate,ch);
        if((ret=ao_open_live(id,&sf,&ao))){
          if(sb_verbose)
            fprintf(stderr," ok!\n");
          break;
        }
        if(sb_verbose)
          fprintf(stderr," errno %d\n",errno);
      }
    }
  }
  if(ret && sb_verbose)
    fprintf(stderr,"Opened %s%s audio device %s%sfor %d bit %d channel %d Hz...\n",
            (device?"":"default "),aname,
            (device?device:""),(device?" ":""),bits,ch,rate);

  return ret;
}

