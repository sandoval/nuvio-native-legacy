#include "dts_engine.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
struct DtsEngine;
#ifndef NV_DTS_FFMPEG
struct DtsEngine { DtsMediaInfo info; };
DtsEngine *dts_engine_create(void) { return calloc(1, sizeof(DtsEngine)); }
int dts_engine_available(void) { return 0; }
void dts_engine_metrics(DtsEngine *e,DtsEngineMetrics *out) { (void)e; if(out) memset(out,0,sizeof(*out)); }
int dts_engine_open(DtsEngine *e,const char *u,const char *h,int a,int c,double s) {
  (void)e;(void)u;(void)h;(void)a;(void)c;(void)s; return -1;
}
const DtsMediaInfo *dts_engine_info(DtsEngine *e) { return e ? &e->info : NULL; }
int dts_engine_has_core(DtsEngine *e) { (void)e; return 0; }
const void *dts_engine_subtitle_parameters(DtsEngine *e,int i) { (void)e;(void)i; return NULL; }
int dts_engine_next(DtsEngine *e,DtsFrame *f) { (void)e;(void)f; return -1; }
int dts_engine_seek(DtsEngine *e,double s) { (void)e;(void)s; return -1; }
void dts_engine_cancel(DtsEngine *e) { (void)e; }
void dts_engine_destroy(DtsEngine *e) { free(e); }
const char *dts_engine_error(DtsEngine *e) { (void)e; return "DTS conversion not built"; }
#else
#include "../rede.h"
#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
#include <libavutil/dovi_meta.h>
#include <libswresample/swresample.h>
#include "../mediaio_ffmpeg.h"
#define RANGE_BYTES (1024 * 1024)
#ifndef RANGE_WORKERS
#define RANGE_WORKERS 4
#endif
#define RANGE_SLOTS 32
typedef struct {
  pthread_t thread;
  struct DtsEngine *engine;
} DtsRangeWorker;
typedef struct {
  int state; /* 0 idle, 1 queued, 2 fetching, 3 complete */
  volatile int cancel;
  int64_t start, end, total;
  char *data;
  long count;
  int status;
  uint64_t elapsed;
} DtsRangeJob;
static const AVRational NS = {1,1000000000};
struct DtsEngine {
  DtsMediaInfo info;
  char error[160], *url, *headers, *cache;
  int64_t pos, total, cache_start;
  long cache_size;
  volatile int cancelled;
  pthread_mutex_t range_lock;
  pthread_cond_t range_wake;
  DtsRangeJob ranges[RANGE_SLOTS];
  DtsRangeWorker workers[RANGE_WORKERS];
  int range_threads, range_stop;
  int64_t range_next;
  long range_bytes;
  AVFormatContext *fmt;
  AVIOContext *io;
  AVCodecContext *dec, *enc;
  AVBSFContext *bsf;
  SwrContext *swr;
  AVAudioFifo *fifo;
  AVPacket *in, *out;
  AVFrame *decoded, *encoded;
  AVChannelLayout input_layout;
  enum AVSampleFormat input_format;
  int input_rate, demux_eof, decoder_eof, resampler_eof, encoder_sent_eof;
  int64_t sample_pts, target_ns;
  int timeline, has_core;
  DtsEngineMetrics metrics;
#ifdef NV_DTS_DEBUG
  DtsEngineMetrics logged_metrics;
  uint64_t logged_at_ns;
#endif
};
static uint64_t clock_ns(clockid_t clock) {
  struct timespec t;
  if(clock_gettime(clock,&t)) return 0;
  return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}
void dts_engine_metrics(DtsEngine *e,DtsEngineMetrics *out) {
  if(out) { if(e) *out=e->metrics; else memset(out,0,sizeof(*out)); }
}
static int fail(DtsEngine *e,const char *msg,int code) {
  char detail[64] = "";
  if (code < 0) av_strerror(code,detail,sizeof(detail));
  snprintf(e->error,sizeof(e->error),"%s%s%s",msg,*detail?": ":"",detail);
  return -1;
}
static int cancelled(void *opaque) {
  return __atomic_load_n(&((DtsEngine *)opaque)->cancelled,__ATOMIC_RELAXED);
}
/* Persistent HTTP lanes retain independent curl connection caches. The bounded
 * queue keeps fetching while native playback backpressures the demux consumer,
 * so a slow future request can finish before playback needs its bytes. Payload
 * storage is at most 32 speculative 1 MiB ranges plus the 1 MiB active cache
 * (each HTTP allocation also includes one terminating byte). */
