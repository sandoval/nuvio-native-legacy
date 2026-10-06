#include "audsource.h"
#include "auddecode.h"
#include "audsync.h"
#include <pthread.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static struct {
  char url[4096], headers[4096];
  int stream, selection, capable, queued, running, quit, registered;
  double position;
  uint64_t generation;
  pthread_t thread;
  AudDecode *decoder;
} source;

static double monotonic_seconds(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
  return now.tv_sec + now.tv_nsec / 1e9;
}

static int current(uint64_t generation) {
  pthread_mutex_lock(&lock);
  int result = !source.quit && source.generation == generation;
  pthread_mutex_unlock(&lock);
  return result;
}

static void *worker(void *unused) {
  (void)unused;
  pthread_mutex_lock(&lock);
  while (!source.quit) {
    while (!source.queued && !source.quit) pthread_cond_wait(&wake, &lock);
    if (source.quit) break;
    char url[sizeof source.url], headers[sizeof source.headers];
    memcpy(url, source.url, sizeof url); memcpy(headers, source.headers, sizeof headers);
    int stream = source.stream;
    double position = source.position;
    uint64_t generation = source.generation;
    uint64_t request = audsync_status().pedido;
    source.queued = 0;
    AudDecode *decoder = auddecode_create();
    source.decoder = decoder;
    pthread_mutex_unlock(&lock);
    double deadline = monotonic_seconds() + 120;
    int result = decoder ? auddecode_open(decoder, url, headers, stream,
                                          position, AUDSYNC_ALVO_SEG) : -1;
    AudSyncMotivo failure = AUDSYNC_M_DECODER;
    if (result == 0 && stream < 0) {
      AudDecodeMetrics metrics;
      auddecode_metrics(decoder, &metrics);
      /* uMS does not expose its container timeline origin. Until that mapping
       * is proved, a nonzero container origin is unavailable, never treated
       * as an offset in the external subtitle. DTS exposes absolute time. */
      if (!isfinite(metrics.container_start_seconds) || fabs(metrics.container_start_seconds) > 0.001) {
        result = -1; failure = AUDSYNC_M_SOURCE;
      }
    }
    if (result == 0) {
      AudDecodePcm pcm;
      while (current(generation) && (result = auddecode_next(decoder, &pcm)) > 0) {
        int used = 0;
        while (used < pcm.count && current(generation)) {
          if (monotonic_seconds() >= deadline) {
            result = -1; failure = AUDSYNC_M_BUDGET; break;
          }
          AudSyncStatus status = audsync_status();
          if (status.pedido != request || status.fase != AUDSYNC_OUVINDO || status.pausado) {
            result = 0; break;
          }
          int count = pcm.count - used;
          if (count > 2048) count = 2048;
          /* Auxiliary decoding can outrun inference. Pace this worker while
           * keeping the playback producer's copy-only contract unchanged. */
          if (audsync_livre() < count) {
            const struct timespec delay = {0, 2000000}; nanosleep(&delay, NULL);
            continue;
          }
          if (!audsync_pcm_pedido(request, pcm.samples + used, count,
                           pcm.pts_us + (int64_t)used * 1000000 / 16000)) {
            result = 0; break;
          }
          used += count;
        }
        if (result <= 0) break;
      }
    }
    if (result < 0 && decoder && failure == AUDSYNC_M_DECODER) {
      const char *reason = auddecode_error(decoder);
      if (strstr(reason, "budget")) failure = AUDSYNC_M_BUDGET;
      else if (strstr(reason, "track")) failure = AUDSYNC_M_TRACK;
      else if (strstr(reason, "supports") || strstr(reason, "Range") ||
               strstr(reason, "seekable") || strstr(reason, "Live") ||
               strstr(reason, "unsupported") || strstr(reason, "Unsupported") || strstr(reason, "index"))
        failure = AUDSYNC_M_SOURCE;
    }
    /* Completion can precede the consumer finishing its last inference frame.
     * Let the bounded ring drain before deciding a short window is unavailable. */
    for (int waits = 0; waits < 500 && current(generation) && monotonic_seconds() < deadline; ++waits) {
      AudSyncStatus status = audsync_status();
      if (status.pedido != request || status.fase != AUDSYNC_OUVINDO || status.pausado) break;
      const struct timespec delay = {0, 2000000}; nanosleep(&delay, NULL);
    }
    if (current(generation)) {
      AudSyncStatus status = audsync_status();
      if (status.pedido == request && status.fase == AUDSYNC_OUVINDO && !status.pausado) {
        /* Stable reasons only: never print decoder diagnostics containing a
         * source URL or authentication headers. A short/failed window cannot
         * produce an automatic correction. */
        audsync_backend_falhar_pedido(request, failure);
      }
    }
    pthread_mutex_lock(&lock);
    source.decoder = NULL;
    pthread_mutex_unlock(&lock);
    auddecode_destroy(decoder);
    pthread_mutex_lock(&lock);
  }
  pthread_mutex_unlock(&lock);
  return NULL;
}

static void enable(int on) {
  pthread_mutex_lock(&lock);
  ++source.generation;
  source.queued = 0;
  if (source.decoder) auddecode_cancel(source.decoder);
  if (on && source.capable && !source.quit) {
    if (!source.running && pthread_create(&source.thread, NULL, worker, NULL) == 0)
      source.running = 1;
    if (source.running) { source.queued = 1; pthread_cond_signal(&wake); }
  }
  int failed = on && source.capable && !source.running;
  pthread_mutex_unlock(&lock);
  if (failed) audsync_backend_falhar(AUDSYNC_M_MEMORIA);
}

void audsource_update(const char *url, const char *headers, int stream,
                      int selection, double position, int capable) {
  if (!auddecode_available()) return;
  if (!url) url = "";
  if (!headers) headers = "";
  if (strlen(url) >= sizeof source.url || strlen(headers) >= sizeof source.headers) capable = 0;
  pthread_mutex_lock(&lock);
  int changed = strcmp(source.url, url) || strcmp(source.headers, headers) ||
                source.stream != stream || source.selection != selection || source.capable != capable;
  if (changed) {
    ++source.generation; source.queued = 0;
    if (source.decoder) auddecode_cancel(source.decoder);
  }
  snprintf(source.url, sizeof source.url, "%s", url);
  snprintf(source.headers, sizeof source.headers, "%s", headers);
  source.stream = stream; source.selection = selection;
  source.position = position; source.capable = capable;
  int register_backend = !source.registered;
  source.registered = 1;
  pthread_mutex_unlock(&lock);
  if (register_backend) audsync_backend(enable);
  if (changed) audsync_cancelar();
  audsync_formato(capable ? AUDSYNC_FMT_PCM : AUDSYNC_FMT_NENHUM);
}

void audsource_stop(void) {
  enable(0);
  audsync_cancelar();
  audsync_formato(AUDSYNC_FMT_NENHUM);
}

void audsource_destroy(void) {
  pthread_mutex_lock(&lock);
  source.quit = 1; ++source.generation; source.queued = 0;
  if (source.decoder) auddecode_cancel(source.decoder);
  pthread_cond_signal(&wake);
  int running = source.running;
  pthread_mutex_unlock(&lock);
  if (running) pthread_join(source.thread, NULL);
  pthread_mutex_lock(&lock);
  source.running = 0; source.quit = 0; source.registered = 0;
  source.capable = 0; source.url[0] = source.headers[0] = 0;
  pthread_mutex_unlock(&lock);
}
