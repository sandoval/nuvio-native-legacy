#ifndef NV_MEDIAIO_FFMPEG_H
#define NV_MEDIAIO_FFMPEG_H
/* Shared custom AVIO/container setup. Read policies remain worker-owned: DTS
 * keeps its playback prefetch, analysis keeps finite budgets and no prefetch.
 * No signed URL, decoder state or mutable buffer is shared between sessions. */
#include <libavformat/avformat.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>

static inline int mediaio_position(int64_t position, int64_t total,
                                  int64_t offset, int whence, int64_t *next) {
  int64_t base=whence==SEEK_SET?0:whence==SEEK_CUR?position:whence==SEEK_END?total:-1;
  if(base<0 || (offset>0 && base>INT64_MAX-offset) || (offset<0 && offset<-base))
    return AVERROR(EINVAL);
  *next=base+offset; return 0;
}

static inline int mediaio_open(AVFormatContext **format, AVIOContext **io,
                               void *owner, int (*read)(void *,uint8_t *,int),
                               int64_t (*seek)(void *,int64_t,int),
                               int (*cancel)(void *), int64_t probe_bytes,
                               int64_t analyze_us) {
  *format=avformat_alloc_context();
  uint8_t *buffer=av_malloc(32768);
  if(!*format || !buffer) { av_free(buffer); return AVERROR(ENOMEM); }
  *io=avio_alloc_context(buffer,32768,0,owner,read,NULL,seek);
  if(!*io) { av_free(buffer); return AVERROR(ENOMEM); }
  (*io)->seekable=AVIO_SEEKABLE_NORMAL;
  (*format)->pb=*io; (*format)->flags|=AVFMT_FLAG_CUSTOM_IO;
  (*format)->interrupt_callback=(AVIOInterruptCB){cancel,owner};
  /* Malformed metadata must never print source/subtitle contents. */
  av_log_set_level(AV_LOG_QUIET);
  AVDictionary *options=NULL;
  av_dict_set(&options,"format_whitelist","matroska,webm,mov",0);
  av_dict_set_int(&options,"probesize",probe_bytes,0);
  av_dict_set_int(&options,"analyzeduration",analyze_us,0);
  int result=avformat_open_input(format,NULL,NULL,&options);
  av_dict_free(&options); return result;
}
#endif
