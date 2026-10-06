/* Opt-in external real media test. No DSP compatibility path or fake VAD. */
#define _POSIX_C_SOURCE 200809L
#include "audsync.h"
#include "audmodel.h"
#include "autosync.h"
#include "rede.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Isolate persistent storage for the real model manager; offline by contract. */
const char *dados_dir(void) { return getenv("NUVIO_E2E_DATA"); }
int dados_model_persistente(void) { return 1; }
int rede_pedir(const RedePedido *p,RedeResposta *r) { (void)p;memset(r,0,sizeof *r);r->erro=REDE_INDISPONIVEL;return 0; }
void rede_resposta_limpar(RedeResposta *r) { free(r->corpo);free(r->cabecalhos); }
char *rede_baixar_bin(const char *p,int s,long *n) { (void)p;(void)s;(void)n;return NULL; }
static void tap(int on) { (void)on; }
static void pause_ms(void) { struct timespec t={0,1000000};nanosleep(&t,NULL); }
static double now(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9; }
static char *read_text(const char *path,long *size) {
 FILE *f=fopen(path,"rb");if(!f)return NULL;
 if(fseek(f,0,SEEK_END)||(*size=ftell(f))<0||*size>1024*1024||fseek(f,0,SEEK_SET)){fclose(f);return NULL;}
 char *s=malloc((size_t)*size+1);if(!s){fclose(f);return NULL;}
 if(fread(s,1,(size_t)*size,f)!=(size_t)*size){free(s);fclose(f);return NULL;}
 s[*size]=0;fclose(f);return s;
}
int main(int argc,char **argv) {
 if(argc!=7 && argc!=8){fprintf(stderr,"usage: real mono16k.raw real.srt start_seconds shift_ms positive|negative label [drift|cut|silence]\n");return 64;}
 const char *variant=argc==8?argv[7]:"none";
 double start=atof(argv[3]);int shift=atoi(argv[4]),positive=!strcmp(argv[5],"positive"),n=0;
 if(start<0)return 64;
 audmodel_enable(1,0);double deadline=now()+30;
 while(audmodel_status().state==AUDMODEL_VERIFYING&&now()<deadline)pause_ms();
 if(!audmodel_ready()){fprintf(stderr,"real model manager: %s\n",audmodel_status().error);audmodel_destroy();return 65;}
 long size=0;char *s=read_text(argv[2],&size);if(!s)return 66;
 LegendaDocumentoInfo info={.sessao=1,.flags=LEGENDA_DOC_COMPLETO};
 strcpy(info.idioma,"en");strcpy(info.origem,"Blender official real subtitle");strcpy(info.identidade,"external-real-test");
 LegendaDocumento *original=legenda_documento_bytes(s,size,&info);free(s);if(!original)return 66;
 const LegendaCue *c=legenda_documento_dados(original,&n);LegendaCue *copy=malloc((size_t)n*sizeof *copy);if(!copy)return 70;
 memcpy(copy,c,(size_t)n*sizeof *copy);
 for(int i=0;i<n;i++){double delta=shift/1000.0;
   if(!strcmp(variant,"drift")) delta+=(copy[i].inicio-start)*0.025;
   if(!strcmp(variant,"cut") && copy[i].inicio>=start+150) delta+=8;
   copy[i].inicio+=delta;copy[i].fim+=delta;}
 LegendaDocumento *doc=legenda_documento_de_cues(copy,n,&info);free(copy);legenda_documento_liberar(original);if(!doc)return 66;
 FILE *pcm=fopen(argv[1],"rb");if(!pcm)return 66;
 if(fseeko(pcm,(off_t)llround(start*16000)*2,SEEK_SET))return 66;
 audsync_backend(tap);audsync_formato(AUDSYNC_FMT_PCM);
 uint64_t pedido=audsync_pedir(1,doc,300,30000);int16_t chunk[997];int64_t fed=0,target=16000*300;
 double began=now();deadline=began+120;
 while(fed<target&&now()<deadline&&audsync_status().fase==AUDSYNC_OUVINDO){
   int take=(int)(target-fed);if(take>997)take=997;
   if(audsync_livre()<take){pause_ms();continue;}
   if(fread(chunk,sizeof *chunk,(size_t)take,pcm)!=(size_t)take){fprintf(stderr,"PCM shorter than 300-second requested window\n");return 66;}
   if(!strcmp(variant,"silence"))memset(chunk,0,(size_t)take*sizeof *chunk);
   if(!audsync_pcm_pedido(pedido,chunk,take,(int64_t)llround(start*1e6)+fed*1000000/16000)){fprintf(stderr,"unexpected ring/source rejection\n");return 70;}
   fed+=take;
 }
 fclose(pcm);
 while((audsync_status().fase==AUDSYNC_OUVINDO||audsync_status().fase==AUDSYNC_ALINHANDO)&&now()<deadline)pause_ms();
 AudSyncStatus st=audsync_status();AutoSyncResultado result={0};int accepted=0,gate=0;
 const char *stage="detector_alignment",*reason=audsync_motivo(st.motivo);
 if(st.fase==AUDSYNC_PRONTO){
   LegendaDocumento *ref=NULL,*crop=NULL;assert(audsync_tomar(pedido,&ref,&crop));
   AutoSyncConfig cfg=autosync_config(AUTOSYNC_QUICK);cfg.raioBuscaMs=30000;
   result=autosync_comparar(crop,ref,&cfg,NULL,NULL);
   stage="final_acceptance";reason=autosync_motivo(result.motivo);accepted=result.estado==AUTOSYNC_ACCEPTED;
   legenda_documento_liberar(ref);legenda_documento_liberar(crop);
 }
 gate=positive?(accepted&&abs(result.offsetMs-shift)<=250):!accepted;
 printf("{\"label\":\"%s\",\"start_seconds\":%.3f,\"samples\":%lld,\"shift_ms\":%d,\"positive\":%s,\"stage\":\"%s\",\"reason\":\"%s\",\"accepted\":%s,\"offset_ms\":%d,\"candidate_ms\":%d,\"confidence\":%.6f,\"candidate_confidence\":%.6f,\"regions\":%d,\"elapsed_seconds\":%.3f,\"drops\":%ld,\"gate_pass\":%s}\n",
 argv[6],start,(long long)fed,shift,positive?"true":"false",stage,reason,accepted?"true":"false",result.offsetMs,st.estimativaMs,result.confianca,st.confianca,result.regioes,now()-began,st.descartados,gate?"true":"false");
 legenda_documento_liberar(doc);audsync_destruir();audmodel_destroy();return gate?0:2;
}
