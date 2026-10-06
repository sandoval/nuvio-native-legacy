#include "auddecode.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#ifndef NV_DTS_FFMPEG
struct AudDecode { int unused; };
AudDecode *auddecode_create(void) { return calloc(1,sizeof(AudDecode)); }
int auddecode_available(void) { return 0; }
int auddecode_open(AudDecode *d,const char *u,const char *h,int t,double s,double n) {
  (void)d;(void)u;(void)h;(void)t;(void)s;(void)n;return -1;
}
int auddecode_next(AudDecode *d,AudDecodePcm *p) { (void)d;(void)p;return -1; }
const char *auddecode_error(AudDecode *d) { (void)d;return "Auxiliary audio decoder not built"; }
void auddecode_cancel(AudDecode *d) { (void)d; }
void auddecode_destroy(AudDecode *d) { free(d); }
void auddecode_metrics(AudDecode *d,AudDecodeMetrics *m) { (void)d;if(m) memset(m,0,sizeof(*m)); }
#else
#include "rede.h"
#include <errno.h>
#include <limits.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include "mediaio_ffmpeg.h"
#define RANGE_BYTES (1024*1024)
#define PCM_CAP 32768
struct AudDecode {
  char error[160], *url, *headers, *cache;
  volatile int cancelled;
  int64_t pos,total,cache_start;
  long cache_size;
  RedeRangeBudget budget;
  unsigned long begun_ms;
  AVFormatContext *fmt;
  AVIOContext *io;
  AVCodecContext *dec;
  AVPacket *packet;
  AVFrame *frame;
  SwrContext *swr;
  int stream, eof, draining, finished, timeline;
  int rate;
  enum AVSampleFormat format;
  AVChannelLayout layout;
  int64_t first_sample, end_sample, next_sample;
  int16_t pcm[PCM_CAP];
};
static unsigned long now_ms(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return (unsigned long)t.tv_sec*1000+(unsigned long)t.tv_nsec/1000000;
}
static int stopped(void *opaque) {
  AudDecode *d=opaque;
  return __atomic_load_n(&d->cancelled,__ATOMIC_RELAXED) || now_ms()>=d->budget.deadline_ms;
}
static int fail(AudDecode *d,const char *message) {
  if (!d->error[0]) snprintf(d->error,sizeof d->error,"%s",message);
  return -1;
}
static int read_range(void *opaque,uint8_t *out,int count) {
  AudDecode *d=opaque;
  if(stopped(d)) { fail(d,"Audio analysis cancelled or time budget exhausted");return AVERROR_EXIT; }
  if(d->total>=0 && d->pos>=d->total) return AVERROR_EOF;
  if(!d->cache || d->pos<d->cache_start || d->pos-d->cache_start>=d->cache_size) {
    uint64_t remain=d->budget.max_body_bytes-d->budget.body_bytes;
    if(d->budget.body_bytes>=d->budget.max_body_bytes) { fail(d,"Audio source byte budget exhausted");return AVERROR_EXIT; }
    int64_t bytes=remain<RANGE_BYTES?(int64_t)remain:RANGE_BYTES;
    if(d->pos>INT64_MAX-bytes) return AVERROR(EOVERFLOW);
    int status=0; int64_t total=-1; long size=0;
    free(d->cache);d->cache=NULL;d->cache_size=0;
    d->cache=rede_baixar_trecho64_budget(d->url,d->headers,d->pos,d->pos+bytes-1,
      &size,&total,&status,&d->cancelled,&d->budget);
    if(!d->cache || size<=0 || status!=206 || total<=0) {
      fail(d,stopped(d)?"Audio analysis cancelled or time budget exhausted":
        d->budget.body_bytes>=d->budget.max_body_bytes?"Audio source byte budget exhausted":
        status==200?"Source ignores HTTP Range":
        status==401||status==403?"Audio source authorization failed":"Audio source requires validated seekable HTTP ranges");
      return AVERROR(EIO);
    }
    if(d->total>=0 && total!=d->total) { fail(d,"Audio source changed during analysis");return AVERROR(EIO); }
    d->total=total;d->cache_start=d->pos;d->cache_size=size;
  }
  int64_t available=d->cache_size-(d->pos-d->cache_start);
  if(count>available) count=(int)available;
  memcpy(out,d->cache+(size_t)(d->pos-d->cache_start),count);d->pos+=count;
  return count;
}
static int64_t seek_range(void *opaque,int64_t offset,int whence) {
  AudDecode *d=opaque;
  if(stopped(d)) return AVERROR_EXIT;
  if(whence==AVSEEK_SIZE) return d->total>=0?d->total:AVERROR(ENOSYS);
  whence &= ~AVSEEK_FORCE;
  int64_t next;
  int result=mediaio_position(d->pos,d->total,offset,whence,&next);
  if(result<0) return result;
  d->pos=next;return d->pos;
}
static void close_media(AudDecode *d) {
  swr_free(&d->swr);av_channel_layout_uninit(&d->layout);
  av_packet_free(&d->packet);av_frame_free(&d->frame);avcodec_free_context(&d->dec);
  avformat_close_input(&d->fmt);
  if(d->io) { av_freep(&d->io->buffer);avio_context_free(&d->io); }
  free(d->cache);d->cache=NULL;free(d->url);d->url=NULL;free(d->headers);d->headers=NULL;
}
AudDecode *auddecode_create(void) { AudDecode *d=calloc(1,sizeof(*d));if(d) d->total=-1;return d; }
int auddecode_available(void) { return avcodec_find_decoder(AV_CODEC_ID_AAC) && avcodec_find_decoder(AV_CODEC_ID_AC3) && avcodec_find_decoder(AV_CODEC_ID_EAC3); }
static int codec_supported(enum AVCodecID codec) {
  switch(codec) {
    case AV_CODEC_ID_AAC:case AV_CODEC_ID_AC3:case AV_CODEC_ID_EAC3:case AV_CODEC_ID_OPUS:
    case AV_CODEC_ID_PCM_S16LE:case AV_CODEC_ID_PCM_S16BE:case AV_CODEC_ID_PCM_S24LE:case AV_CODEC_ID_PCM_S24BE:
    case AV_CODEC_ID_PCM_S32LE:case AV_CODEC_ID_PCM_S32BE:case AV_CODEC_ID_PCM_F32LE:case AV_CODEC_ID_PCM_F64LE:return 1;
    default:return 0;
  }
}
int auddecode_open(AudDecode *d,const char *url,const char *headers,int track,double start,double duration) {
  if(!d) return -1;
  close_media(d);d->error[0]=0;d->pos=0;d->total=-1;d->cache_size=0;
  d->eof=d->draining=d->finished=d->timeline=0;
  if(__atomic_load_n(&d->cancelled,__ATOMIC_RELAXED)) return fail(d,"Audio analysis cancelled");
  memset(&d->budget,0,sizeof d->budget);d->begun_ms=now_ms();
  d->budget.deadline_ms=d->begun_ms+120000;d->budget.max_body_bytes=64u*1024u*1024u;
  if(!url || (strncmp(url,"http://",7) && strncmp(url,"https://",8))) return fail(d,"Audio analysis supports seekable HTTP(S) MP4/MKV only");
  if(!isfinite(start)||start<0||!isfinite(duration)||duration<=0||duration>300||start>INT64_MAX/16000.0-duration)
    return fail(d,"Invalid audio analysis window");
  d->first_sample=(int64_t)llround(start*16000);d->end_sample=(int64_t)llround((start+duration)*16000);
  d->url=strdup(url);d->headers=strdup(headers?headers:"");
  if(!d->url||!d->headers) return fail(d,"Audio decoder allocation failed");
  /* Explicit demux whitelist: no HLS/DASH/live protocol fallback. */
  int r=mediaio_open(&d->fmt,&d->io,d,read_range,seek_range,stopped,1024*1024,3*AV_TIME_BASE);
  if(r<0) return fail(d,"Unsupported or malformed MP4/MKV audio source");
  if(avformat_find_stream_info(d->fmt,NULL)<0) return fail(d,"Audio track metadata unavailable");
  if(d->fmt->duration==AV_NOPTS_VALUE || d->fmt->duration<=0) return fail(d,"Live or unindexed audio source unavailable");
  int only=-1,audio_count=0;
  for(unsigned i=0;i<d->fmt->nb_streams;i++) if(d->fmt->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_AUDIO) { only=(int)i;audio_count++; }
  if(track < -1) return fail(d,"Invalid selected audio track mapping");
  if(track<0) { if(audio_count!=1) return fail(d,"Selected audio track cannot be mapped unambiguously");track=only; }
  if(track<0 || (unsigned)track>=d->fmt->nb_streams || d->fmt->streams[track]->codecpar->codec_type!=AVMEDIA_TYPE_AUDIO)
    return fail(d,"Selected audio track unavailable");
  d->stream=track;AVStream *stream=d->fmt->streams[track];
  for (unsigned i=0;i<d->fmt->nb_streams;i++)
    if ((int)i != track) d->fmt->streams[i]->discard=AVDISCARD_ALL;
  if(!codec_supported(stream->codecpar->codec_id)) return fail(d,"Selected audio codec is unsupported");
  const AVCodec *codec=avcodec_find_decoder(stream->codecpar->codec_id);
  if(!codec) return fail(d,"Selected audio codec was not built");
  d->dec=avcodec_alloc_context3(codec);d->packet=av_packet_alloc();d->frame=av_frame_alloc();
  if(!d->dec||!d->packet||!d->frame) return fail(d,"Audio decoder allocation failed");
  d->dec->thread_count=1;d->dec->max_samples=65536;d->dec->pkt_timebase=stream->time_base;
  if(avcodec_parameters_to_context(d->dec,stream->codecpar)<0 || avcodec_open2(d->dec,codec,NULL)<0)
    return fail(d,"Audio decoder initialization failed");
  /* Two seconds preroll covers Opus convergence and codec delay; trim by decoded
   * timestamps, never bytes. Small windows starting at zero use full decode. */
  if(start>2) {
    int64_t seek=av_rescale_q((int64_t)((start-2)*AV_TIME_BASE),AV_TIME_BASE_Q,stream->time_base);
    if(avformat_seek_file(d->fmt,track,INT64_MIN,seek,seek,AVSEEK_FLAG_BACKWARD)<0)
      return fail(d,"Audio source index cannot seek to requested window");
    avcodec_flush_buffers(d->dec);
  }
  return 0;
}
static int output(AudDecode *d,int count,int64_t sample,AudDecodePcm *out) {
  d->next_sample=sample+count;
  int skip=0;
  if(sample<d->first_sample) { int64_t n=d->first_sample-sample;skip=n<count?(int)n:count; }
  sample+=skip;count-=skip;
  if(sample>=d->end_sample) { d->finished=1;return 0; }
  if(count>d->end_sample-sample) count=(int)(d->end_sample-sample);
  if(!count) return 2; /* preroll; keep decoding */
  *out=(AudDecodePcm){d->pcm+skip,count,av_rescale_q(sample,(AVRational){1,16000},AV_TIME_BASE_Q)};
  return 1;
}
int auddecode_next(AudDecode *d,AudDecodePcm *out) {
  if(!d||!out||!d->dec||d->error[0]) return -1;
  while(!d->finished) {
    if(stopped(d)) return fail(d,"Audio analysis cancelled or time budget exhausted");
    int r=avcodec_receive_frame(d->dec,d->frame);
    if(r==AVERROR_EOF) {
      if(d->swr) {
        uint8_t *dest=(uint8_t *)d->pcm;
        int count=swr_convert(d->swr,&dest,PCM_CAP,NULL,0);
        if(count<0) return fail(d,"Audio resampler flush failed");
        if(count) { int status=output(d,count,d->next_sample,out);if(status!=2) return status;continue; }
      }
      d->finished=1;return 0;
    }
    if(r==AVERROR(EAGAIN)) {
      if(d->eof) {
        if(d->draining) return fail(d,"Audio decoder stalled during drain");
        if(avcodec_send_packet(d->dec,NULL)<0) return fail(d,"Audio decoder drain failed");
        d->draining=1;continue;
      }
      r=av_read_frame(d->fmt,d->packet);
      if(r==AVERROR_EOF) { d->eof=1;continue; }
      if(r<0) return fail(d,"Malformed audio packets or source read failed");
      if(d->packet->stream_index==d->stream) r=avcodec_send_packet(d->dec,d->packet);
      else r=0;
      av_packet_unref(d->packet);
      if(r<0) return fail(d,"Malformed audio packet");
      continue;
    }
    if(r<0) return fail(d,"Audio decode failed");
    AVFrame *frame=d->frame;
    if(frame->best_effort_timestamp==AV_NOPTS_VALUE || frame->sample_rate<8000 || frame->sample_rate>192000 || frame->nb_samples<=0 || frame->nb_samples>65536)
      return fail(d,"Decoded audio has no reliable timestamps or exceeds frame limit");
    if(!d->swr) {
      AVChannelLayout mono=AV_CHANNEL_LAYOUT_MONO;
      if(frame->ch_layout.nb_channels<=0 || frame->ch_layout.nb_channels>8) return fail(d,"Unsupported audio channel layout");
      if(swr_alloc_set_opts2(&d->swr,&mono,AV_SAMPLE_FMT_S16,16000,&frame->ch_layout,frame->format,frame->sample_rate,0,NULL)<0 || swr_init(d->swr)<0)
        return fail(d,"Audio resampler initialization failed");
      av_channel_layout_copy(&d->layout,&frame->ch_layout);d->rate=frame->sample_rate;d->format=frame->format;
    } else if(d->rate!=frame->sample_rate || d->format!=frame->format || av_channel_layout_compare(&d->layout,&frame->ch_layout))
      return fail(d,"Audio format changed during analysis");
    int64_t delay=swr_get_delay(d->swr,frame->sample_rate);
    int64_t sample=av_rescale_q(frame->best_effort_timestamp,d->fmt->streams[d->stream]->time_base,(AVRational){1,16000})
      -av_rescale_rnd(delay,16000,frame->sample_rate,AV_ROUND_NEAR_INF);
    if(d->timeline) {
      int64_t precision=av_rescale_q(1,d->fmt->streams[d->stream]->time_base,(AVRational){1,16000})+2;
      if(precision<2) precision=2;
      if(precision>64) precision=64;
      if(llabs(sample-d->next_sample)>precision) return fail(d,"Decoded audio timestamp discontinuity");
      sample=d->next_sample;
    } else {
      /* An origin/container without usable cues must not fall back to scanning
       * the entire movie prefix. Allow bounded cluster/keyframe preroll only. */
      if(sample < d->first_sample - 15*16000) return fail(d,"Audio source index requires excessive preroll");
      d->timeline=1;
    }
    if(av_rescale_rnd(delay+frame->nb_samples,16000,frame->sample_rate,AV_ROUND_UP)>PCM_CAP)
      return fail(d,"Audio resampler frame exceeds buffer limit");
    uint8_t *dest=(uint8_t *)d->pcm;
    int count=swr_convert(d->swr,&dest,PCM_CAP,(const uint8_t **)frame->extended_data,frame->nb_samples);
    av_frame_unref(frame);
    if(count<0) return fail(d,"Audio resampling failed");
    int status=output(d,count,sample,out);if(status!=2) return status;
  }
  return 0;
}
const char *auddecode_error(AudDecode *d) { return d?d->error:"Audio decoder allocation failed"; }
void auddecode_cancel(AudDecode *d) { if(d) __atomic_store_n(&d->cancelled,1,__ATOMIC_RELAXED); }
void auddecode_destroy(AudDecode *d) { if(d) { close_media(d);free(d); } }
void auddecode_metrics(AudDecode *d,AudDecodeMetrics *out) {
  if(!out) return;
  *out=(AudDecodeMetrics){0};if(d) *out=(AudDecodeMetrics){
    .source_bytes=d->budget.body_bytes, .range_requests=d->budget.requests,
    .elapsed_seconds=(now_ms()-d->begun_ms)/1000.0,
    .container_start_seconds=d->fmt && d->fmt->start_time!=AV_NOPTS_VALUE ? d->fmt->start_time/(double)AV_TIME_BASE : NAN};
}
#endif