static void *range_worker(void *opaque) {
  DtsEngine *e=((DtsRangeWorker *)opaque)->engine;
  pthread_mutex_lock(&e->range_lock);
  for (;;) {
    DtsRangeJob *job=NULL;
    while(!e->range_stop) {
      if(!cancelled(e)) {
        for(int i=0;i<RANGE_SLOTS;i++) {
          DtsRangeJob *candidate=&e->ranges[i];
          if(candidate->state==1 && (!job || candidate->start<job->start)) job=candidate;
        }
      }
      if(job) break;
      pthread_cond_wait(&e->range_wake,&e->range_lock);
    }
    if(e->range_stop) break;
    job->state=2;
    pthread_mutex_unlock(&e->range_lock);
    uint64_t begun=clock_ns(CLOCK_MONOTONIC);
    job->data=rede_baixar_trecho64_cab(e->url,e->headers,job->start,job->end,
      &job->count,&job->total,&job->status,&job->cancel);
    uint64_t ended=clock_ns(CLOCK_MONOTONIC);
    pthread_mutex_lock(&e->range_lock);
    job->elapsed=ended>=begun?ended-begun:0; job->state=3;
    pthread_cond_broadcast(&e->range_wake);
  }
  pthread_mutex_unlock(&e->range_lock); return NULL;
}
static void clear_ranges(DtsEngine *e) {
  pthread_mutex_lock(&e->range_lock);
  for(int i=0;i<RANGE_SLOTS;i++) {
    DtsRangeJob *job=&e->ranges[i];
    __atomic_store_n(&job->cancel,1,__ATOMIC_RELAXED);
    if(job->state==1) job->state=0;
  }
  for(int i=0;i<RANGE_SLOTS;i++) {
    DtsRangeJob *job=&e->ranges[i];
    while(job->state==2) pthread_cond_wait(&e->range_wake,&e->range_lock);
    free(job->data); job->data=NULL; job->state=0;
  }
  e->range_next=e->pos;
  pthread_mutex_unlock(&e->range_lock);
}
static int start_ranges(DtsEngine *e) {
  pthread_mutex_lock(&e->range_lock);
  if(e->range_threads) { pthread_mutex_unlock(&e->range_lock); return 1; }
  for(int i=0;i<RANGE_WORKERS;i++) {
    DtsRangeWorker *worker=&e->workers[i]; worker->engine=e;
    if(pthread_create(&worker->thread,NULL,range_worker,worker)) break;
    e->range_threads++;
  }
  e->range_next=e->pos;
  int ok=e->range_threads>0;
  pthread_mutex_unlock(&e->range_lock); return ok;
}
/* Called under range_lock; only idle slots can receive new work. */
static void queue_ranges(DtsEngine *e) {
  for(int i=0;i<RANGE_SLOTS && !cancelled(e);i++) {
    DtsRangeJob *job=&e->ranges[i];
    if(job->state || (e->total>=0 && e->range_next>=e->total)) continue;
    job->start=e->range_next;
    job->end=job->start>INT64_MAX-e->range_bytes?INT64_MAX:job->start+e->range_bytes-1;
    job->count=0; job->status=0; job->total=-1;
    __atomic_store_n(&job->cancel,0,__ATOMIC_RELAXED);
    job->state=1;
    e->range_next=job->end==INT64_MAX?INT64_MAX:job->end+1;
  }
  pthread_cond_broadcast(&e->range_wake);
}
static int read_range(void *opaque,uint8_t *dst,int n) {
  DtsEngine *e=opaque;
  if (cancelled(e)) return AVERROR_EXIT;
  if (e->total>=0 && e->pos>=e->total) return AVERROR_EOF;
  if (!e->cache || e->pos<e->cache_start || e->pos-e->cache_start>=e->cache_size) {
    uint64_t wait_started=clock_ns(CLOCK_MONOTONIC);
    if(!start_ranges(e)) return AVERROR(ENOMEM);
    pthread_mutex_lock(&e->range_lock);
    DtsRangeJob *job=NULL;
    for(int i=0;i<RANGE_SLOTS;i++)
      if(e->ranges[i].state && e->ranges[i].start==e->pos) job=&e->ranges[i];
    if(!job) {
      pthread_mutex_unlock(&e->range_lock);
      /* Seek or short 206: discard obsolete speculative ranges and restart
       * at the exact next byte. Never jump over a short response's tail. */
      clear_ranges(e);
      pthread_mutex_lock(&e->range_lock); queue_ranges(e);
      for(int i=0;i<RANGE_SLOTS;i++)
        if(e->ranges[i].state && e->ranges[i].start==e->pos) job=&e->ranges[i];
    }
    if(!job) { pthread_mutex_unlock(&e->range_lock); return AVERROR_EXIT; }
    while(job->state!=3 && !cancelled(e)) pthread_cond_wait(&e->range_wake,&e->range_lock);
    if(cancelled(e)) { pthread_mutex_unlock(&e->range_lock); return AVERROR_EXIT; }
    long count=job->count; int status=job->status; int64_t total=job->total, end=job->end;
    uint64_t ready_at=clock_ns(CLOCK_MONOTONIC);
    if(ready_at>=wait_started) e->metrics.range_blocked_ns+=ready_at-wait_started;
    free(e->cache); e->cache=NULL; e->cache_size=0;
    e->cache=job->data; job->data=NULL; job->state=0;
    uint64_t elapsed=job->elapsed;
    e->metrics.range_requests++;
    e->metrics.range_wait_ns+=elapsed;
    if(elapsed>e->metrics.range_max_wait_ns) e->metrics.range_max_wait_ns=elapsed;
    if(count>0) e->metrics.range_bytes+=(uint64_t)count;
    if(e->cache && count>0 && count<end-e->pos+1 &&
        !(total>=0 && e->pos<=total && count>=total-e->pos)) {
      e->metrics.range_short_reads++;
      /* Origins can impose a smaller range size. Match their observed cap
       * after the first short response, avoiding repeated speculative holes. */
      if(count<e->range_bytes) e->range_bytes=count;
    }
    if(total>=0) {
      e->total=total;
      /* The first response discovers EOF after the initial speculative queue. */
      for(int i=0;i<RANGE_SLOTS;i++)
        if(e->ranges[i].state==1 && e->ranges[i].start>=total) e->ranges[i].state=0;
    }
    queue_ranges(e);
    pthread_mutex_unlock(&e->range_lock);
    if (!e->cache || count<=0 || count>RANGE_BYTES || status!=206) {
      free(e->cache); e->cache=NULL;
      if (status==416 && total>=0 && e->pos>=total) { e->total=total; return AVERROR_EOF; }
      return AVERROR(EIO);
    }
    e->total=total; e->cache_start=e->pos; e->cache_size=count;
  }
  int64_t available=e->cache_size-(e->pos-e->cache_start);
  if (n>available) n=(int)available;
  if(e->pos>INT64_MAX-n) return AVERROR(EOVERFLOW);
  memcpy(dst,e->cache+(size_t)(e->pos-e->cache_start),n); e->pos+=n;
  return n;
}
static int64_t seek_range(void *opaque,int64_t offset,int whence) {
  DtsEngine *e=opaque;
  if (cancelled(e)) return AVERROR_EXIT;
  if (whence==AVSEEK_SIZE) return e->total>=0?e->total:AVERROR(ENOSYS);
  whence &= ~AVSEEK_FORCE;
  int64_t next;
  int result=mediaio_position(e->pos,e->total,offset,whence,&next);
  if(result<0) return result;
  e->pos=next;
  if(!e->cache || e->pos<e->cache_start || e->pos-e->cache_start>=e->cache_size)
    clear_ranges(e);
  return e->pos;
}
static int64_t ns(int64_t value,AVRational tb) {
  return value==AV_NOPTS_VALUE?INT64_MIN:av_rescale_q(value,tb,NS);
}
static void copy_metadata(char *out,size_t size,AVDictionary *m,const char *key) {
  AVDictionaryEntry *v=av_dict_get(m,key,NULL,0);
  snprintf(out,size,"%s",v?v->value:"");
}
static void close_media(DtsEngine *e) {
  pthread_mutex_lock(&e->range_lock);
  e->range_stop=1;
  for(int i=0;i<RANGE_SLOTS;i++) __atomic_store_n(&e->ranges[i].cancel,1,__ATOMIC_RELAXED);
  pthread_cond_broadcast(&e->range_wake);
  pthread_mutex_unlock(&e->range_lock);
  for(int i=0;i<e->range_threads;i++) pthread_join(e->workers[i].thread,NULL);
  for(int i=0;i<RANGE_SLOTS;i++) free(e->ranges[i].data);
  pthread_mutex_lock(&e->range_lock);
  memset(e->ranges,0,sizeof e->ranges); e->range_threads=0; e->range_stop=0;
  pthread_mutex_unlock(&e->range_lock);
  av_packet_free(&e->in); av_packet_free(&e->out);
  av_frame_free(&e->decoded); av_frame_free(&e->encoded);
  avcodec_free_context(&e->dec); avcodec_free_context(&e->enc);
  av_bsf_free(&e->bsf); swr_free(&e->swr); av_audio_fifo_free(e->fifo); e->fifo=NULL;
  av_channel_layout_uninit(&e->input_layout);
  avformat_close_input(&e->fmt);
  if (e->io) { av_freep(&e->io->buffer); avio_context_free(&e->io); }
  free(e->cache); e->cache=NULL; free(e->url); e->url=NULL;
  free(e->headers); e->headers=NULL;
}
DtsEngine *dts_engine_create(void) {
  DtsEngine *e=calloc(1,sizeof(*e));
  if(e) { e->total=-1; e->range_bytes=RANGE_BYTES; pthread_mutex_init(&e->range_lock,NULL); pthread_cond_init(&e->range_wake,NULL); }
  return e;
}
int dts_engine_available(void) {
  return avcodec_find_decoder(AV_CODEC_ID_DTS) && avcodec_find_encoder(AV_CODEC_ID_AAC) && av_bsf_get_by_name("h264_mp4toannexb")
      && av_bsf_get_by_name("hevc_mp4toannexb");
}
const DtsMediaInfo *dts_engine_info(DtsEngine *e) { return e?&e->info:NULL; }
int dts_engine_has_core(DtsEngine *e) { return e?e->has_core:0; }
const void *dts_engine_subtitle_parameters(DtsEngine *e,int i) {
  if (!e || !e->fmt || i<0 || (unsigned)i>=e->fmt->nb_streams) return NULL;
  AVCodecParameters *p=e->fmt->streams[i]->codecpar;
  return p->codec_type==AVMEDIA_TYPE_SUBTITLE?p:NULL;
}
const char *dts_engine_error(DtsEngine *e) { return e?e->error:"No DTS engine"; }
void dts_engine_cancel(DtsEngine *e) {
  if(!e) return;
  __atomic_store_n(&e->cancelled,1,__ATOMIC_RELAXED);
  pthread_mutex_lock(&e->range_lock);
  for(int i=0;i<RANGE_SLOTS;i++) __atomic_store_n(&e->ranges[i].cancel,1,__ATOMIC_RELAXED);
  pthread_cond_broadcast(&e->range_wake); pthread_mutex_unlock(&e->range_lock);
}
void dts_engine_destroy(DtsEngine *e) {
  if(e) { close_media(e); pthread_cond_destroy(&e->range_wake); pthread_mutex_destroy(&e->range_lock); free(e); }
}
static int reset_encoder(DtsEngine *e) {
  /* Encoders are reopened after seek; encoder flush is not supported by AAC. */
  enum AVCodecID id=e->enc->codec_id;
  AVChannelLayout layout={0}; av_channel_layout_copy(&layout,&e->enc->ch_layout);
  avcodec_free_context(&e->enc);
  const AVCodec *codec=avcodec_find_encoder(id);
  e->enc=avcodec_alloc_context3(codec);
  if (!e->enc) { av_channel_layout_uninit(&layout); return fail(e,"Audio encoder allocation",0); }
  av_channel_layout_copy(&e->enc->ch_layout,&layout); av_channel_layout_uninit(&layout);
  e->enc->sample_rate=48000; e->enc->time_base=(AVRational){1,48000};
  e->enc->sample_fmt=AV_SAMPLE_FMT_FLTP;
  e->enc->bit_rate=192000;
  e->enc->profile=AV_PROFILE_AAC_LOW;
  int r=avcodec_open2(e->enc,codec,NULL);
  return r<0?fail(e,"Audio encoder open",r):0;
}
int dts_engine_open(DtsEngine *e,const char *url,const char *headers,int audio,int core,double start) {
  if (!e || !url || !*url) return -1;
  close_media(e); memset(&e->info,0,sizeof(e->info)); e->error[0]=0;
  memset(&e->metrics,0,sizeof(e->metrics));
#ifdef NV_DTS_DEBUG
  memset(&e->logged_metrics,0,sizeof(e->logged_metrics)); e->logged_at_ns=0;
#endif
  e->pos=0; e->total=-1; e->cache_size=0; e->has_core=0;
  e->range_bytes=RANGE_BYTES;
  e->demux_eof=e->decoder_eof=e->resampler_eof=e->encoder_sent_eof=e->timeline=0;
  if (cancelled(e)) return fail(e,"Cancelled",0);
  if (audio < -1 || !isfinite(start) || start<0 || start>INT64_MAX/1000000000.0)
    return fail(e,"Invalid DTS conversion options",0);
  e->url=strdup(url); e->headers=strdup(headers?headers:"");
  if (!e->url || !e->headers) return fail(e,"Demux allocation",0);
  /* Never give libavformat the signed URL; custom AVIO is its sole transport.
   * No decoder logs are enabled, including malformed metadata containing URLs. */
  int r=mediaio_open(&e->fmt,&e->io,e,read_range,seek_range,cancelled,4194304,5000000);
  if(r<0) return fail(e,"Container open",r);
  r=avformat_find_stream_info(e->fmt,NULL);
  if(r<0) return fail(e,"Container metadata",r);
  if(e->fmt->duration==AV_NOPTS_VALUE || e->fmt->duration<=0)
    return fail(e,"Finite VOD duration required",0);
  e->info.audio_stream=e->info.video_stream=-1;
  for(unsigned i=0;i<e->fmt->nb_streams;i++) {
    AVStream *s=e->fmt->streams[i]; AVCodecParameters *p=s->codecpar;
    if(av_packet_side_data_get(p->coded_side_data,p->nb_coded_side_data,AV_PKT_DATA_ENCRYPTION_INIT_INFO)
       || p->codec_tag==MKTAG('e','n','c','a') || p->codec_tag==MKTAG('e','n','c','v'))
      return fail(e,"Encrypted media is unsupported",0);
    if(p->codec_type==AVMEDIA_TYPE_VIDEO && e->info.video_stream<0 && !(s->disposition&AV_DISPOSITION_ATTACHED_PIC))
      e->info.video_stream=i;
    if(p->codec_id==AV_CODEC_ID_DTS && e->info.audio_stream<0 && (audio<0 || audio==(int)i)) e->info.audio_stream=i;
    int kind=p->codec_type==AVMEDIA_TYPE_VIDEO?DTS_VIDEO:p->codec_type==AVMEDIA_TYPE_AUDIO?DTS_AUDIO:p->codec_type==AVMEDIA_TYPE_SUBTITLE?DTS_SUBTITLE:0;
    if(kind && e->info.n_tracks<DTS_TRACK_MAX) {
      DtsTrack *t=&e->info.tracks[e->info.n_tracks++]; t->kind=kind; t->stream_index=i; t->stream_id=s->id;
      t->channels=p->ch_layout.nb_channels; t->forced=!!(s->disposition&AV_DISPOSITION_FORCED);
      snprintf(t->codec,sizeof(t->codec),"%s",avcodec_get_name(p->codec_id));
      copy_metadata(t->language,sizeof(t->language),s->metadata,"language");
      copy_metadata(t->title,sizeof(t->title),s->metadata,"title");
    }
  }
  if(e->info.audio_stream<0 || e->info.video_stream<0) return fail(e,"Selected DTS track or video absent",0);
  AVStream *v=e->fmt->streams[e->info.video_stream]; AVCodecParameters *vp=v->codecpar;
  const char *filter=vp->codec_id==AV_CODEC_ID_H264?"h264_mp4toannexb":vp->codec_id==AV_CODEC_ID_HEVC?"hevc_mp4toannexb":NULL;
  if(!filter && vp->codec_id!=AV_CODEC_ID_VP9 && vp->codec_id!=AV_CODEC_ID_AV1)
    return fail(e,"Compressed video codec unsupported",0);
  const AVPacketSideData *dv=av_packet_side_data_get(vp->coded_side_data,vp->nb_coded_side_data,AV_PKT_DATA_DOVI_CONF);
  if(dv && dv->size>=sizeof(AVDOVIDecoderConfigurationRecord)) {
    const AVDOVIDecoderConfigurationRecord *conf=(const AVDOVIDecoderConfigurationRecord *)dv->data;
    e->info.dovi_profile=conf->dv_profile; e->info.dovi_level=conf->dv_level;
    e->info.dovi_rpu_present=conf->rpu_present_flag;
    e->info.dovi_el_present=conf->el_present_flag;
    e->info.dovi_bl_present=conf->bl_present_flag;
    e->info.dovi_bl_compatibility_id=conf->dv_bl_signal_compatibility_id;
    if ((conf->dv_profile!=5 && conf->dv_profile!=8) || conf->el_present_flag
        || !conf->bl_present_flag || !conf->rpu_present_flag || vp->codec_id!=AV_CODEC_ID_HEVC)
      return fail(e,"Dolby Vision layer/profile unsupported",0);
    /* mp4toannexb preserves HEVC RPU NAL units; no video re-encode occurs. */
  }
  e->info.width=vp->width; e->info.height=vp->height;
  snprintf(e->info.video_codec,sizeof(e->info.video_codec),"%s",avcodec_get_name(vp->codec_id));
  e->info.fps_num=v->avg_frame_rate.num; e->info.fps_den=v->avg_frame_rate.den;
  e->info.color_primaries=vp->color_primaries; e->info.color_transfer=vp->color_trc; e->info.color_matrix=vp->color_space;
  snprintf(e->info.hdr,sizeof(e->info.hdr),"%s",e->info.dovi_profile?"DolbyVision":vp->color_trc==AVCOL_TRC_SMPTE2084?"PQ":vp->color_trc==AVCOL_TRC_ARIB_STD_B67?"HLG":"SDR");
  if(filter) {
    const AVBitStreamFilter *video_filter=av_bsf_get_by_name(filter);
    if(!video_filter) return fail(e,"Video filter absent",0);
    r=av_bsf_alloc(video_filter,&e->bsf);
    if(r<0) return fail(e,"Video filter allocation",r);
    avcodec_parameters_copy(e->bsf->par_in,vp); e->bsf->time_base_in=v->time_base;
    if((r=av_bsf_init(e->bsf))<0) return fail(e,"Video filter open",r);
  }
  AVCodecParameters *ap=e->fmt->streams[e->info.audio_stream]->codecpar;
  const AVCodec *decoder=avcodec_find_decoder(AV_CODEC_ID_DTS);
  if(!decoder) return fail(e,"DTS decoder absent",0);
  e->dec=avcodec_alloc_context3(decoder);
  if(!e->dec) return fail(e,"DTS decoder allocation",0);
  avcodec_parameters_to_context(e->dec,ap);
  e->dec->pkt_timebase=e->fmt->streams[e->info.audio_stream]->time_base;
  /* Corrupt HD extensions must reach the worker error path; an explicit
   * core-only reopen decides whether to discard the failed extension. */
  e->dec->err_recognition=AV_EF_EXPLODE;
  if(core && av_opt_set_int(e->dec->priv_data,"core_only",1,0)<0) return fail(e,"DTS core-only unavailable",0);
  if((r=avcodec_open2(e->dec,decoder,NULL))<0) return fail(e,"DTS decoder open",r);
  const AVCodec *encoder=avcodec_find_encoder(AV_CODEC_ID_AAC);
  if(!encoder) return fail(e,"Audio encoder absent",0);
  e->enc=avcodec_alloc_context3(encoder);
  if(!e->enc) return fail(e,"Audio encoder absent",0);
  e->enc->codec_id=encoder->id;
  av_channel_layout_default(&e->enc->ch_layout,2);
  if(reset_encoder(e)<0) return -1;
  e->fifo=av_audio_fifo_alloc(e->enc->sample_fmt,e->enc->ch_layout.nb_channels,4096);
  e->in=av_packet_alloc(); e->out=av_packet_alloc(); e->decoded=av_frame_alloc(); e->encoded=av_frame_alloc();
  if(!e->fifo || !e->in || !e->out || !e->decoded || !e->encoded) return fail(e,"Audio queue allocation",0);
  e->info.channels=e->enc->ch_layout.nb_channels; e->info.sample_rate=48000;
  snprintf(e->info.audio_codec,sizeof(e->info.audio_codec),"%s",avcodec_get_name(e->enc->codec_id));
  e->info.duration_ns=e->fmt->duration==AV_NOPTS_VALUE?0:av_rescale_q(e->fmt->duration,AV_TIME_BASE_Q,NS);
  e->info.start_ns=e->fmt->start_time==AV_NOPTS_VALUE?0:av_rescale_q(e->fmt->start_time,AV_TIME_BASE_Q,NS);
  e->target_ns=(int64_t)(start*1000000000.0);
  return start>0?dts_engine_seek(e,start):0;
}
static int resample(DtsEngine *e,AVFrame *f) {
  int r;
  if(f && (!e->swr || e->input_rate!=f->sample_rate || e->input_format!=f->format || av_channel_layout_compare(&e->input_layout,&f->ch_layout))) {
    /* Layout changes midstream need a new playback session; no silent remapping. */
    if(e->swr) return fail(e,"DTS layout changed midstream",0);
    e->input_rate=f->sample_rate; e->input_format=f->format;
    av_channel_layout_copy(&e->input_layout,&f->ch_layout);
    r=swr_alloc_set_opts2(&e->swr,&e->enc->ch_layout,e->enc->sample_fmt,48000,&f->ch_layout,f->format,f->sample_rate,0,NULL);
    if(r<0 || (r=swr_init(e->swr))<0) return fail(e,"Channel resampler open",r);
  }
  if(!e->swr) return 0;
  if(f && e->timeline && f->best_effort_timestamp!=AV_NOPTS_VALUE) {
    int64_t incoming=av_rescale_q(f->best_effort_timestamp,e->dec->pkt_timebase,e->enc->time_base);
    int64_t expected=e->sample_pts+av_audio_fifo_size(e->fifo)
        +av_rescale_rnd(swr_get_delay(e->swr,e->input_rate),48000,e->input_rate,AV_ROUND_NEAR_INF);
    if (incoming>expected+4800 || incoming<expected-4800)
      return fail(e,"DTS timestamp discontinuity requires reload",0);
  }
  int capacity=(int)av_rescale_rnd(swr_get_delay(e->swr,e->input_rate)+(f?f->nb_samples:0),48000,e->input_rate,AV_ROUND_UP)+32;
  uint8_t **data=NULL;
  r=av_samples_alloc_array_and_samples(&data,NULL,e->enc->ch_layout.nb_channels,capacity,e->enc->sample_fmt,0);
  if(r<0) return fail(e,"Resample buffer",r);
  int count=swr_convert(e->swr,data,capacity,f?(const uint8_t **)f->extended_data:NULL,f?f->nb_samples:0);
  if(count>=0 && f && !e->timeline) {
    int64_t pts=f->best_effort_timestamp;
    if(pts==AV_NOPTS_VALUE) { av_freep(&data[0]); av_freep(&data); return fail(e,"DTS sample timestamp absent",0); }
    e->sample_pts=av_rescale_q(pts,e->dec->pkt_timebase,e->enc->time_base); e->timeline=1;
  }
  if(count>0 && av_audio_fifo_write(e->fifo,(void **)data,count)!=count) count=AVERROR(ENOMEM);
  av_freep(&data[0]); av_freep(&data);
  return count<0?fail(e,"Audio resample",count):count;
}
static int encode_fifo(DtsEngine *e,int final) {
  int size=av_audio_fifo_size(e->fifo), count=e->enc->frame_size;
  if(size<count && (!final || size==0)) return 0;
  av_frame_unref(e->encoded);
  e->encoded->format=e->enc->sample_fmt; e->encoded->sample_rate=48000;
  av_channel_layout_copy(&e->encoded->ch_layout,&e->enc->ch_layout);
  e->encoded->nb_samples=count; e->encoded->pts=e->sample_pts;
  int r=av_frame_get_buffer(e->encoded,0);
  if(r<0) return fail(e,"Encoder sample buffer",r);
  int actual=size<count?size:count;
  av_samples_set_silence(e->encoded->extended_data,0,count,e->enc->ch_layout.nb_channels,e->enc->sample_fmt);
  if(av_audio_fifo_read(e->fifo,(void **)e->encoded->extended_data,actual)!=actual) return fail(e,"Audio queue read",0);
  e->sample_pts+=count;
  r=avcodec_send_frame(e->enc,e->encoded);
  return r<0?fail(e,"Audio encode",r):1;
}
static int frame_out(DtsEngine *e,DtsFrame *f,int kind,AVRational tb) {
  f->kind=kind; f->stream_index=kind==DTS_SUBTITLE?e->out->stream_index:kind==DTS_AUDIO?e->info.audio_stream:e->info.video_stream;
  f->data=e->out->data; f->size=e->out->size;
  f->pts_ns=ns(e->out->pts,tb); f->dts_ns=ns(e->out->dts,tb); f->duration_ns=ns(e->out->duration,tb);
  return 1;
}
static int next_frame(DtsEngine *e,DtsFrame *f) {
  if(!e || !f || !e->enc) return -1;
  av_packet_unref(e->out);
  for(;;) {
    if(cancelled(e)) return fail(e,"Cancelled",0);
    int r=avcodec_receive_packet(e->enc,e->out);
    if(r==0) {
      /* Delay appears in packet timestamps; trim only complete preroll packets. */
      int64_t end=ns(e->out->pts+e->out->duration,e->enc->time_base);
      if(end<=e->target_ns) { av_packet_unref(e->out); continue; }
      return frame_out(e,f,DTS_AUDIO,e->enc->time_base);
    }
    if(r==AVERROR_EOF) return 0;
    if(r!=AVERROR(EAGAIN)) return fail(e,"Audio output",r);
    if(e->bsf) {
      r=av_bsf_receive_packet(e->bsf,e->out);
      if(r==0) return frame_out(e,f,DTS_VIDEO,e->bsf->time_base_out);
      if(r!=AVERROR(EAGAIN) && r!=AVERROR_EOF) return fail(e,"Compressed video output",r);
    }
    r=encode_fifo(e,e->resampler_eof);
    if(r<0) return -1;
    if(r>0) continue;
    if(!e->decoder_eof) {
      r=avcodec_receive_frame(e->dec,e->decoded);
      if(r==0) { r=resample(e,e->decoded); av_frame_unref(e->decoded); if(r<0) return -1; continue; }
      if(r==AVERROR_EOF) e->decoder_eof=1;
      else if(r!=AVERROR(EAGAIN)) return fail(e,"DTS decode",r);
    }
    if(e->decoder_eof && !e->resampler_eof) {
      r=resample(e,NULL); if(r<0) return -1; if(r==0) e->resampler_eof=1; continue;
    }
    if(e->resampler_eof && !e->encoder_sent_eof) {
      if((r=avcodec_send_frame(e->enc,NULL))<0) return fail(e,"Audio encoder drain",r);
      e->encoder_sent_eof=1; continue;
    }
    if(e->demux_eof) return fail(e,"Unexpected DTS drain state",0);
    r=av_read_frame(e->fmt,e->in);
    if(r==AVERROR_EOF) {
      e->demux_eof=1;
      if((r=avcodec_send_packet(e->dec,NULL))<0) return fail(e,"DTS decoder drain",r);
      if(e->bsf && (r=av_bsf_send_packet(e->bsf,NULL))<0) return fail(e,"Video filter drain",r);
      continue;
    }
    if(r<0) return fail(e,"Container read",r);
    if(av_packet_get_side_data(e->in,AV_PKT_DATA_ENCRYPTION_INFO,NULL)) { av_packet_unref(e->in); return fail(e,"Encrypted packet unsupported",0); }
    if(e->fmt->streams[e->in->stream_index]->codecpar->codec_type==AVMEDIA_TYPE_SUBTITLE) {
      AVRational tb=e->fmt->streams[e->in->stream_index]->time_base;
      av_packet_move_ref(e->out,e->in);
      return frame_out(e,f,DTS_SUBTITLE,tb);
    }
    if(e->in->stream_index==e->info.audio_stream) {
      if(e->in->size>=4) {
        const uint8_t *b=e->in->data;
        uint32_t sync=((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];
        if(sync==UINT32_C(0x7ffe8001) || sync==UINT32_C(0xfe7f0180)
            || sync==UINT32_C(0x1fffe800) || sync==UINT32_C(0xff1f00e8)) e->has_core=1;
      }
      r=avcodec_send_packet(e->dec,e->in);
      if(r<0) { av_packet_unref(e->in); return fail(e,"DTS decode input",r); }
    }
    else if(e->in->stream_index==e->info.video_stream) {
      if(!e->bsf) {
        AVRational tb=e->fmt->streams[e->info.video_stream]->time_base;
        av_packet_move_ref(e->out,e->in);
        return frame_out(e,f,DTS_VIDEO,tb);
      }
      r=av_bsf_send_packet(e->bsf,e->in);
    }
    else r=0;
    av_packet_unref(e->in);
    if(r<0) return fail(e,"Compressed video input",r);
  }
}
int dts_engine_next(DtsEngine *e,DtsFrame *f) {
  int result=next_frame(e,f);
#ifdef NV_DTS_DEBUG
  if(e) {
    uint64_t now=clock_ns(CLOCK_MONOTONIC);
    if(!e->logged_at_ns) e->logged_at_ns=now;
    if(now>=e->logged_at_ns+UINT64_C(5000000000)) {
      DtsEngineMetrics *m=&e->metrics, *last=&e->logged_metrics;
      int ready=0, active=0, queued=0;
      uint64_t buffered=0;
      pthread_mutex_lock(&e->range_lock);
      for(int i=0;i<RANGE_SLOTS;i++) {
        DtsRangeJob *job=&e->ranges[i];
        if(job->state==1) queued++;
        if(job->state==2) active++;
        if(job->state==3 && job->data && job->count>0) { ready++; buffered+=(uint64_t)job->count; }
      }
      pthread_mutex_unlock(&e->range_lock);
      printf("[dts-transport] lanes=%d ready=%d active=%d queued=%d bufferedKiB=%llu ranges=%llu bytesKiB=%llu httpMs=%llu blockedMs=%llu maxReadMs=%llu shortReads=%llu\n",
             e->range_threads, ready, active, queued, (unsigned long long)(buffered/1024),
             (unsigned long long)(m->range_requests-last->range_requests),
             (unsigned long long)((m->range_bytes-last->range_bytes)/1024),
             (unsigned long long)((m->range_wait_ns-last->range_wait_ns)/1000000),
             (unsigned long long)((m->range_blocked_ns-last->range_blocked_ns)/1000000),
             (unsigned long long)(m->range_max_wait_ns/1000000),
             (unsigned long long)(m->range_short_reads-last->range_short_reads));
      e->logged_metrics=*m; e->logged_at_ns=now;
    }
  }
#endif
  return result;
}
int dts_engine_seek(DtsEngine *e,double seconds) {
  if(!e || !e->fmt || !isfinite(seconds) || seconds<0 || seconds>INT64_MAX/1000000000.0) return -1;
  if(cancelled(e)) return fail(e,"Cancelled",0);
  int64_t timestamp=(int64_t)(seconds*1000000.0);
  int r=avformat_seek_file(e->fmt,-1,INT64_MIN,timestamp,timestamp,AVSEEK_FLAG_BACKWARD);
  if(r<0) return fail(e,"Container seek",r);
  avcodec_flush_buffers(e->dec); if(e->bsf) av_bsf_flush(e->bsf);
  if(reset_encoder(e)<0) return -1;
  swr_free(&e->swr); av_channel_layout_uninit(&e->input_layout);
  av_audio_fifo_reset(e->fifo); av_packet_unref(e->in); av_packet_unref(e->out);
  e->demux_eof=e->decoder_eof=e->resampler_eof=e->encoder_sent_eof=e->timeline=0;
  e->target_ns=(int64_t)(seconds*1000000000.0);
  return 0;
}
#endif
