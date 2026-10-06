#ifndef NV_AUDDECODE_H
#define NV_AUDDECODE_H
#include <stdint.h>
typedef struct AudDecode AudDecode;
typedef struct {
  const int16_t *samples; /* borrowed until next/open/destroy */
  int count;             /* mono, 16000 Hz */
  int64_t pts_us;        /* media timestamp of first sample */
} AudDecodePcm;
typedef struct {
  uint64_t source_bytes, range_requests;
  double elapsed_seconds;
  double container_start_seconds;
} AudDecodeMetrics;
AudDecode *auddecode_create(void);
int auddecode_available(void);
/* Worker-only. HTTP(S) indexed MP4/MKV only; absolute demux stream index,
 * or -1 ONLY when there is exactly one audio track. Window <=300 seconds.
 * A player ordinal must be explicitly mapped before calling this API.
 * Returns 0 on success, -1 on failure. */
int auddecode_open(AudDecode *, const char *url, const char *headers,
                   int audio_stream, double start_seconds, double duration_seconds);
/* 1 = PCM; 0 = window complete/EOF; -1 = unavailable/error (no correction).
 * Source budget: 64 MiB including rejected/redirect bodies, 120 s elapsed,
 * <=2 HTTP requests per second; no speculative reads, no encode. */
int auddecode_next(AudDecode *, AudDecodePcm *);
void auddecode_metrics(AudDecode *, AudDecodeMetrics *);
const char *auddecode_error(AudDecode *);
/* Only cancel is concurrent-safe; cancellation is terminal for this instance.
 * Owner must join worker before destroy. */
void auddecode_cancel(AudDecode *);
void auddecode_destroy(AudDecode *);
#endif
