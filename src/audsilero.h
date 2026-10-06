#ifndef NV_AUDSILERO_H
#define NV_AUDSILERO_H
#include "audvad.h"
#include <stddef.h>
/* Worker-owned; never call inference from the playback PCM producer. */
typedef struct AudSilero AudSilero;
int audsilero_available(void);
AudSilero *audsilero_create(const char *path, char *error, size_t error_size);
/* Opens the model and runs one zero frame; zero means valid. */
int audsilero_validate_model(const char *path, char *error, size_t error_size);
void audsilero_reset(AudSilero *v);
/* Returns zero on success. Discontinuous timestamps reset the whole window. */
int audsilero_feed(AudSilero *v, const int16_t *pcm, int n, int64_t pts_us);
int audsilero_flush(AudSilero *v);
const AudSeg *audsilero_segments(const AudSilero *v, int *count);
const char *audsilero_error(const AudSilero *v);
void audsilero_destroy(AudSilero *v);
/* Optional worker-side observation for native parity tests. */
typedef void (*AudSileroProbability)(void *, int64_t, int, float);
void audsilero_observe(AudSilero *v, AudSileroProbability callback, void *opaque);
#endif
