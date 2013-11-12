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
#include <vorbis/vorbisfile.h>
#include <opus/opusfile.h>
#include <FLAC/stream_decoder.h>
#include <unistd.h>
#include "main.h"

static inline int host_is_big_endian() {
  union {
    int32_t pattern;
    unsigned char bytewise[4];
  } m;
  m.pattern = 0xfeedface; /* deadbeef */
  if (m.bytewise[0] == 0xfe) return 1;
  return 0;
}

static char *trim_path(char *in){
  /* search back to first /, \, or : */
  if(in){
    char *a = strrchr(in,'/');
    char *b = strrchr(in,'\\');
    char *c = strrchr(in,':');
    int posa = (a ? a-in+1 : 0);
    int posb = (b ? b-in+1 : 0);
    int posc = (c ? c-in+1 : 0);
    if(posb>posa)posa=posb;
    if(posc>posa)posa=posc;
    return in+posa;
  }
  return NULL;
}

typedef struct{
  int (*id_func)(char *path,unsigned char *buf);
  pcm_t *(*load_func)(char *path, FILE *in);
  char *format;
} input_format;

/* steal/simplify/expand file ID/load code from oggenc */

/* Macros to read header data */
#define READ_U32_LE(buf) \
    (((buf)[3]<<24)|((buf)[2]<<16)|((buf)[1]<<8)|((buf)[0]&0xff))

#define READ_U16_LE(buf) \
    (((buf)[1]<<8)|((buf)[0]&0xff))

#define READ_U32_BE(buf) \
    (((buf)[0]<<24)|((buf)[1]<<16)|((buf)[2]<<8)|((buf)[3]&0xff))

#define READ_U16_BE(buf) \
    (((buf)[0]<<8)|((buf)[1]&0xff))

static int wav_id(char *path,unsigned char *buf){
  if(memcmp(buf, "RIFF", 4))
    return 0; /* Not wave */
  if(memcmp(buf+8, "WAVE",4))
    return 0; /* RIFF, but not wave */
  return 1;
}

static int aiff_id(char *path,unsigned char *buf){
  if(memcmp(buf, "FORM", 4))
    return 0;
  if(memcmp(buf+8, "AIF",3))
    return 0;
  if(buf[11]!='C' && buf[11]!='F')
    return 0;
  return 1;
}

static int flac_id(char *path,unsigned char *buf){
  return memcmp(buf, "fLaC", 4) == 0;
}

static int oggflac_id(char *path,unsigned char *buf){
  return memcmp(buf, "OggS", 4) == 0 &&
    (memcmp (buf+28, "\177FLAC", 5) == 0 ||
     flac_id(path,buf+28));
}

static int vorbis_id(char *path,unsigned char *buf){
  return memcmp(buf, "OggS", 4) == 0 &&
    memcmp (buf+28, "\x01vorbis", 7) == 0;
}

static int opus_id(char *path,unsigned char *buf){
  return memcmp(buf, "OggS", 4) == 0 &&
    memcmp (buf+28, "OpusHead", 8) == 0;
}

static int sw_id(char *path,unsigned char *buf){
  /* if all else fails, look for JM's favorite extension */
  return memcmp(path+strlen(path)-3,".sw",3)==0;
}

/* WAV file support ***********************************************************/

static int find_wav_chunk(FILE *in, char *path, char *type, unsigned int *len){
  unsigned char buf[8];

  while(1){
    if(fread(buf,1,8,in) < 8){
      fprintf(stderr, "%s: Unexpected EOF in reading WAV header\n",path);
      return 0; /* EOF before reaching the appropriate chunk */
    }

    if(memcmp(buf, type, 4)){
      *len = READ_U32_LE(buf+4);
      if(fseek(in, *len, SEEK_CUR))
        return 0;

      buf[4] = 0;
    }else{
      *len = READ_U32_LE(buf+4);
      return 1;
    }
  }
}

