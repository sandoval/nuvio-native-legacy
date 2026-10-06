#define _DEFAULT_SOURCE
#include "audsource.h"
#include "auddecode.h"
#include "audsync.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct AudDecode { int mode, sent; atomic_int cancelled; };
enum { DECODE_SHORT_EOF, DECODE_WAIT, DECODE_PAUSE };

static pthread_mutex_t M = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t C = PTHREAD_COND_INITIALIZER;
static AudSyncStatus status;
static void (*backend)(int);
static int entered, opened, destroyed, received, failures, cancels, release_next;
static double last_open_start;
static int16_t pcm[2048];

AudDecode *auddecode_create(void) {
  AudDecode *d=calloc(1,sizeof *d); assert(d); return d;
}
int auddecode_available(void) { return 1; }
int auddecode_open(AudDecode *d,const char *url,const char *headers,int track,double start,double duration) {
  assert(url && *url); (void)headers;(void)track;(void)duration;
  pthread_mutex_lock(&M); opened++;last_open_start=start;
  d->mode=strstr(url,"wait")?DECODE_WAIT:strstr(url,"pause")?DECODE_PAUSE:DECODE_SHORT_EOF;
  pthread_mutex_unlock(&M);
  return strstr(url,"unsupported")?-1:0;
}
int auddecode_next(AudDecode *d,AudDecodePcm *out) {
  pthread_mutex_lock(&M); entered++; pthread_cond_broadcast(&C); pthread_mutex_unlock(&M);
  if(d->mode==DECODE_WAIT) {
    while(!atomic_load(&d->cancelled)) usleep(1000);
    return -1;
  }
  if(d->mode==DECODE_PAUSE && d->sent) {
    while(!atomic_load(&d->cancelled)) {
      pthread_mutex_lock(&M);int go=release_next;pthread_mutex_unlock(&M);
      if(go) break;
      usleep(1000);
    }
    if(atomic_load(&d->cancelled)) return -1;
  }
  if(d->sent++) return 0;
  *out=(AudDecodePcm){pcm,2048,1000000}; return 1;
}
void auddecode_cancel(AudDecode *d) { atomic_store(&d->cancelled,1); pthread_mutex_lock(&M);cancels++;pthread_mutex_unlock(&M); }
void auddecode_destroy(AudDecode *d) { pthread_mutex_lock(&M);destroyed++;pthread_mutex_unlock(&M);free(d); }
const char *auddecode_error(AudDecode *d) { (void)d; return "Unsupported or malformed MP4/MKV audio source"; }
void auddecode_metrics(AudDecode *d,AudDecodeMetrics *m) { (void)d;*m=(AudDecodeMetrics){0}; }

void audsync_backend(void (*fn)(int on)) { pthread_mutex_lock(&M);backend=fn;pthread_mutex_unlock(&M); }
void audsync_formato(int f) { (void)f; }
AudSyncStatus audsync_status(void) { AudSyncStatus s;pthread_mutex_lock(&M);s=status;pthread_mutex_unlock(&M);return s; }
int audsync_livre(void) { return AUDSYNC_RING; }
int audsync_pcm_pedido(uint64_t request,const int16_t *samples,int n,int64_t pts) {
  (void)samples;(void)pts;pthread_mutex_lock(&M);
  int ok=status.pedido==request && status.fase==AUDSYNC_OUVINDO && !status.pausado;
  if(ok) received+=n;
  pthread_mutex_unlock(&M);
  return ok;
}
void audsync_backend_falhar_pedido(uint64_t request,AudSyncMotivo reason) {
  pthread_mutex_lock(&M);if(status.pedido==request&&status.fase==AUDSYNC_OUVINDO){status.fase=AUDSYNC_FALHOU;status.motivo=reason;failures++;}pthread_cond_broadcast(&C);pthread_mutex_unlock(&M);
}
void audsync_backend_falhar(AudSyncMotivo reason) { audsync_backend_falhar_pedido(0,reason); }
void audsync_cancelar(void) { pthread_mutex_lock(&M);status.fase=AUDSYNC_PARADO;status.pedido++;pthread_cond_broadcast(&C);pthread_mutex_unlock(&M); }

static void wait_for(int *value,int expected,int milliseconds) {
  for(int i=0;i<milliseconds;i++) { pthread_mutex_lock(&M);int ready=*value>=expected;pthread_mutex_unlock(&M);if(ready)return;usleep(1000); }
  assert(!"timed out waiting for fake decoder lifecycle");
}
static void reset_case(uint64_t request) {
  pthread_mutex_lock(&M);
  status=(AudSyncStatus){.pedido=request,.fase=AUDSYNC_OUVINDO};
  entered=opened=destroyed=received=failures=cancels=release_next=0;last_open_start=0;
  pthread_mutex_unlock(&M);
}
static void enable_backend(void) {
  pthread_mutex_lock(&M);void (*fn)(int)=backend;pthread_mutex_unlock(&M);assert(fn);fn(1);
}

int main(void) {
  audsource_update("https://example.invalid/unsupported.mkv","",0,0,1.0,1);
  reset_case(10);enable_backend();wait_for(&failures,1,2500);
  assert(status.motivo==AUDSYNC_M_SOURCE);audsource_destroy();
  /* A genuinely short EOF cannot satisfy the requested analysis window. */
  audsource_update("https://example.invalid/short.mkv","",0,0,1.0,1);
  reset_case(11);
  enable_backend();
  wait_for(&failures,1,2500);
  assert(status.motivo==AUDSYNC_M_DECODER && received==2048 && opened==1);
  audsource_destroy();
  assert(destroyed==1);

  /* Replacing a source cancels and joins its active decoder. The old request
   * must not publish a decoder failure into the new source generation. */
  audsource_update("https://example.invalid/wait.mkv","",0,0,1.0,1);
  reset_case(22);
  enable_backend(); wait_for(&entered,1,1000);
  audsource_update("https://example.invalid/replaced.mkv","",0,0,1.0,1);
  wait_for(&destroyed,1,1000);
  assert(cancels>=1 && failures==0 && status.fase==AUDSYNC_PARADO);
  audsource_destroy();

  /* Buffer/seek pause is temporary. It must not be reported as a short or
   * failed decode while the analysis session remains paused. */
  audsource_update("https://example.invalid/pause.mkv","",0,0,1.0,1);
  reset_case(33);
  enable_backend(); wait_for(&entered,2,1000);
  pthread_mutex_lock(&M);status.pausado=1;void (*fn)(int)=backend;pthread_mutex_unlock(&M);
  assert(fn);fn(0);wait_for(&destroyed,1,1000);
  /* Player updates continue while paused. Resume must start a fresh decoder at
   * the latest playback position, with the same session request. */
  audsource_update("https://example.invalid/pause.mkv","",0,0,4.0,1);
  pthread_mutex_lock(&M);status.pausado=0;pthread_mutex_unlock(&M);fn(1);
  wait_for(&opened,2,1000);
  for(int i=0;i<500;i++){pthread_mutex_lock(&M);int more=received>=4096;pthread_mutex_unlock(&M);if(more)break;usleep(1000);}
  pthread_mutex_lock(&M);int resumed=received>=4096 && last_open_start==4.0 && failures==0;pthread_mutex_unlock(&M);
  assert(resumed && "PCM producer must resume from the updated player position");
  audsource_stop(); audsource_destroy();
  puts("audsource: short EOF, generation cancellation and paused lifecycle checked");
  return 0;
}
