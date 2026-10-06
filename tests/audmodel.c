#define _DEFAULT_SOURCE
#include "audmodel.h"
#include "rede.h"
#include "sha256.h"
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
static char root[256];
static atomic_int requests,mode,holding,blocked,thread_failure;
int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
int __wrap_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void *),void *arg) {
  return atomic_load(&thread_failure)?EAGAIN:__real_pthread_create(t,a,fn,arg);
}
const char *dados_dir(void){return root;}
int dados_model_persistente(void){return !atomic_load(&blocked);}
int audsilero_available(void){return 1;}
int audsilero_validate_model(const char *p,char *e,size_t n){(void)p;(void)n;if(atomic_load(&mode)==3){strcpy(e,"Incompatible model format");return -1;}return 0;}
int rede_pedir(const RedePedido *p,RedeResposta *r){
  atomic_fetch_add(&requests,1);
  while(atomic_load(&holding) && !p->parar(p->parar_usuario))usleep(1000);
  if(p->parar(p->parar_usuario)){r->erro=REDE_CANCELADO;return 0;}
  r->erro=REDE_OK;r->status=200;
  r->corpo=strdup(atomic_load(&mode)==1?"abd":"abc");
  r->n_corpo=atomic_load(&mode)==2?2:3;return 1;
}
void rede_resposta_limpar(RedeResposta *r){free(r->corpo);}
static void wait_state(AudModelState state){for(int i=0;i<2000;i++){if(audmodel_status().state==state)return;usleep(1000);}assert(!"state timeout");}
int main(void){
  char pattern[]="/tmp/nv-audmodel.XXXXXX",path[1024],hash[65];
  assert(mkdtemp(pattern));strcpy(root,pattern);
  nv_sha256_hex((const unsigned char *)"abc",3,hash);assert(!strcmp(hash,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  assert(audmodel_status().state==AUDMODEL_OFF && !requests);
  thread_failure=1;
  audmodel_enable(1,0);wait_state(AUDMODEL_FAILED);assert(strstr(audmodel_status().error,"worker"));
  audmodel_retry();assert(audmodel_status().state==AUDMODEL_FAILED && !requests);
  audmodel_remove();assert(audmodel_status().state==AUDMODEL_FAILED && !requests);
  thread_failure=0;audmodel_enable(0,0);
  audmodel_enable(1,0);wait_state(AUDMODEL_FAILED);assert(!requests);
  char old[1024],partial[1024];
  snprintf(old,sizeof old,"%s/subtitle-autosync/silero-obsolete.ort",root);
  snprintf(partial,sizeof partial,"%s/subtitle-autosync/silero-obsolete.partial",root);
  assert(!mkdir(strcat(strcpy(path,root),"/subtitle-autosync"),0700));
  FILE *oldfile=fopen(old,"wb");assert(oldfile);fputs("old weights",oldfile);fclose(oldfile);
  oldfile=fopen(partial,"wb");assert(oldfile);fputs("crash",oldfile);fclose(oldfile);
  audmodel_retry();wait_state(AUDMODEL_READY);assert(requests==1);assert(audmodel_path(path,sizeof path));
  assert(audmodel_status().bytes==AUDMODEL_BYTES && access(old,F_OK) && access(partial,F_OK));
  audmodel_enable(0,0);assert(!audmodel_ready());
  audmodel_enable(1,0);wait_state(AUDMODEL_READY);assert(requests==1);
  oldfile=fopen(old,"wb");assert(oldfile);fputs("old weights",oldfile);fclose(oldfile);
  char lease[1024];assert(audmodel_acquire(lease,sizeof lease));
  audmodel_remove();usleep(10000);assert(!access(path,F_OK));
  audmodel_release();wait_state(AUDMODEL_OFF);for(int i=0;i<1000&&!access(path,F_OK);i++)usleep(1000);assert(access(path,F_OK));assert(access(old,F_OK));assert(audmodel_status().freed==14 && !audmodel_status().bytes);
  audmodel_enable(1,1);wait_state(AUDMODEL_READY);assert(requests==2);
  audmodel_enable(0,0);FILE *f=fopen(path,"wb");assert(f);fwrite("abd",1,3,f);fclose(f);
  audmodel_enable(1,0);wait_state(AUDMODEL_FAILED);assert(requests==2);
  mode=1;audmodel_retry();wait_state(AUDMODEL_FAILED);assert(!audmodel_ready());
  mode=2;audmodel_retry();wait_state(AUDMODEL_FAILED);
  mode=3;audmodel_retry();wait_state(AUDMODEL_FAILED);assert(strstr(audmodel_status().error,"format"));
  mode=0;holding=1;audmodel_retry();for(int i=0;i<1000&&audmodel_status().state!=AUDMODEL_DOWNLOADING;i++)usleep(1000);
  audmodel_enable(0,0);holding=0;usleep(20000);assert(audmodel_status().state==AUDMODEL_OFF && !audmodel_ready());
  blocked=1;audmodel_enable(1,1);wait_state(AUDMODEL_FAILED);assert(strstr(audmodel_status().error,"storage"));
  blocked=0;audmodel_enable(0,0);audmodel_remove();usleep(20000);audmodel_destroy();
  char dir[1024];snprintf(dir,sizeof dir,"%s/subtitle-autosync",root);rmdir(dir);rmdir(root);
  puts("audmodel: lifecycle, hash, cache, corruption, truncated transfer, cancel, persistence pass");
}
