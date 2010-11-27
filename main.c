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
#include "mincurses.h"
#include "main.h"

#define MAXFILES 10
static int verbose=0;

static inline int host_is_big_endian() {
  union {
    int32_t pattern;
    unsigned char bytewise[4];
  } m;
  m.pattern = 0xfeedface; /* deadbeef */
  if (m.bytewise[0] == 0xfe) return 1;
  return 0;
}

void free_pcm(pcm_t *pcm){
  if(pcm){
    if(pcm->path)free(pcm->path);
    if(pcm->matrix)free(pcm->matrix);
    if(pcm->data)free(pcm->data);
    memset(pcm,0,sizeof(pcm));
    free(pcm);
  }
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

int wav_id(char *path,unsigned char *buf){
  if(memcmp(buf, "RIFF", 4))
    return 0; /* Not wave */
  if(memcmp(buf+8, "WAVE",4))
    return 0; /* RIFF, but not wave */
  return 1;
}

int aiff_id(char *path,unsigned char *buf){
  if(memcmp(buf, "FORM", 4))
    return 0;
  if(memcmp(buf+8, "AIF",3))
    return 0;
  if(buf[11]!='C' && buf[11]!='F')
    return 0;
  return 1;
}

int flac_id(char *path,unsigned char *buf){
  return memcmp(buf, "fLaC", 4) == 0;
}

int oggflac_id(char *path,unsigned char *buf){
  return memcmp(buf, "OggS", 4) == 0 &&
    (memcmp (buf+28, "\177FLAC", 5) == 0 ||
     flac_id(path,buf+28));
}

int vorbis_id(char *path,unsigned char *buf){
  return memcmp(buf, "OggS", 4) == 0 &&
    memcmp (buf+28, "\x01vorbis", 7);
}

int sw_id(char *path,unsigned char *buf){
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

pcm_t *wav_load(char *path, FILE *in){
  unsigned char buf[40];
  unsigned int len;
  pcm_t *pcm = NULL;
  int i;

  if(fseek(in,12,SEEK_SET)==-1){
    fprintf(stderr,"%s: Failed to seek\n",path);
    goto err;
  }

  pcm = calloc(1,sizeof(pcm_t));
  pcm->path=strdup(path);

  if(!find_wav_chunk(in, path, "fmt ", &len)){
    fprintf(stderr,"%s: Failed to find fmt chunk in WAV file\n",path);
    goto err;
  }

  if(len < 16){
    fprintf(stderr, "%s: Unrecognised format chunk in WAV header\n",path);
    goto err;
  }

  if(verbose){
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
      break;
    case 2:
      pcm->matrix = strdup("L,R");
      break;
    case 3:
      pcm->matrix = strdup("L,R,C");
      break;
    case 4:
      pcm->matrix = strdup("L,R,BL,BR");
      break;
    case 5:
      pcm->matrix = strdup("L,R,C,BL,BR");
      break;
    case 6:
      pcm->matrix = strdup("L,R,C,LFE,BL,BR");
      break;
    case 7:
      pcm->matrix = strdup("L,R,C,LFE,BC,SL,SR");
      break;
    default:
      pcm->matrix = strdup("L,R,C,LFE,BL,BR,SL,SR");
      break;
    }
  }else{
    pcm->matrix = calloc(32*4,sizeof(char));
    for(i=0;i<32;i++){
      if(mask&(1<<i)){
        strcat(pcm->matrix,mask_map[i]);
        strcat(pcm->matrix,",");
      }
    }
    pcm->matrix[strlen(pcm->matrix)-1]=0;
  }

  if(!find_wav_chunk(in, path, "data", &len)){
    fprintf(stderr,"%s: Failed to find fmt chunk in WAV file\n",path);
    goto err;
  }

  if(verbose){
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
    pcm->bits = (format==3 ? -samplesize : samplesize);

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
  switch(pcm->bits){
  case 8:
    /* load as 8-bit, expand it out to 16. */
    pcm->data = calloc(1,pcm->size*2);
    break;
  case 24:
    pcm->dither = 1;
  case 16:
    pcm->data = calloc(1,pcm->size);
    break;
  case -32:
    pcm->data = calloc(1,pcm->size);
    pcm->dither = 1;
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
    off_t j=0;
    while(j<pcm->size){
      off_t bytes = (pcm->size-j > 65536 ? 65536 : pcm->size-j);
      if(verbose)
        fprintf(stderr,"\rLoading %s: %ld to go...       ",path,(long)(pcm->size-j));
      j+=bytes=fread(pcm->data+j,1,bytes,in);
      if(bytes==0)break;
    }
    if(j<pcm->size){
      if(verbose)
        fprintf(stderr,"\r%s: File ended before declared length (%ld < %ld); continuing...\n",path,(long)j,(long)pcm->size);
      pcm->size=j;
    }

    /* 8-bit must be expanded to 16 */
    if(samplesize==8){
      off_t j;
      unsigned char *d = pcm->data;
      for(j=pcm->size-1;j>=0;j--){
        int val = (d[j]-128)<<8;
        d[j*2] = val&0xff;
        d[j*2+1] = (val>>8)&0xff;
      }
      pcm->bits=16;
      pcm->size*=2;
    }
  }

  if(verbose)
    fprintf(stderr,"\r%s: loaded.                 \n",path);

  return pcm;
 err:
  free_pcm(pcm);
  return NULL;
}

/* AIFF file support ***********************************************************/

int find_aiff_chunk(FILE *in, char *path, char *type, unsigned int *len){
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

double read_IEEE80(unsigned char *buf){
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

static inline void swap(unsigned char *a, unsigned char *b){
  unsigned char temp=*a;
  *a=*b;
  *b=temp;
}

pcm_t *aiff_load(char *path, FILE *in){
  pcm_t *pcm = NULL;
  int aifc; /* AIFC or AIFF? */
  unsigned int len;
  unsigned char *buffer;
  unsigned char buf2[12];
  int bend = 1;

  if(fseek(in,0,SEEK_SET)==-1){
    fprintf(stderr,"%s: Failed to seek\n",path);
    goto err;
  }
  if(fread(buf2,1,12,in)!=12){
    fprintf(stderr,"%s: Failed to read AIFF header\n",path);
    goto err;
  }

  pcm = calloc(1,sizeof(pcm_t));
  pcm->path=strdup(path);

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
  pcm->bits = READ_U16_BE(buffer+6);
  pcm->size = READ_U32_BE(buffer+2)*pcm->ch*((pcm->bits+7)/8);

  switch(pcm->ch){
  case 1:
    pcm->matrix = strdup("M");
    break;
  case 2:
    pcm->matrix = strdup("L,R");
    break;
  case 3:
    pcm->matrix = strdup("L,R,C");
    break;
  default:
    pcm->matrix = strdup("L,R,BL,BR");
    break;
  }

  if(aifc){
    if(len < 22){
      fprintf(stderr, "%s: AIFF-C header truncated.\n",path);
      goto err;
    }

    if(!memcmp(buffer+18, "NONE", 4)){
      bend = 1;
    }else if(!memcmp(buffer+18, "sowt", 4)){
      bend = 0;
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
  int blocksize = READ_U32_BE(buf2+4);

  if( blocksize != 0 ||
      !(pcm->bits==24 || pcm->bits == 16 || pcm->bits == 8)){
    fprintf(stderr,
            "%s: Unsupported type of AIFF/AIFC file\n"
            " Must be 8-, 16- or 24-bit integer PCM.\n",path);
    goto err;
  }

  fseek(in, offset, SEEK_CUR); /* Swallow some data */

  /* read the samples into memory */
  switch(pcm->bits){
  case 8:
    /* load as 8-bit, expand it out to 16. */
    pcm->data = calloc(1,pcm->size*2);
    break;
  case 24:
    pcm->dither = 1;
    /* fall through */
  default:
    pcm->data = calloc(1,pcm->size);
    break;
  }

  if(pcm->data == NULL){
    fprintf(stderr,"Unable to allocate enough memory to load sample into memory\n");
    goto err;
  }

  {
    unsigned char *d = pcm->data;
    off_t j=0;
    while(j<pcm->size){
      off_t bytes = (pcm->size-j > 65536 ? 65536 : pcm->size-j);
      if(verbose)
        fprintf(stderr,"\rLoading %s: %ld to go...       \r",path,(long)(pcm->size-j));
      j+=bytes=fread(d+j,1,bytes,in);
      if(bytes==0)break;
    }
    if(j<pcm->size){
      if(verbose)
        fprintf(stderr,"\r%s: File ended before declared length (%ld < %ld); continuing...\n",path,(long)j,(long)pcm->size);
      pcm->size=j;
    }

    /* 8-bit must be expanded to 16 */
    switch(pcm->bits){
    case 8:
      for(j=pcm->size-1;j>=0;j--){
        int val = d[j]<<8;
        d[j*2] = val&0xff;
        d[j*2+1] = (val>>8)&0xff;
      }
      pcm->bits=16;
      pcm->size*=2;
      break;
    case 16:
      if(bend){
        for(j=0;j<pcm->size/2;j++)
          swap(d+j*2,d+j*2+1);
      }
      break;
    case 24:
      if(bend){
        for(j=0;j<pcm->size/3;j++)
          swap(d+j*3,d+j*3+2);
      }
      break;
    }
  }

  if(verbose)
    fprintf(stderr,"\r%s: loaded.                 \n",path);

  return pcm;
 err:
  free_pcm(pcm);
  return NULL;

}

/* SW loading to make JM happy *******************************************************/

pcm_t *sw_load(char *path, FILE *in){

  pcm_t *pcm = calloc(1,sizeof(pcm_t));
  pcm->path=strdup(path);
  pcm->bits=16;
  pcm->ch=1;
  pcm->rate=48000;
  pcm->matrix=strdup("M");

  if(fseek(in,0,SEEK_END)==-1){
    fprintf(stderr,"%s: Failed to seek\n",path);
    goto err;
  }
  pcm->size=ftell(in);
  if(pcm->size==-1 || fseek(in,0,SEEK_SET)==-1){
    fprintf(stderr,"%s: Failed to seek\n",path);
    goto err;
  }

  pcm->data = calloc(1,pcm->size);

  if(pcm->data == NULL){
    fprintf(stderr,"Unable to allocate enough memory to load sample into memory\n");
    goto err;
  }

  {
    off_t j=0;
    while(j<pcm->size){
      off_t bytes = (pcm->size-j > 65536 ? 65536 : pcm->size-j);
      if(verbose)
        fprintf(stderr,"\rLoading %s: %ld to go...       ",path,(long)(pcm->size-j));
      j+=bytes=fread(pcm->data+j,1,bytes,in);
      if(bytes==0)break;
    }
    if(j<pcm->size){
      if(verbose)
        fprintf(stderr,"\r%s: File ended before declared length (%ld < %ld); continuing...\n",path,(long)j,(long)pcm->size);
      pcm->size=j;
    }
  }

  if(verbose)
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
FLAC__StreamDecoderReadStatus read_callback(const FLAC__StreamDecoder *decoder,
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

  if(verbose)
    fprintf(stderr,"\rLoading %s: %ld to go...       ",flac->pcm->path,(long)(pcm->size-flac->fill));
  *bytes = fread(buffer, sizeof(FLAC__byte), *bytes, flac->in);

  return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderWriteStatus write_callback(const FLAC__StreamDecoder *decoder,
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
    /* lasy initialization */
    pcm->ch = channels;
    pcm->bits = (bits_per_sample+7)/8*8;
    pcm->size *= pcm->bits/8*channels;
    pcm->data = calloc(pcm->size,1);
  }

  if(channels != pcm->ch){
    fprintf(stderr,"\r%s: number of channels changes part way through file\n",pcm->path);
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if(pcm->bits != (bits_per_sample+7)/8*8){
    fprintf(stderr,"\r%s: bit depth changes part way through file\n",pcm->path);
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }

  {
    unsigned char *d = pcm->data + fill;
    int shift = pcm->bits - bits_per_sample;
    switch(pcm->bits){
    case 16:
      for (j = 0; j < samples; j++)
        for (i = 0; i < channels; i++){
          d[0] = (buffer[i][j]<<shift)&0xff;
          d[1] = (buffer[i][j]<<shift>>8)&0xff;
          d+=2;
          fill+=2;
        }
      break;
    case 24:
      pcm->dither = 1;
      for (j = 0; j < samples; j++)
        for (i = 0; i < channels; i++){
          d[0] = (buffer[i][j]<<shift)&0xff;
          d[1] = (buffer[i][j]<<shift>>8)&0xff;
          d[3] = (buffer[i][j]<<shift>>16)&0xff;
          d+=3;
          fill+=3;
        }
      break;
    default:
      fprintf(stderr,"\r%s: Only 16- and 24-bit FLACs are supported for decode right now.\n",pcm->path);
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
  }
  flac->fill=fill;

  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void metadata_callback(const FLAC__StreamDecoder *decoder,
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

void error_callback(const FLAC__StreamDecoder *decoder,
                    FLAC__StreamDecoderErrorStatus status,
                    void *client_data){

  flac_callback_arg *flac = (flac_callback_arg *)client_data;
  pcm_t *pcm = flac->pcm;
  fprintf(stderr,"\r%s: Error decoding file.\n",pcm->path);
}

FLAC__bool eof_callback(const FLAC__StreamDecoder *decoder,
                        void *client_data){
  flac_callback_arg *flac = (flac_callback_arg *)client_data;
  return feof(flac->in)? true : false;
}

pcm_t *flac_load_i(char *path, FILE *in, int oggp){
  pcm_t *pcm = calloc(1,sizeof(pcm_t));
  flac_callback_arg *flac = calloc(1,sizeof(flac_callback_arg));
  FLAC__StreamDecoder *decoder = FLAC__stream_decoder_new();
  FLAC__bool ret;
  FLAC__stream_decoder_set_md5_checking(decoder, true);
  FLAC__stream_decoder_set_metadata_respond(decoder, FLAC__METADATA_TYPE_STREAMINFO);
  pcm->path=strdup(path);
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
    break;
  case 2:
    pcm->matrix = strdup("L,R");
    break;
  case 3:
    pcm->matrix = strdup("L,R,C");
    break;
  case 4:
    pcm->matrix = strdup("L,R,BL,BR");
    break;
  case 5:
    pcm->matrix = strdup("L,R,C,BL,BR");
    break;
  case 6:
    pcm->matrix = strdup("L,R,C,LFE,BL,BR");
    break;
  case 7:
    pcm->matrix = strdup("L,R,C,LFE,BC,SL,SR");
    break;
  default:
    pcm->matrix = strdup("L,R,C,LFE,BL,BR,SL,SR");
    break;
  }

  if(verbose)
    fprintf(stderr,"\r%s: loaded.                 \n",path);
  return pcm;
}

pcm_t *flac_load(char *path, FILE *in){
  return flac_load_i(path,in,0);
}

pcm_t *oggflac_load(char *path, FILE *in){
  return flac_load_i(path,in,1);
}

/* Vorbis load support **************************************************************************/
pcm_t *vorbis_load(char *path, FILE *in){
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
  pcm->path=strdup(path);
  pcm->bits=-32;
  pcm->ch=vi->channels;
  pcm->rate=vi->rate;
  pcm->size=ov_pcm_total(&vf,-1)*vi->channels*4;
  pcm->data=calloc(pcm->size,1);

  switch(pcm->ch){
  case 1:
    pcm->matrix = strdup("M");
    break;
  case 2:
    pcm->matrix = strdup("L,R");
    break;
  case 3:
    pcm->matrix = strdup("L,C,R");
    break;
  case 4:
    pcm->matrix = strdup("L,R,BL,BR");
    break;
  case 5:
    pcm->matrix = strdup("L,C,R,BL,BR");
    break;
  case 6:
    pcm->matrix = strdup("L,C,R,BL,BR,LFE");
    break;
  case 7:
    pcm->matrix = strdup("L,C,R,SL,SR,BC,LFE");
    break;
  default:
    pcm->matrix = strdup("L,C,R,SL,SR,BL,BR,LFE");
    break;
  }

  while(fill<pcm->size){
    int current_section;
    int i,j;
    float **pcmout;
    long ret=ov_read_float(&vf,&pcmout,4096,&current_section);
    unsigned char *d = pcm->data+fill;

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

    if(sizeof(float)==4){
      /* Assuming IEEE754, which is pedantically incorrect */
      union {
        float f;
        unsigned char c[4];
      } m;
      if(host_is_big_endian()){
        for(i=0;i<ret;i++){
          for(j=0;j<pcm->ch;j++){
            m.f=pcmout[j][i];
            d[0] = m.c[3];
            d[1] = m.c[2];
            d[2] = m.c[1];
            d[3] = m.c[0];
            d+=4;
          }
        }
      }else{
        for(i=0;i<ret;i++){
          for(j=0;j<pcm->ch;j++){
            m.f=pcmout[j][i];
            d[0] = m.c[0];
            d[1] = m.c[1];
            d[2] = m.c[2];
            d[3] = m.c[3];
            d+=4;
          }
        }
      }
    }else{
      fprintf(stderr,"Unhandled case: sizeof(float)!=4\n");
      exit(10);
    }
    fill += ret*pcm->ch*3;
    if (verbose && (throttle&0x3f)==0)
      fprintf(stderr,"\rLoading %s: %ld to go...       ",pcm->path,(long)(pcm->size-fill));
    throttle++;
  }
  ov_clear(&vf);

  if(verbose)
    fprintf(stderr,"\r%s: loaded.                 \n",path);
  return pcm;
 err:
  ov_clear(&vf);
  free_pcm(pcm);
  return NULL;
}

#define MAX_ID_LEN 35
unsigned char buf[MAX_ID_LEN];

/* Define the supported formats here */
input_format formats[] = {
  {wav_id,     wav_load,    "wav"},
  {aiff_id,    aiff_load,   "aiff"},
  {flac_id,    flac_load,   "flac"},
  {oggflac_id, oggflac_load,"oggflac"},
  {vorbis_id,  vorbis_load, "oggvorbis"},
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
  return NULL;
}

/* sample formatting helpers ********************************************************/

void float32_to_24(pcm_t *pcm){
  unsigned char *d = pcm->data;
  off_t j;
  if(verbose)
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
  if(verbose)
    fprintf(stderr,"...done.\n");
  pcm->bits=24;
  pcm->size/=4;
  pcm->size*=3;
}

static inline float triangle_ditherval(float *save){
  float r = rand()/(float)RAND_MAX-.5f;
  float ret = *save-r;
  *save = r;
  return ret;
}

void float32_to_16(pcm_t *pcm){
  unsigned char *d = pcm->data;
  off_t j;
  union {
    float f;
    unsigned char c[4];
  } m;

  if(verbose)
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

  if(verbose)
    fprintf(stderr,"...done.\n");

  pcm->bits=16;
  pcm->size/=2;
}

void demote_24_to_16(pcm_t *pcm){
  float t[pcm->ch];
  unsigned char *d = pcm->data;
  off_t i;
  int ch=0;

  if(verbose)
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

  if(verbose)
    fprintf(stderr,"...done.\n");

  pcm->bits=16;
  pcm->size/=3;
  pcm->size*=2;
}

void promote_to_24(pcm_t *pcm){
  off_t i;

  if(verbose)
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
  if(verbose)
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

const char *chlist[]={"X","M","L","R","C","LFE","SL","SR","BC","BL","BR","CL","CR",NULL};
void tokenize_channels(char *matrix,int *out,int n){
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
  if(ret && verbose)
    fprintf(stderr,"Opened %s%s audio device %s%sfor %d bit %d channel %d Hz...\n",
            (device?"":"default "),aname,
            (device?device:""),(device?" ":""),bits,ch,rate);

  return ret;
}

struct option long_options[] = {
  {"ab",no_argument,0,'a'},
  {"abx",no_argument,0,'b'},
  {"beep-flip",no_argument,0,'B'},
  {"casual",no_argument,0,'c'},
  {"device",required_argument,0,'d'},
  {"force-dither",no_argument,0,'D'},
  {"end-time",no_argument,0,'e'},
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
char *short_options="abcd:De:hn:rRs:tvVxBMS";

void usage(FILE *out){
  fprintf(out,
          "\nXiph Squishyball %s\n"
          "perform sample comparison testing on the command line\n\n"
          "USAGE:\n"
          "  squishyball [options] fileA [fileB [[-c] fileN...]]\n\n"
          "OPTIONS:\n"
          "  -a --ab                : Perform randomized A/B test\n"
          "  -b --abx               : Perform randomized A/B/X test\n"
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
          "                           Vorbis playback; all other files\n"
          "                           a dithered by default during down-\n"
          "                           conversion.\n"
          "  -e --end-time <time>   : Set sample end time for playback\n"
          "  -h --help              : Print this usage information.\n"
          "  -M --mark-flip         : Mark transitions between samples with\n"
          "                           a short period of silence\n"
          "  -n --trials <n>        : Set desired number of trials\n"
          "                           (default: 10)\n"
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
          "  -x --xxy               : Perform randomized X/X/Y test.\n"
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
          "  AIFF and AIFC    : 8-, 16-, 24-bit linear integer PCM\n"
          "  FLAC and OggFLAC : 16- and 24-bit\n"
          "  SW               : mono signed 16-bit little endian raw\n"
          "  OggVorbis        : all Vorbis I files\n"
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

float *fadewindow1;
float *fadewindow2;
float *fadewindow3;
float *beep1;
float *beep2;

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

int setup_windows(pcm_t **pcm, int test_files){
  int i;
  int fragsamples = pcm[0]->rate/10;  /* 100ms */
  float mul = (pcm[0]->bits==24 ? 8388608.f : 32768.f) * .0625;
  /* precompute the fades/beeps */
  fadewindow1 = calloc(fragsamples,sizeof(*fadewindow1));
  fadewindow2 = calloc(fragsamples,sizeof(*fadewindow2));
  fadewindow3 = calloc(fragsamples,sizeof(*fadewindow3));
  beep1 = calloc(fragsamples,sizeof(*beep1));
  beep2 = calloc(fragsamples,sizeof(*beep2));

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

  /* fadewindow3 crossfades with attenuation to give headroom for a beep */
  for(i=0;i<fragsamples/4;i++){
    float val = cosf(M_PI*2.f*(i+.5f)/fragsamples);
    fadewindow3[i] = val*val*.875f+.125f;
  }
  for(;i<fragsamples*3/4;i++)
    fadewindow3[i] = .125f;
  for(;i<fragsamples;i++){
    float val = cosf(M_PI*2.f*(i-fragsamples*3/4+.5f)/fragsamples);
    fadewindow3[i] = val*val*.125f;
  }

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

  /* make sure that the samples are at least fragsamples*2 in length! If they're not, extend... */
  if(pcm[0]->size<fragsamples*2){
    int fadesize = pcm[0]->size/4;
    int bps = (pcm[0]->bits+7)/8;
    int ch = pcm[0]->ch;
    int bpf = bps*ch;

    for(i=0;i<test_files;i++){
      int j,k;
      unsigned char *newd=calloc(fadesize,bpf);
      if(!newd){
        fprintf(stderr,"Unable to allocate memory to extend sample to minimum length.\n");
        exit(5);
      }
      memcpy(newd,pcm[i]->data,fragsamples*2*bpf);
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
      pcm[i]->size=fragsamples*2;
    }
  }
  return fragsamples;
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
} threadstate_t;

/* playback is a degenerate thread that simply allows audio output
   without blocking */
void *playback_thread(void *arg){
  threadstate_t *s = (threadstate_t *)arg;
  ao_device *adev = s->adev;

  pthread_mutex_lock(&s->mutex);
  while(1){
    if(s->exiting){
      pthread_mutex_unlock(&s->mutex);
      break;
    }
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
      if(s->exiting){
        pthread_mutex_unlock(&s->mutex);
        break;
      }
    }

    pthread_cond_wait(&s->play_cond,&s->mutex);
  }
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
    int ret;
    if(s->exiting){
      pthread_mutex_unlock(&s->mutex);
      break;
    }
    if(s->key_waiting==0){
      pthread_mutex_unlock(&s->mutex);
      ret=min_getch(0);
      pthread_mutex_lock(&s->mutex);
    }
    if(s->exiting){
      pthread_mutex_unlock(&s->mutex);
      break;
    }
    if(ret!=ERR){
      s->key_waiting=ret;
      pthread_cond_signal(&s->main_cond);
      pthread_cond_wait(&s->key_cond,&s->mutex);
    }
  }
  return NULL;
}

/* fragment is filled such that a crossloop never begins after
   pcm->size-fragsize, and it always begins from the start of the
   window, even if that means starting a crossloop late because the
   endpos moved. */
void fill_fragment1(unsigned char *out, pcm_t *pcm, off_t start, off_t *pos, off_t end, int *loop,int fragsamples){
  int bps = (pcm->bits+7)/8;
  int cpf = pcm->ch;
  int bpf = bps*cpf;
  int fragsize = fragsamples*bpf;

  /* guard limits here */
  if(end<fragsize)end=fragsize;
  if(end>pcm->size)end=pcm->size;
  if(start<0)start=0;
  if(start>pcm->size-fragsize)start=pcm->size-fragsize;

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
        float w = fadewindow1[--lp];
        for(j=0;j<cpf;j++){
          float val = get_val(A,bps)*(1.-w) + get_val(B,bps)*w;
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
    }else if(*pos+fragsize>=end-fragsize){
      int i,j;
      unsigned char *A = pcm->data+*pos;
      unsigned char *B = pcm->data+start;
      int lp = (end-*pos)/bpf;
      if(lp<fragsamples)lp=fragsamples; /* If we're late, start immediately, but use full window */

      for(i=0;i<fragsamples;i++){
        if(--lp>fragsamples){
          /* still before crossloop begins */
          memcpy(out,A,bpf);
          A+=bpf;
          out+=bpf;
        }else{
          /* crosslooping */
          float w = fadewindow1[lp];
          for(j=0;j<cpf;j++){
            float val = get_val(A,bps)*(1.-w) + get_val(B,bps)*w;
            put_val(out,val,bps);
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
      *pos=A-pcm->data+fragsize;
    }
  }
}

/* fragment is filled such that a crossloop is always 'exactly on
   schedule' even if that means beginning partway through the window. */
void fill_fragment2(unsigned char *out, pcm_t *pcm, off_t start, off_t *pos, off_t end, int *loop,int fragsamples){
  int bps = (pcm->bits+7)/8;
  int cpf = pcm->ch;
  int bpf = bps*cpf;
  int fragsize=fragsamples*bpf;

  /* guard limits here */
  if(end<fragsize)end=fragsize;
  if(end>pcm->size)end=pcm->size;
  if(start<0)start=0;
  if(start>pcm->size-fragsize)start=pcm->size-fragsize;

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
        float w = fadewindow1[lp];
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

void randomize_samples(int *r,int *cchoice, int test_mode){
  switch(test_mode){
  case 1:
    r[0] = random()&1;
    r[1] = 1-r[0];
    r[2] = random()&1;
    *cchoice = (r[1]==r[2] ? 1 : 0);
    break;
  case 0:
    r[0] = random()&1;
    r[1] = 1-r[0];
    *cchoice = 1;
    break;
  case 2:
    r[0] = random()&1;
    r[1] = random()&1;
    if(r[0] == r[1])
      r[2]=1-r[0];
    else
      r[2] = random()&1;
    break;
    *cchoice = (r[0]==r[1] ? 2 : (r[1]==r[2] ? 0 : 1));
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

int main(int argc, char **argv){
  int fragsamples;
  int fragsize;
  unsigned char *fragmentA;
  unsigned char *fragmentB;
  pthread_t playback_handle;
  pthread_t fd_handle;
  threadstate_t state;
  int c,long_option_index;
  pcm_t *pcm[MAXFILES];
  int test_mode=3;
  int test_files;
  char *device=NULL;
  int force_dither=0;
  int force_truncate=0;
  int restart_mode=0;
  int beep_mode=3;
  int tests=10;
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
      break;
    case 's':
      parse_time(optarg,&start);
      break;
    case 'r':
      restart_mode=1;
      break;
    case 'R':
      restart_mode=2;
      break;
    case 'v':
      verbose=1;
      break;
    case 'V':
      fprintf(stdout,"%s\n",VERSION);
      exit(0);
    default:
      usage(stderr);
      exit(1);
    }
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

  outbits=16;
  for(i=0;i<test_files;i++){
    pcm[i]=load_audio_file(argv[optind+i]);
    if(!pcm[i])exit(2);

    if(!pcm[i]->dither && force_dither)pcm[i]->dither=1;
    if(pcm[i]->bits!=16 && force_truncate)pcm[i]->dither=0;

    /* Are all samples the same rate?  If not, bail. */
    if(pcm[0]->rate != pcm[i]->rate){
      fprintf(stderr,"Input sample rates do not match!\n"
              "\t%s: %dHz\n"
              "\t%s: %dHz\n"
              "Aborting\n",pcm[0]->path,pcm[0]->rate,pcm[i]->path,pcm[i]->rate);
      exit(3);
    }

    /* Are all samples the same number of channels?  If not, bail. */
    if(pcm[0]->ch != pcm[i]->ch){
      fprintf(stderr,"Input channel counts do not match!\n"
              "\t%s: %dHz\n"
              "\t%s: %dHz\n"
              "Aborting\n",pcm[0]->path,pcm[0]->ch,pcm[i]->path,pcm[i]->ch);
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
        if(verbose)
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
      if(verbose)
        fprintf(stderr,"Input sample lengths do not match!\n");

      for(i=0;i<test_files;i++){
        if(verbose)
        fprintf(stderr,"\t%s: %s\n",pcm[i]->path,
                make_time_string((double)pcm[i]->size/pcm[i]->ch/((pcm[i]->bits+7)/8)/pcm[i]->rate,0));
        pcm[i]->size=n;
      }
      if(verbose)
        fprintf(stderr,"Using the shortest sample for playback length...\n");
    }
  }

  /* set up various transition windows/beeps */
  fragsamples=setup_windows(pcm,test_files);

  /* set up terminal */
  atexit(min_panel_remove);
  {
    double len=pcm[0]->size/((pcm[0]->bits+7)/8)/pcm[0]->ch/(double)pcm[0]->rate;
    panel_init(pcm, test_files, test_mode, start, end>0 ? end : len, len,
               beep_mode, restart_mode, tests, "");
  }

  /* set up shared state */
  memset(&state,0,sizeof(state));
  pthread_mutex_init(&state.mutex,NULL);
  pthread_cond_init(&state.main_cond,NULL);
  pthread_cond_init(&state.play_cond,NULL);
  pthread_cond_init(&state.key_cond,NULL);
  state.adev=adev;

  /* fire off helper threads */
  if(pthread_create(&playback_handle,NULL,playback_thread,&state)){
    fprintf(stderr,"Failed to create playback thread.\n");
    exit(7);
  }
  if(pthread_create(&fd_handle,NULL,key_thread,&state)){
    fprintf(stderr,"Failed to create playback thread.\n");
    exit(7);
  }

  /* casual mode is not randomized */
  for(i=0;i<MAXFILES;i++)
    randomize[i]=i;
  /* randomize samples for first trial */
  srandom(time(NULL)+getpid());
  randomize_samples(randomize,&cchoice,test_mode);

  /* playback loop */
  pthread_mutex_lock(&state.mutex);
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

    fragsize=fragsamples*bpf;
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

    while(1){

      int c;
      if(state.exiting) break;

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
        case '0': case '9': case '8': case '7': case '6':
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
          seek_to=start_pos;
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
          if(current_pos<end_pos)
            start_pos=current_pos;
          break;
        case 'S':
          start_pos=0;
          break;
        case 'e':
          if(current_pos>start_pos)
            end_pos=current_pos;
          break;
        case 'E':
          end_pos=pcm[0]->size;
          break;
        case '?':
          panel_toggle_keymap();
          break;
        case 331:
          if(tests_cursor<tests_total)
            tests_cursor++;
          break;
        case 330:
          if(tests_cursor>0)
            tests_cursor--;
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
        double base = 1.f/(rate*bpf);
        double current = current_pos*base;
        double start = start_pos*base;
        double len = pcm[0]->size*base;
        double end = end_pos>0?end_pos*base:len;

        pthread_mutex_unlock(&state.mutex);
        panel_update_start(start);
        panel_update_current(current);
        panel_update_end(end);
        panel_update_pause(paused);
        panel_update_playing(current_choice);
        panel_update_repeat_mode(restart_mode);
        panel_update_flip_mode(beep_mode);
        panel_update_trials(choice_list,tests_cursor);
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
          current_choice=0;

          if(tests_cursor==tests){
            pthread_mutex_lock(&state.mutex);
            state.exiting=1;
            pthread_mutex_unlock(&state.mutex);
            break;
          }
        }

        if(paused){
          current_sample=randomize[current_choice];
          memset(fragmentA,0,fragsize);
          if(do_seek){
            current_pos+=seek_to;
            seek_to=0;
            do_seek=0;
            loop=0;
          }
        }else{
          fill_fragment1(fragmentA, pcm[current_sample], start_pos, &current_pos, end_pos, &loop, fragsamples);
          if(do_flip || do_seek || do_select){
            current_sample=randomize[current_choice];
            if(do_seek){
              current_pos=save_pos+seek_to;
              fill_fragment2(fragmentB, pcm[current_sample], start_pos, &current_pos, end_pos, &loop, fragsamples);
              seek_to=0;
            }else{
              fill_fragment1(fragmentB, pcm[current_sample], start_pos, &save_pos, end_pos, &save_loop, fragsamples);
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
              break;
            case 2:
              wA=fadewindow3;
              beep=beep1;
              break;
            case 3:
              wA=fadewindow1;
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

      /* wait */
      if(!state.key_waiting && state.fragment_size>0)
        pthread_cond_wait(&state.main_cond,&state.mutex);
    }
  }

  /* tear down terminal */
  min_panel_remove();

  /* join */
  pthread_cond_signal(&state.play_cond);
  pthread_cancel(fd_handle);
  pthread_mutex_unlock(&state.mutex);

  if(test_mode!=3 && tests_cursor>0){
    int total1=0;
    tests=tests_cursor;
    for(i=0;i<tests;i++)
      total1+=sample_list[i];

    switch(test_mode){
    case 0:
      fprintf(stdout, "\nA/B test results:\n");
      fprintf(stdout, "\tSample 1 (%s) chosen %d/%d trials.\n",pcm[0]->path,tests-total1,tests);
      fprintf(stdout, "\tSample 2 (%s) chosen %d/%d trials.\n",pcm[1]->path,total1,tests);
      break;
    case 1:
      fprintf(stdout, "\nA/B/X test results:\n");
      fprintf(stdout, "\tCorrect sample identified %d/%d trials.\n",total1,tests);
      break;
    case 2:
      fprintf(stdout, "\nX/X/Y test results:\n");
      fprintf(stdout, "\tCorrect sample identified %d/%d trials.\n",total1,tests);
      break;
    }

    if(test_mode==1 || test_mode==2){
      // 0.5^20*(20!/(17!*3!))
      double p=0;
      for(i=total1;i<=tests;i++)
        p += pow(.5,tests) * (factorial(tests) / (factorial(tests-i)*factorial(i)));
      fprintf(stdout, "\tProbability of %d or better correct via random chance: %.2f%%\n",total1,p*100);
      if(p<.05)
        fprintf(stdout,"\tStatistically significant result (>=95%% confidence)\n");
    }
    fprintf(stdout,"\n");
  }

  pthread_join(playback_handle,NULL);
  pthread_join(fd_handle,NULL);
  for(i=0;i<test_files;i++)
    free_pcm(pcm[i]);
  return 0;
}

