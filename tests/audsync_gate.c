#include "audsync.h"
#include "audmodel.h"
#include "audsilero.h"
#include <assert.h>
#include <stdio.h>
static int supported, ready, created;
int audmodel_supported(void){return supported;}
int audmodel_ready(void){return ready;}
int audmodel_acquire(char *d,size_t n){(void)d;(void)n;return 0;}
void audmodel_release(void){}
AudSilero *audsilero_create(const char *p,char *e,size_t n){(void)p;(void)e;(void)n;created++;return NULL;}
void audsilero_reset(AudSilero *s){(void)s;}
void audsilero_destroy(AudSilero *s){(void)s;}
int audsilero_feed(AudSilero *s,const int16_t *p,int n,int64_t t){(void)s;(void)p;(void)n;(void)t;return -1;}
int audsilero_flush(AudSilero *s){(void)s;return -1;}
const AudSeg *audsilero_segments(const AudSilero *s,int *n){(void)s;*n=0;return NULL;}
LegendaDocumento *legenda_documento_reter(LegendaDocumento *d){return d;}
void legenda_documento_liberar(LegendaDocumento *d){(void)d;}
const LegendaCue *legenda_documento_dados(const LegendaDocumento *d,int *n){(void)d;*n=0;return NULL;}
const LegendaDocumentoInfo *legenda_documento_info(const LegendaDocumento *d){(void)d;return NULL;}
LegendaDocumento *legenda_documento_de_cues(const LegendaCue *c,int n,const LegendaDocumentoInfo *i){(void)c;(void)n;(void)i;return NULL;}
static void tap(int on){assert(!on);}
int main(void){
 audsync_backend(tap);audsync_formato(AUDSYNC_FMT_PCM);
 assert(audsync_capacidade()==AUDSYNC_CAP_RUNTIME);
 audsync_pedir(1,(LegendaDocumento *)1,300,30000);
 assert(audsync_status().fase==AUDSYNC_FALHOU && audsync_status().motivo==AUDSYNC_M_RUNTIME);
 supported=1;assert(audsync_capacidade()==AUDSYNC_CAP_MODEL);
 audsync_pedir(1,(LegendaDocumento *)1,300,30000);
 assert(audsync_status().motivo==AUDSYNC_M_MODEL);
 assert(!audsync_pcm((int16_t[1]){0},1,0));assert(!created);
 audsync_destruir();puts("audsync production gate: no runtime/model, no DSP fallback or inference");
}