static pcm_t *wav_load(char *path, FILE *in){
  unsigned char buf[40];
  unsigned int len;
  pcm_t *pcm = NULL;
  int i;

  if(fseek(in,12,SEEK_SET)==-1){
    fprintf(stderr,"%s: Failed to seek\n",path);
    goto err;
  }

  pcm = calloc(1,sizeof(pcm_t));
  pcm->name=strdup(trim_path(path));

  if(!find_wav_chunk(in, path, "fmt ", &len)){
    fprintf(stderr,"%s: Failed to find fmt chunk in WAV file\n",path);
    goto err;
  }

  if(len < 16){
    fprintf(stderr, "%s: Unrecognised format chunk in WAV header\n",path);
    goto err;
  }

  if(sb_verbose){
    /* A common error is to have a format chunk that is not 16, 18 or
     * 40 bytes in size.  This is incorrect, but not fatal, so we only
     * warn about it instead of refusing to work with the file.
     * Please, if you have a program that's creating format chunks of
     * sizes other than 16 or 18 bytes in size, report a bug to the
     * author.
     */
    if(len!=16 && len!=18 && len!=40)
      fprintf(stderr,
              "%s: INVALID format chunk in WAV header.\n"
              " Trying to read anyway (may not work)...\n",path);
  }

  if(len>40)len=40;

  if(fread(buf,1,len,in) < len){
    fprintf(stderr,"%s: Unexpected EOF in reading WAV header\n",path);
    goto err;
  }

  unsigned int mask = 0;
  unsigned int format =      READ_U16_LE(buf);
  unsigned int channels =    READ_U16_LE(buf+2);
  unsigned int samplerate =  READ_U32_LE(buf+4);
  //unsigned int bytespersec = READ_U32_LE(buf+8);
  unsigned int align =       READ_U16_LE(buf+12);
  unsigned int samplesize =  READ_U16_LE(buf+14);
  const char *mask_map[32]={
    "L","R","C","LFE", "BL","BR","CL","CR",
    "BC","SL","SR","X", "X","X","X","X",
    "X","X","X","X", "X","X","X","X",
    "X","X","X","X", "X","X","X","X"};

  if(format == 0xfffe){ /* WAVE_FORMAT_EXTENSIBLE */

    if(len<40){
      fprintf(stderr,"%s: Extended WAV format header invalid (too small)\n",path);
      goto err;
    }

    mask = READ_U32_LE(buf+20);
    format = READ_U16_LE(buf+24);
  }

  if(mask==0){
    switch(channels){
    case 1:
      pcm->matrix = strdup("M");
      pcm->mix = strdup("A");
      break;
    case 2:
      pcm->matrix = strdup("L,R");
      pcm->mix = strdup("BC");
      break;
    case 3:
      pcm->matrix = strdup("L,R,C");
      pcm->mix = strdup("BCD");
      break;
    case 4:
      pcm->matrix = strdup("L,R,BL,BR");
      pcm->mix = strdup("BCFG");
      break;
    case 5:
      pcm->matrix = strdup("L,R,C,BL,BR");
      pcm->mix = strdup("BCDFG");
      break;
    case 6:
      pcm->matrix = strdup("L,R,C,LFE,BL,BR");
      pcm->mix = strdup("BCDEFG");
      break;
    case 7:
      pcm->matrix = strdup("L,R,C,LFE,BC,SL,SR");
      pcm->mix = strdup("BCDEJKL");
      break;
    default:
      pcm->matrix = strdup("L,R,C,LFE,BL,BR,SL,SR");
      pcm->mix = strdup("BCDEFGKL");
      break;
    }
  }else{
    int count=0;
    pcm->matrix = calloc(32*4+1,sizeof(char));
    pcm->mix = calloc(33,sizeof(char));
    for(i=0;i<32;i++){
      if(mask&(1<<i)){
        strcat(pcm->matrix,mask_map[i]);
        strcat(pcm->matrix,",");
        pcm->mix[count++]='B'+i;
      }
    }
  }

  if(!find_wav_chunk(in, path, "data", &len)){
    fprintf(stderr,"%s: Failed to find fmt chunk in WAV file\n",path);
    goto err;
  }

  if(sb_verbose){
    if(align != channels * ((samplesize+7)/8)) {
      /* This is incorrect according to the spec. Warn loudly, then ignore
       * this value.
       */
      fprintf(stderr, "%s: WAV 'block alignment' value is incorrect, "
              "ignoring.\n"
              "The software that created this file is incorrect.\n",path);
    }
  }

  if((format==1 && (samplesize == 24 || samplesize == 16 || samplesize == 8)) ||
     (samplesize == 32 && format == 3)){
    /* OK, good - we have a supported format,
       now we want to find the size of the file */
    pcm->rate = samplerate;
    pcm->ch = channels;
    pcm->nativebits = (format==3 ? -samplesize : samplesize);
    pcm->currentbits = -32;

    if(len){
      pcm->size = len;
    }else{
      long pos;
      pos = ftell(in);
      if(fseek(in, 0, SEEK_END) == -1){
        fprintf(stderr,"%s failed to seek: %s\n",path,strerror(errno));
        goto err;
      }else{
        pcm->size = ftell(in) - pos;
        fseek(in,pos, SEEK_SET);
      }
    }

  }else{
    fprintf(stderr,
            "%s: Wav file is unsupported subformat (must be 8,16, or 24-bit PCM\n"
            "or floating point PCM\n",path);
    goto err;
  }

  /* read the samples into memory */
  switch(pcm->nativebits){
  case 8:
    pcm->data = calloc(1,pcm->size*sizeof(float));
    break;
  case 16:
    pcm->data = calloc(1,pcm->size/2*sizeof(float));
    break;
  case 24:
    pcm->data = calloc(1,pcm->size/3*sizeof(float));
    break;
  case -32:
  case 32:
    pcm->data = calloc(1,pcm->size/4*sizeof(float));
    break;
  default:
    /* Won't get here unless the code is modified and the modifier
       misses expanding here and below */
    fprintf(stderr,"%s: Unsupported bit depth\n",path);
    goto err;
  }

  if(pcm->data == NULL){
    fprintf(stderr,"Unable to allocate enough memory to load sample into memory\n");
    goto err;
  }

  {
    off_t j=0,k;
    unsigned char *d = pcm->data;
    float         *f = (float *)pcm->data;

    while(j<pcm->size){
      off_t bytes = (pcm->size-j > 65536 ? 65536 : pcm->size-j);
      if(sb_verbose)
        fprintf(stderr,"\rLoading %s: %ld to go...       ",path,(long)(pcm->size-j));
      j+=bytes=fread(d+j,1,bytes,in);
      if(bytes==0)break;
    }
    if(j<pcm->size){
      if(sb_verbose)
        fprintf(stderr,"\r%s: File ended before declared length (%ld < %ld); continuing...\n",path,(long)j,(long)pcm->size);
      pcm->size=j;
    }

    /* non float must be expanded to float */
    switch(pcm->nativebits){
    case 8:
      k=pcm->size;
      for(j=pcm->size-1;j>=0;j--)
        f[--k] = (int32_t)((d[j]-128)<<24) * (1.f/2147483648.f);
      pcm->size=pcm->size*sizeof(float);
      break;
    case 16:
      k=pcm->size/2;
      for(j=pcm->size-2;j>=0;j-=2)
        f[--k] = (int32_t)((d[j]<<16)|(d[j+1]<<24)) * (1.f/2147483648.f);
      pcm->size=pcm->size/2*sizeof(float);
      break;
    case 24:
      k=pcm->size*4/3;
      for(j=pcm->size-3;j>=0;j-=3)
        f[--k] = (int32_t)((d[j]<<8)|(d[j+1]<<16)|(d[j+2]<<24)) * (1.f/2147483648.f);
      pcm->size=pcm->size/3*sizeof(float);
      break;
    case 32:
      k=pcm->size/4;
      for(j=pcm->size-4;j>=0;j-=4)
        f[--k] = (int32_t)(d[j]|(d[j+1]<<8)|(d[j+2]<<16)|(d[j+3]<<24)) * (1.f/2147483648.f);
      pcm->size=pcm->size/4*sizeof(float);
      break;
    case -32:
      k=pcm->size/4;
      for(j=pcm->size-4;j>=0;j-=4){
        int val=0;
        int mantissa = d[j] | (d[j+1]<<8) | ((d[j+2]&0x7f)<<16) | (1<<23);
        int exponent = 127 - ((d[j+2]>>7) | ((d[j+3]&0x7f)<<1));
        int sign = d[j+3]>>7;
        if(exponent <= 0){
          if(exponent == -128){
            fprintf(stderr,"%s: Input file contains invalid floating point values.\n",pcm->name);
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
        f[--k] = val/8388608.;
      }
      pcm->size=pcm->size/4*sizeof(float);
      break;
    }
  }

  if(sb_verbose)
    fprintf(stderr,"\r%s: loaded.                 \n",path);

  return pcm;
 err:
  free_pcm(pcm);
  return NULL;
}

/* AIFF file support ***********************************************************/

static int find_aiff_chunk(FILE *in, char *path, char *type, unsigned int *len){
  unsigned char buf[8];
  int restarted = 0;

  while(1){
    if(fread(buf,1,8,in)<8){
      if(!restarted) {
        /* Handle out of order chunks by seeking back to the start
         * to retry */
        restarted = 1;
        fseek(in, 12, SEEK_SET);
        continue;
      }
      fprintf(stderr,"%s: Unexpected EOF in AIFF chunk\n",path);
      return 0;
    }

    *len = READ_U32_BE(buf+4);

    if(memcmp(buf,type,4)){
      if((*len) & 0x1)
        (*len)++;

      if(fseek(in,*len,SEEK_CUR))
        return 0;
    }else
      return 1;
  }
}

static double read_IEEE80(unsigned char *buf){
  int s=buf[0]&0xff;
  int e=((buf[0]&0x7f)<<8)|(buf[1]&0xff);
  double f=((unsigned long)(buf[2]&0xff)<<24)|
    ((buf[3]&0xff)<<16)|
    ((buf[4]&0xff)<<8) |
    (buf[5]&0xff);

  if(e==32767){
    if(buf[2]&0x80)
      return HUGE_VAL; /* Really NaN, but this won't happen in reality */
    else{
      if(s)
        return -HUGE_VAL;
      else
        return HUGE_VAL;
    }
  }

  f=ldexp(f,32);
  f+= ((buf[6]&0xff)<<24)|
    ((buf[7]&0xff)<<16)|
    ((buf[8]&0xff)<<8) |
    (buf[9]&0xff);
  return ldexp(f, e-16446);
}

static pcm_t *aiff_load(char *path, FILE *in){
  pcm_t *pcm = NULL;
  int aifc; /* AIFC or AIFF? */
  unsigned int len;
  unsigned char *buffer;
  unsigned char buf2[12];
  int bend = 1;
  int fp = 0;

  if(fseek(in,0,SEEK_SET)==-1){
    fprintf(stderr,"%s: Failed to seek\n",path);
    goto err;
  }
  if(fread(buf2,1,12,in)!=12){
    fprintf(stderr,"%s: Failed to read AIFF header\n",path);
    goto err;
  }

  pcm = calloc(1,sizeof(pcm_t));
  pcm->name=strdup(trim_path(path));

  if(buf2[11]=='C')
    aifc=1;
  else
    aifc=0;

  if(!find_aiff_chunk(in, path, "COMM", &len)){
    fprintf(stderr,"%s: No common chunk found in AIFF file\n",path);
    goto err;
  }

  if(len < 18){
    fprintf(stderr, "%s: Truncated common chunk in AIFF header\n",path);
    goto err;
  }

  buffer = alloca(len);

  if(fread(buffer,1,len,in) < len){
    fprintf(stderr, "%s: Unexpected EOF in reading AIFF header\n",path);
    goto err;
  }

  pcm->ch = READ_U16_BE(buffer);
  pcm->rate = (int)read_IEEE80(buffer+8);
  pcm->nativebits = READ_U16_BE(buffer+6);
  pcm->size = READ_U32_BE(buffer+2)*pcm->ch*((pcm->nativebits+7)/8);
  pcm->currentbits = -32;

  switch(pcm->ch){
  case 1:
    pcm->matrix = strdup("M");
    pcm->mix = strdup("A");
    break;
  case 2:
    pcm->matrix = strdup("L,R");
    pcm->mix = strdup("BC");
    break;
  case 3:
    pcm->matrix = strdup("L,R,C");
    pcm->mix = strdup("BCD");
    break;
  default:
    pcm->matrix = strdup("L,R,BL,BR");
    pcm->mix = strdup("BCFG");
    break;
  }

  if(aifc){
    if(len < 22){
      fprintf(stderr, "%s: AIFF-C header truncated.\n",path);
      goto err;
    }

    if(!memcmp(buffer+18, "NONE", 4) || !memcmp(buffer+18, "twos", 4)){
      bend = 1;
    }else if(!memcmp(buffer+18, "sowt", 4)){
      bend = 0;
    }else if(!memcmp(buffer+18, "fl32", 4)){
      bend = 1;
      fp = 1;
    }else{
      fprintf(stderr, "%s: Can't handle compressed AIFF-C (%c%c%c%c)\n", path,
              *(buffer+18), *(buffer+19), *(buffer+20), *(buffer+21));
      goto err;
    }
  }

  if(!find_aiff_chunk(in, path, "SSND", &len)){
    fprintf(stderr, "%s: No SSND chunk found in AIFF file\n",path);
    goto err;
  }
  if(len < 8) {
    fprintf(stderr,"%s: Corrupted SSND chunk in AIFF header\n",path);
    goto err;
  }

  if(fread(buf2,1,8, in) < 8){
    fprintf(stderr, "%s: Unexpected EOF reading AIFF header\n",path);
    goto err;
  }

  int offset = READ_U32_BE(buf2);

  if(!((fp==0 && (pcm->nativebits==32 ||
                  pcm->nativebits==24 ||
                  pcm->nativebits==16 ||
                  pcm->nativebits==8)) ||
       (fp==1 && pcm->nativebits==32))){
    fprintf(stderr,
            "%s: Unsupported type of AIFF/AIFC file\n"
            " Must be 8-, 16-, 24- or 32-bit integer or 32-bit floating point PCM.\n",
            path);
    goto err;
  }
  if(fp==1)
    pcm->nativebits = -pcm->nativebits;

  fseek(in, offset, SEEK_CUR); /* Swallow some data */

  /* read the samples into memory */
  switch(pcm->nativebits){
  case 8:
    pcm->data = calloc(1,pcm->size*sizeof(float));
    break;
  case 16:
    pcm->data = calloc(1,pcm->size/2*sizeof(float));
    break;
  case 24:
    pcm->data = calloc(1,pcm->size/3*sizeof(float));
    break;
  case 32:
  case -32:
    pcm->data = calloc(1,pcm->size/4*sizeof(float));
    break;
  }

  if(pcm->data == NULL){
    fprintf(stderr,"Unable to allocate enough memory to load sample into memory\n");
    goto err;
  }

  {
    unsigned char *d = pcm->data;
    float *f = (float *)pcm->data;
    off_t j=0,k;
    while(j<pcm->size){
      off_t bytes = (pcm->size-j > 65536 ? 65536 : pcm->size-j);
      if(sb_verbose)
        fprintf(stderr,"\rLoading %s: %ld to go...       \r",path,(long)(pcm->size-j));
      j+=bytes=fread(d+j,1,bytes,in);
      if(bytes==0)break;
    }
    if(j<pcm->size){
      if(sb_verbose)
        fprintf(stderr,"\r%s: File ended before declared length (%ld < %ld); continuing...\n",path,(long)j,(long)pcm->size);
      pcm->size=j;
    }

    /* expand to float */
    switch(pcm->nativebits){
    case 8:
      k=pcm->size;
      for(j=pcm->size-1;j>=0;j--)
        f[--k] = (int32_t)((d[j]-128)<<24) * (1.f/2147483648.f);
      pcm->size=pcm->size*sizeof(float);
      break;
    case 16:
      k=pcm->size/2;
      if(bend){
        for(j=pcm->size-2;j>=0;j-=2)
          f[--k] = (int32_t)((d[j]<<24)|(d[j+1]<<16)) * (1.f/2147483648.f);
      }else{
        for(j=pcm->size-2;j>=0;j-=2)
          f[--k] = (int32_t)((d[j]<<16)|(d[j+1]<<24)) * (1.f/2147483648.f);
      }
      pcm->size=pcm->size/2*sizeof(float);
      break;
    case 24:
      k=pcm->size/3;
      if(bend){
        for(j=pcm->size-3;j>=0;j-=3)
          f[--k] = (int32_t)(d[j+2]|(d[j+1]<<8)|(d[j]<<16)) * (1.f/2147483648.f);
      }else{
        for(j=pcm->size-3;j>=0;j-=3)
          f[--k] = (int32_t)(d[j]|(d[j+1]<<8)|(d[j+2]<<16)) * (1.f/2147483648.f);
      }
      pcm->size=pcm->size/3*sizeof(float);
      break;
    case 32:
      k=pcm->size/4;
      if(bend){
        for(j=pcm->size-4;j>=0;j-=4)
          f[--k] = (int32_t)(d[j+3]|(d[j+2]<<8)|(d[j+1]<<16)|(d[j]<<24)) * (1.f/2147483648.f);
      }else{
        for(j=pcm->size-4;j>=0;j-=4)
          f[--k] = (int32_t)(d[j]|(d[j+1]<<8)|(d[j+2]<<16)|(d[j+3]<<24)) * (1.f/2147483648.f);
      }
      pcm->size=pcm->size/4*sizeof(float);
      break;
    case -32:
      k=pcm->size/4;
      for(j=pcm->size-4;j>=0;j-=4){
        int val=0;
        int mantissa;
        int exponent;
        int sign;

        if(bend){
          mantissa = d[j+3] | (d[j+2]<<8) | ((d[j+1]&0x7f)<<16) | (1<<23);
          exponent = 127 - ((d[j+1]>>7) | ((d[j]&0x7f)<<1));
          sign = d[j]>>7;
        }else{
          mantissa = d[j] | (d[j+1]<<8) | ((d[j+2]&0x7f)<<16) | (1<<23);
          exponent = 127 - ((d[j+2]>>7) | ((d[j+3]&0x7f)<<1));
          sign = d[j+3]>>7;
        }

        if(exponent <= 0){
          if(exponent == -128){
            fprintf(stderr,"%s: Input file contains invalid floating point values.\n",pcm->name);
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
        f[--k] = val/8388608.;
      }
      pcm->size=pcm->size/4*sizeof(float);
      break;
    }
  }

  if(sb_verbose)
    fprintf(stderr,"\r%s: loaded.                 \n",path);

  return pcm;
 err:
  free_pcm(pcm);
  return NULL;

}

/* SW loading to make JM happy *******************************************************/

static pcm_t *sw_load(char *path, FILE *in){
  pcm_t *pcm = calloc(1,sizeof(pcm_t));
  pcm->name=strdup(trim_path(path));
  pcm->nativebits=16;
  pcm->currentbits=-32;
  pcm->ch=1;
  pcm->rate=48000;
  pcm->matrix=strdup("M");
  pcm->mix=strdup("A");

  if(fseek(in,0,SEEK_END)==-1){
    fprintf(stderr,"%s: Failed to seek\n",path);
    goto err;
  }
  pcm->size=ftell(in);
  if(pcm->size==-1 || fseek(in,0,SEEK_SET)==-1){
    fprintf(stderr,"%s: Failed to seek\n",path);
    goto err;
  }

  pcm->data = calloc(1,pcm->size/2*sizeof(float));

  if(pcm->data == NULL){
    fprintf(stderr,"Unable to allocate enough memory to load sample into memory\n");
    goto err;
  }

  {
    off_t j=0;
    int16_t *d = (int16_t *)pcm->data;
    float   *f = (float *)pcm->data;

    while(j<pcm->size){
      off_t bytes = (pcm->size-j > 65536 ? 65536 : pcm->size-j);
      if(sb_verbose)
        fprintf(stderr,"\rLoading %s: %ld to go...       ",path,(long)(pcm->size-j));
      j+=bytes=fread(pcm->data+j,1,bytes,in);
      if(bytes==0)break;
    }
    if(j<pcm->size){
      if(sb_verbose)
        fprintf(stderr,"\r%s: File ended before declared length (%ld < %ld); continuing...\n",path,(long)j,(long)pcm->size);
      pcm->size=j;
    }

    for(j=pcm->size/2-1;j>=0;j--)
      f[j] = d[j]/32768.;

    pcm->size=pcm->size/2*sizeof(float);
  }

  if(sb_verbose)
    fprintf(stderr,"\r%s: loaded.                 \n",path);

  return pcm;
 err:
  free_pcm(pcm);
  return NULL;
}

/* FLAC and OggFLAC load support *****************************************************************/

typedef struct {
  FILE *in;
  pcm_t *pcm;
  off_t fill;
} flac_callback_arg;

/* glorified fread wrapper */
static FLAC__StreamDecoderReadStatus read_callback(const FLAC__StreamDecoder *decoder,
                                            FLAC__byte buffer[],
                                            size_t *bytes,
                                            void *client_data){
  flac_callback_arg *flac = (flac_callback_arg *)client_data;
  pcm_t *pcm = flac->pcm;

  if(feof(flac->in)){
    *bytes = 0;
    return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
  }else if(ferror(flac->in)){
    *bytes = 0;
    return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
  }

  if(sb_verbose)
    fprintf(stderr,"\rLoading %s: %ld to go...       ",flac->pcm->name,(long)(pcm->size-flac->fill));
  *bytes = fread(buffer, sizeof(FLAC__byte), *bytes, flac->in);

  return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderWriteStatus write_callback(const FLAC__StreamDecoder *decoder,
                                              const FLAC__Frame *frame,
                                              const FLAC__int32 *const buffer[],
                                              void *client_data){
  flac_callback_arg *flac = (flac_callback_arg *)client_data;
  pcm_t *pcm = flac->pcm;
  int samples = frame->header.blocksize;
  int channels = frame->header.channels;
  int bits_per_sample = frame->header.bits_per_sample;
  off_t fill = flac->fill;
  int i, j;

  if(pcm->data == NULL){
    /* lazy initialization */
    pcm->ch = channels;
    pcm->nativebits = (bits_per_sample+7)/8*8;
    pcm->size *= channels*sizeof(float);
    pcm->currentbits = -32;
    pcm->data = calloc(pcm->size,1);
  }

  if(channels != pcm->ch){
    fprintf(stderr,"\r%s: number of channels changes part way through file\n",pcm->name);
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if(pcm->nativebits != (bits_per_sample+7)/8*8){
    fprintf(stderr,"\r%s: bit depth changes part way through file\n",pcm->name);
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }

  {
    float *d = (float *)pcm->data;
    int shift = pcm->nativebits - bits_per_sample;
    switch(pcm->nativebits){
    case 16:
      for (j = 0; j < samples; j++)
        for (i = 0; i < channels; i++)
          d[fill++] = (buffer[i][j]<<shift)*(1.f/32768.f);
      break;
    case 24:
      for (j = 0; j < samples; j++)
        for (i = 0; i < channels; i++)
          d[fill++] = (buffer[i][j]<<shift)*(1.f/8388608.f);
      break;
    default:
      fprintf(stderr,"\r%s: Only 16- and 24-bit FLACs are supported for decode right now.\n",pcm->name);
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
  }
  flac->fill=fill;

  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void metadata_callback(const FLAC__StreamDecoder *decoder,
                       const FLAC__StreamMetadata *metadata,
                       void *client_data){
  flac_callback_arg *flac = (flac_callback_arg *)client_data;
  pcm_t *pcm = flac->pcm;

  switch (metadata->type){
  case FLAC__METADATA_TYPE_STREAMINFO:
    pcm->size = metadata->data.stream_info.total_samples; /* temp setting */
    pcm->rate = metadata->data.stream_info.sample_rate;
    break;
  default:
    break;
  }
}

static void error_callback(const FLAC__StreamDecoder *decoder,
                    FLAC__StreamDecoderErrorStatus status,
                    void *client_data){

  flac_callback_arg *flac = (flac_callback_arg *)client_data;
  pcm_t *pcm = flac->pcm;
  fprintf(stderr,"\r%s: Error decoding file.\n",pcm->name);
}

static FLAC__bool eof_callback(const FLAC__StreamDecoder *decoder,
                        void *client_data){
  flac_callback_arg *flac = (flac_callback_arg *)client_data;
  return feof(flac->in)? true : false;
}

static pcm_t *flac_load_i(char *path, FILE *in, int oggp){
  pcm_t *pcm;
  flac_callback_arg *flac;
  FLAC__StreamDecoder *decoder;
  FLAC__bool ret;

  if(fseek(in,0,SEEK_SET)==-1){
    fprintf(stderr,"%s: Failed to seek\n",path);
    goto err;
  }

  pcm = calloc(1,sizeof(pcm_t));
  flac = calloc(1,sizeof(flac_callback_arg));
  decoder = FLAC__stream_decoder_new();
  FLAC__stream_decoder_set_md5_checking(decoder, true);
  FLAC__stream_decoder_set_metadata_respond(decoder, FLAC__METADATA_TYPE_STREAMINFO);

  pcm->name=strdup(trim_path(path));
  flac->in=in;
  flac->pcm=pcm;

  if(oggp)
    FLAC__stream_decoder_init_ogg_stream(decoder,
                                         read_callback,
                                         /*seek_callback=*/0,
                                         /*tell_callback=*/0,
                                         /*length_callback=*/0,
                                         eof_callback,
                                         write_callback,
                                         metadata_callback,
                                         error_callback,
                                         flac);
  else
    FLAC__stream_decoder_init_stream(decoder,
                                     read_callback,
                                     /*seek_callback=*/0,
                                     /*tell_callback=*/0,
                                     /*length_callback=*/0,
                                     eof_callback,
                                     write_callback,
                                     metadata_callback,
                                     error_callback,
                                     flac);

  /* setup and sample reading handled by configured callbacks */
  ret=FLAC__stream_decoder_process_until_end_of_stream(decoder);
  FLAC__stream_decoder_finish(decoder);
  FLAC__stream_decoder_delete(decoder);
  free(flac);
  if(!ret){
    free_pcm(pcm);
    return NULL;
  }

  /* set channel matrix */
  switch(pcm->ch){
  case 1:
    pcm->matrix = strdup("M");
    pcm->mix = strdup("A");
    break;
  case 2:
    pcm->matrix = strdup("L,R");
    pcm->mix = strdup("BC");
    break;
  case 3:
    pcm->matrix = strdup("L,R,C");
    pcm->mix = strdup("BCD");
    break;
  case 4:
    pcm->matrix = strdup("L,R,BL,BR");
    pcm->mix = strdup("BCFG");
    break;
  case 5:
    pcm->matrix = strdup("L,R,C,BL,BR");
    pcm->mix = strdup("BCDFG");
    break;
  case 6:
    pcm->matrix = strdup("L,R,C,LFE,BL,BR");
    pcm->mix = strdup("BCDEFG");
    break;
  case 7:
    pcm->matrix = strdup("L,R,C,LFE,BC,SL,SR");
    pcm->mix = strdup("BCDEJKL");
    break;
  default:
    pcm->matrix = strdup("L,R,C,LFE,BL,BR,SL,SR");
    pcm->mix = strdup("BCDEFGKL");
    break;
  }

  if(sb_verbose)
    fprintf(stderr,"\r%s: loaded.                 \n",path);
  return pcm;
 err:
  return NULL;
}

static pcm_t *flac_load(char *path, FILE *in){
  return flac_load_i(path,in,0);
}

static pcm_t *oggflac_load(char *path, FILE *in){
  return flac_load_i(path,in,1);
}

/* Vorbis load support **************************************************************************/
static pcm_t *vorbis_load(char *path, FILE *in){
  OggVorbis_File vf;
  vorbis_info *vi=NULL;
  pcm_t *pcm=NULL;
  off_t fill=0;
  int throttle=0;
  int last_section=-1;

  memset(&vf,0,sizeof(vf));

  if(fseek(in,0,SEEK_SET)==-1){
    fprintf(stderr,"%s: Failed to seek\n",path);
    goto err;
  }

  if(ov_open_callbacks(in, &vf, NULL, 0, OV_CALLBACKS_NOCLOSE) < 0) {
    fprintf(stderr,"Input does not appear to be an Ogg bitstream.\n");
    goto err;
  }

  vi=ov_info(&vf,-1);
  pcm = calloc(1,sizeof(pcm_t));
  pcm->name=strdup(trim_path(path));
  pcm->nativebits=-32;
  pcm->currentbits=-32;
  pcm->ch=vi->channels;
  pcm->rate=vi->rate;
  pcm->size=ov_pcm_total(&vf,-1)*vi->channels*sizeof(float);
  pcm->data=calloc(pcm->size,1);

  switch(pcm->ch){
  case 1:
    pcm->matrix = strdup("M");
    pcm->mix = strdup("A");
    break;
  case 2:
    pcm->matrix = strdup("L,R");
    pcm->mix = strdup("BC");
    break;
  case 3:
    pcm->matrix = strdup("L,C,R");
    pcm->mix = strdup("BDC");
    break;
  case 4:
    pcm->matrix = strdup("L,R,BL,BR");
    pcm->mix = strdup("BCFG");
    break;
  case 5:
    pcm->matrix = strdup("L,C,R,BL,BR");
    pcm->mix = strdup("BDCFG");
    break;
  case 6:
    pcm->matrix = strdup("L,C,R,BL,BR,LFE");
    pcm->mix = strdup("BDCFGE");
    break;
  case 7:
    pcm->matrix = strdup("L,C,R,SL,SR,BC,LFE");
    pcm->mix = strdup("BDCKLJE");
    break;
  default:
    pcm->matrix = strdup("L,C,R,SL,SR,BL,BR,LFE");
    pcm->mix = strdup("BDCKLFGE");
    break;
  }

  while(fill*sizeof(float)<pcm->size){
    int current_section;
    int i,j;
    float **pcmout;
    long ret=ov_read_float(&vf,&pcmout,4096,&current_section);
    float *d = (float *)pcm->data;

    if(current_section!=last_section){
      last_section=current_section;
      vi=ov_info(&vf,-1);
      if(vi->channels != pcm->ch || vi->rate!=pcm->rate){
        fprintf(stderr,"%s: Chained file changes channel count/sample rate\n",path);
        goto err;
      }
    }

    if(ret<0){
      fprintf(stderr,"%s: Error while decoding file\n",path);
      goto err;
    }
    if(ret==0){
      fprintf(stderr,"%s: Audio data ended prematurely\n",path);
      goto err;
    }

    for(i=0;i<ret;i++)
      for(j=0;j<pcm->ch;j++)
        d[fill++]=pcmout[j][i];

    if (sb_verbose && (throttle&0x3f)==0)
      fprintf(stderr,"\rLoading %s: %ld to go...       ",pcm->name,(long)(pcm->size-fill));
    throttle++;
  }
  ov_clear(&vf);

  if(sb_verbose)
    fprintf(stderr,"\r%s: loaded.                 \n",path);
  return pcm;
 err:
  ov_clear(&vf);
  free_pcm(pcm);
  return NULL;
}

/* Opus load support **************************************************************************/

int opc_read(void *_stream,unsigned char *_ptr,int _nbytes){
  return fread(_ptr,1,_nbytes,_stream);
}

int opc_seek(void *_stream,opus_int64 _offset,int _whence){
  return fseek(_stream,_offset,_whence);
}

opus_int64 opc_tell(void *_stream){
  return ftell(_stream);
}

int opc_close(void *_stream){
  return 0;
}

static OpusFileCallbacks opus_callbacks =
  { opc_read,opc_seek,opc_tell,opc_close };

static pcm_t *opus_load(char *path, FILE *in){
  OggOpusFile *of;
  pcm_t *pcm=NULL;
  off_t fill=0;
  int throttle=0;
  int last_section=-1;

  if(fseek(in,0,SEEK_SET)==-1){
    fprintf(stderr,"%s: Failed to seek\n",path);
    goto err;
  }

  of = op_open_callbacks(in, &opus_callbacks , NULL, 0, NULL);
  if(!of){
    fprintf(stderr,"Input does not appear to be an Opus bitstream.\n");
    goto err;
  }

  pcm = calloc(1,sizeof(pcm_t));
  pcm->name=strdup(trim_path(path));
  pcm->nativebits=-32;
  pcm->currentbits=-32;
  pcm->ch=op_channel_count(of,-1);
  pcm->rate=48000;
  pcm->size=op_pcm_total(of,-1)*pcm->ch*sizeof(float);
  pcm->data=calloc(pcm->size,1);


  switch(pcm->ch){
  case 1:
    pcm->matrix = strdup("M");
    pcm->mix = strdup("A");
    break;
  case 2:
    pcm->matrix = strdup("L,R");
    pcm->mix = strdup("BC");
    break;
  case 3:
    pcm->matrix = strdup("L,C,R");
    pcm->mix = strdup("BDC");
    break;
  case 4:
    pcm->matrix = strdup("L,R,BL,BR");
    pcm->mix = strdup("BCFG");
    break;
  case 5:
    pcm->matrix = strdup("L,C,R,BL,BR");
    pcm->mix = strdup("BDCFG");
    break;
  case 6:
    pcm->matrix = strdup("L,C,R,BL,BR,LFE");
    pcm->mix = strdup("BDCFGE");
    break;
  case 7:
    pcm->matrix = strdup("L,C,R,SL,SR,BC,LFE");
    pcm->mix = strdup("BDCKLJE");
    break;
  default:
    pcm->matrix = strdup("L,C,R,SL,SR,BL,BR,LFE");
    pcm->mix = strdup("BDCKLFGE");
    break;
  }

  while(fill*sizeof(float)<pcm->size){
    int current_section;
    int i,j;
    float pcmout[4096];
    long ret=op_read_float(of,pcmout,4096,&current_section);
    float *d = (float *)pcm->data;
    float *s = pcmout;

    if(current_section!=last_section){
      last_section=current_section;
      if(op_channel_count(of,-1) != pcm->ch){
        fprintf(stderr,"%s: Chained file changes channel count\n",path);
        goto err;
      }
    }

    if(ret<0){
      fprintf(stderr,"%s: Error while decoding file\n",path);
      goto err;
    }
    if(ret==0){
      fprintf(stderr,"%s: Audio data ended prematurely\n",path);
      goto err;
    }

    for(i=0;i<ret;i++)
      for(j=0;j<pcm->ch;j++)
        d[fill++]=*s++;

    if (sb_verbose && (throttle&0x3f)==0)
      fprintf(stderr,"\rLoading %s: %ld to go...       ",pcm->name,(long)(pcm->size-fill));
    throttle++;
  }
  op_free(of);

  if(sb_verbose)
    fprintf(stderr,  "\r%s: loaded.                          \n",path);
  return pcm;
 err:
  op_free(of);
  free_pcm(pcm);
  return NULL;
}

#define MAX_ID_LEN 36
unsigned char buf[MAX_ID_LEN];

/* Define the supported formats here */
static input_format formats[] = {
  {wav_id,     wav_load,    "wav"},
  {aiff_id,    aiff_load,   "aiff"},
  {flac_id,    flac_load,   "flac"},
  {oggflac_id, oggflac_load,"oggflac"},
  {vorbis_id,  vorbis_load, "oggvorbis"},
  {opus_id,    opus_load,   "oggopus"},
  {sw_id,      sw_load,     "sw"},
  {NULL,       NULL,        NULL}
};

pcm_t *load_audio_file(char *path){
  FILE *f = fopen(path,"rb");
  int j=0;
  int fill;

  if(!f){
    fprintf(stderr,"Unable to open file %s: %s\n",path,strerror(errno));
    return NULL;
  }

  fill = fread(buf, 1, MAX_ID_LEN, f);
  if(fill<MAX_ID_LEN){
    fprintf(stderr,"%s: Input file truncated or NULL\n",path);
    fclose(f);
    return NULL;
  }

  while(formats[j].id_func){
    if(formats[j].id_func(path,buf)){
      pcm_t *ret=formats[j].load_func(path,f);
      fclose(f);
      return ret;
    }
    j++;
  }
  fprintf(stderr,"%s: Unrecognized file format\n",path);
  return NULL;
}

void free_pcm(pcm_t *pcm){
  if(pcm){
    if(pcm->name)free(pcm->name);
    if(pcm->matrix)free(pcm->matrix);
    if(pcm->mix)free(pcm->mix);
    if(pcm->data)free(pcm->data);
    memset(pcm,0,sizeof(pcm));
    free(pcm);
  }
}

