#ifndef NV_AUDMODEL_H
#define NV_AUDMODEL_H
#include <stdint.h>
#include <stddef.h>
#include "audmodel_manifest.h"
typedef enum { AUDMODEL_OFF, AUDMODEL_DOWNLOADING, AUDMODEL_VERIFYING, AUDMODEL_READY, AUDMODEL_FAILED } AudModelState;
typedef struct { AudModelState state; int enabled, progress; uint64_t bytes, freed; char error[192]; } AudModelStatus;
int audmodel_supported(void);
const char *audmodel_unavailable(void);
/* UI calls are nonblocking. First enable checks cached integrity, then downloads
 * only when explicitly enabled. A persisted startup enable checks cache only. */
void audmodel_enable(int enabled, int allow_download);
void audmodel_retry(void);
void audmodel_remove(void); /* disable first; asynchronously join file users */
AudModelStatus audmodel_status(void);
int audmodel_ready(void);
int audmodel_path(char *dst, size_t size);
/* Worker leases prevent removal while a runtime session maps model bytes. */
int audmodel_acquire(char *dst, size_t size);
void audmodel_release(void);
void audmodel_destroy(void);
#endif
