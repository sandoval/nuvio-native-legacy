#define _POSIX_C_SOURCE 200809L
#include "audmodel.h"
#include "audsilero.h"
#include "dados.h"
#include "rede.h"
#include "sha256.h"
#include <pthread.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static pthread_mutex_t M = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t C = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static int started, stop, pending, download, remove_file, users;
static uint64_t generation;
static AudModelStatus status;

int audmodel_supported(void) {
#if defined(NUVIO_SILERO_ORT)
  return !strncmp(AUDMODEL_URL, "https://", 8) && audsilero_available();
#else
  return 0;
#endif
}
const char *audmodel_unavailable(void) {
#ifdef NUVIO_SILERO_MINIMAL
  return "Converted Silero model release is not published for this runtime";
#else
  return "Silero runtime is unavailable in this build";
#endif
}
static int paths(char *final, char *partial, char *dir) {
  const char *root = dados_dir();
  if (!dados_model_persistente() || !root || !*root || strlen(root) > 700) return 0;
  snprintf(dir, 1024, "%s/subtitle-autosync", root);
  snprintf(final, 1024, "%s/subtitle-autosync/" AUDMODEL_ID "." AUDMODEL_FORMAT, root);
  snprintf(partial, 1024, "%s/subtitle-autosync/" AUDMODEL_ID ".partial", root);
  return 1;
}
static int canceled(void *opaque) {
  uint64_t g = *(uint64_t *)opaque; int result;
  pthread_mutex_lock(&M); result = stop || g != generation || !status.enabled; pthread_mutex_unlock(&M);
  return result;
}
static void progress(const RedeIntervalo *i, void *opaque) {
  if (canceled(opaque)) return;
  pthread_mutex_lock(&M);
  if (!stop && *(uint64_t *)opaque == generation && status.enabled) {
    status.bytes += i->bytes;
    status.progress = (int)(status.bytes * 100 / AUDMODEL_BYTES);
    if (status.progress > 99) status.progress = 99;
  }
  pthread_mutex_unlock(&M);
}
static int verify(const unsigned char *data, size_t size, char *error) {
  char hash[65];
  if (size != AUDMODEL_BYTES) { strcpy(error, "Model download has an incorrect size"); return 0; }
  nv_sha256_hex(data, size, hash);
  if (strcmp(hash, AUDMODEL_SHA256)) { strcpy(error, "Model checksum verification failed"); return 0; }
  return 1;
}
static int cache(const char *path, char *error) {
  struct stat st; unsigned char *buf; FILE *f; size_t n; int ok;
  if (lstat(path, &st) || !S_ISREG(st.st_mode) || st.st_size != AUDMODEL_BYTES) return 0;
  f = fopen(path, "rb"); if (!f) return 0;
  buf = malloc(AUDMODEL_BYTES); if (!buf) { fclose(f); strcpy(error,"Out of memory verifying model"); return 0; }
  n = fread(buf, 1, AUDMODEL_BYTES, f); fclose(f);
  ok = verify(buf,n,error); free(buf);
  return ok && audsilero_validate_model(path,error,192) == 0;
}
/* Only manager-owned regular files are removed. Old version weights are never
 * loaded using new metadata; retain them until a replacement verifies. */
static uint64_t prune(const char *dir, const char *keep, char *error) {
  DIR *d=opendir(dir); uint64_t freed=0;
  if (!d) { if (errno!=ENOENT) strcpy(error,"Unable to inspect downloaded models"); return 0; }
  struct dirent *entry;
  while ((entry=readdir(d))) {
    const char *name=entry->d_name, *ext=strrchr(name,'.');
    if (strncmp(name,"silero-",7) || !ext ||
        (strcmp(ext,".onnx") && strcmp(ext,".ort") && strcmp(ext,".partial"))) continue;
    char path[1024]; struct stat st;
    int n=snprintf(path,sizeof path,"%s/%s",dir,name);
    if (n<0 || (size_t)n>=sizeof path || (keep && !strcmp(path,keep))) continue;
    if (!lstat(path,&st) && S_ISREG(st.st_mode)) {
      if (!unlink(path)) freed+=(uint64_t)st.st_size;
      else strcpy(error,"Unable to remove downloaded model");
    }
  }
  closedir(d); return freed;
}
static void *run(void *unused) {
  (void)unused;
  pthread_mutex_lock(&M);
  while (!stop) {
    uint64_t g; int dl, rm, ok=0; char final[1024],part[1024],dir[1024],error[192]=""; uint64_t freed=0;
    while (!stop && !pending) pthread_cond_wait(&C,&M);
    if (stop) break;
    g=generation; dl=download; rm=remove_file; pending=0; remove_file=0;
    pthread_mutex_unlock(&M);
    if (!paths(final,part,dir)) strcpy(error,"Persistent model storage is unavailable");
    else if (rm) {
      pthread_mutex_lock(&M);
      while (users && !stop && g==generation) pthread_cond_wait(&C,&M);
      int can_remove = !users && g==generation;
      pthread_mutex_unlock(&M);
      if (!can_remove) { pthread_mutex_lock(&M); continue; }
      /* Inference owns independent loaded weights, never a mapped cache file.
       * Session cancellation occurs on UI before this action. */
      freed=prune(dir,NULL,error);
    } else if (!audmodel_supported()) snprintf(error,sizeof error,"%s",audmodel_unavailable());
    else {
      unlink(part); /* incomplete prior attempt/crash */
      ok=cache(final,error);
      if (!ok && dl && !canceled(&g)) {
        struct statvfs space;
        RedePedido req={0}; RedeResposta res={0};
        if ((mkdir(dir,0700) && access(dir,W_OK)) || statvfs(dir,&space) ||
            (uint64_t)space.f_bavail * space.f_frsize < 2u*AUDMODEL_BYTES+1048576u) strcpy(error,"Not enough writable storage for model download");
        else {
          pthread_mutex_lock(&M);
          if (g==generation) { status.state=AUDMODEL_DOWNLOADING; status.bytes=0; status.progress=0; }
          pthread_mutex_unlock(&M);
          req.url=AUDMODEL_URL; req.prazo_ms=120000; req.max_bytes=AUDMODEL_BYTES; req.seguir=1;
          req.parar=canceled; req.parar_usuario=&g; req.intervalo=progress; req.intervalo_usuario=&g;
          if (!rede_pedir(&req,&res) || res.erro!=REDE_OK || res.status!=200) strcpy(error,"Model download failed; check connection and retry");
          else if (!canceled(&g)) {
            pthread_mutex_lock(&M); if (g==generation) status.state=AUDMODEL_VERIFYING; pthread_mutex_unlock(&M);
            if (verify((unsigned char *)res.corpo,res.n_corpo,error)) {
              FILE *f=fopen(part,"wb"); int wrote=0;
              if (f) { wrote=fwrite(res.corpo,1,res.n_corpo,f)==res.n_corpo; if (fflush(f) || fsync(fileno(f))) wrote=0; if (fclose(f)) wrote=0; }
              if (!wrote) strcpy(error,"Unable to save downloaded model");
              else if (audsilero_validate_model(part,error,sizeof error) == 0) {
                pthread_mutex_lock(&M);
                if (g==generation && status.enabled && !stop) { ok=rename(part,final)==0; if (!ok) strcpy(error,"Unable to install verified model"); }
                pthread_mutex_unlock(&M);
              }
            }
          }
          rede_resposta_limpar(&res); unlink(part);
        }
      }
      if (!ok && !*error) strcpy(error,"Verified model is missing; choose Retry to download");
    }
    if (ok && !canceled(&g)) {
      error[0]=0;
      prune(dir,final,error);
    }
    pthread_mutex_lock(&M);
    if (g==generation) {
      status.freed=freed;
      if (ok) status.bytes=AUDMODEL_BYTES;
      if (rm && !*error) status.bytes=0;
      if (rm) { status.state=*error?AUDMODEL_FAILED:AUDMODEL_OFF; }
      else { status.state=ok?AUDMODEL_READY:AUDMODEL_FAILED; status.progress=ok?100:status.progress; }
      snprintf(status.error,sizeof status.error,"%s",error);
    }
  }
  pthread_mutex_unlock(&M); return NULL;
}
static void start(void) { if (!started && !stop && !pthread_create(&worker,NULL,run,NULL)) started=1; }
void audmodel_enable(int enabled,int allow_download) {
  pthread_mutex_lock(&M);
  if (status.enabled==!!enabled) { pthread_mutex_unlock(&M); return; }
  generation++; remove_file=0; status.enabled=!!enabled; status.error[0]=0;
  if (!enabled) { pending=0; status.state=AUDMODEL_OFF; }
  else { start(); status.state=AUDMODEL_VERIFYING; pending=1; download=allow_download; pthread_cond_signal(&C);
    if (!started) { status.state=AUDMODEL_FAILED; strcpy(status.error,"Unable to start model worker"); } }
  pthread_mutex_unlock(&M);
}
void audmodel_retry(void) { pthread_mutex_lock(&M); if(status.enabled && status.state==AUDMODEL_FAILED) { generation++; start(); pending=1;download=1;status.state=AUDMODEL_VERIFYING;pthread_cond_signal(&C); if (!started) { status.state=AUDMODEL_FAILED; strcpy(status.error,"Unable to start model worker"); } } pthread_mutex_unlock(&M); }
void audmodel_remove(void) { audmodel_enable(0,0); pthread_mutex_lock(&M);generation++;start();pending=1;remove_file=1;pthread_cond_signal(&C);if (!started) { status.state=AUDMODEL_FAILED; strcpy(status.error,"Unable to start model worker"); } pthread_mutex_unlock(&M); }
AudModelStatus audmodel_status(void) { AudModelStatus s;pthread_mutex_lock(&M);s=status;pthread_mutex_unlock(&M);return s; }
int audmodel_ready(void) { AudModelStatus s=audmodel_status();return s.enabled && s.state==AUDMODEL_READY; }
int audmodel_path(char *dst,size_t size) { char f[1024],p[1024],d[1024];if(!audmodel_ready()||!paths(f,p,d)||strlen(f)>=size)return 0;strcpy(dst,f);return 1; }
int audmodel_acquire(char *dst,size_t size) {
  char f[1024],p[1024],d[1024]; int ok=0;
  pthread_mutex_lock(&M);
  if(status.enabled && status.state==AUDMODEL_READY && paths(f,p,d) && strlen(f)<size) { strcpy(dst,f);users++;ok=1; }
  pthread_mutex_unlock(&M);return ok;
}
void audmodel_release(void) { pthread_mutex_lock(&M);if(users)users--;pthread_cond_broadcast(&C);pthread_mutex_unlock(&M); }
void audmodel_destroy(void) { int join;pthread_mutex_lock(&M);generation++;stop=1;status.enabled=0;status.state=AUDMODEL_OFF;join=started;pthread_cond_broadcast(&C);pthread_mutex_unlock(&M);if(join)pthread_join(worker,NULL);pthread_mutex_lock(&M);started=0;pthread_mutex_unlock(&M); }
