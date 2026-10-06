#include "autosync.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define AS_MAX_CUES 8000
#define AS_PASSO_MS 25
#define AS_MAX_PICOS 24
#define AS_MIN_SCORE .78
#define AS_MIN_MARGEM .09
#define AS_MIN_REGIAO .72
#define AS_MIN_REG_MARGEM .07
#define AS_EXCLUIDAS 16

typedef struct { double inicio, fim; } Intervalo;
typedef struct { Intervalo *v; int n; double fala, inicio, fim; } Atividade;
typedef struct {
  AutoSyncCancelar cancelar; void *usuario;
  struct timespec inicio; int orcamento, terminou;
} Controle;
typedef struct { int ms; double score, alternativo; } Pico;
static int tempoMs(const struct timespec *inicio) {
  struct timespec agora;clock_gettime(CLOCK_MONOTONIC,&agora);
  double ms=(agora.tv_sec-inicio->tv_sec)*1000.0+(agora.tv_nsec-inicio->tv_nsec)/1000000.0;
  return ms>INT_MAX?INT_MAX:(int)ms;
}
static int parou(Controle *c) {
  if(c->cancelar&&c->cancelar(c->usuario)){c->terminou=AUTOSYNC_SESSION_CHANGED;return 1;}
  if(tempoMs(&c->inicio)>=c->orcamento){c->terminou=AUTOSYNC_BUDGET;return 1;}
  return 0;
}
AutoSyncConfig autosync_config(AutoSyncModo modo) {
  return (AutoSyncConfig){modo,250,60000,modo==AUTOSYNC_THOROUGH?16000:4000};
}
const char *autosync_motivo(AutoSyncMotivo m) {
  static const char *const nomes[]={"accepted","no_reference","incomplete_document",
    "forced_or_signs","sparse_dialogue","repeated_dialogue","low_confidence",
    "ambiguous_peak","region_disagreement","session_changed","analysis_budget",
    "out_of_memory","excluded_reference","invalid_argument"};
  return m>=0&&m<(int)(sizeof nomes/sizeof *nomes)?nomes[m]:"invalid_argument";
}
static int cmpHash(const void *a,const void *b){uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return x<y?-1:x>y;}
static uint64_t textoHash(const char *s,int *letras) {
  uint64_t h=UINT64_C(14695981039346656037);int n=0;
  for(;*s;s++) {unsigned char c=(unsigned char)*s;
    if(c>=128||isalnum(c)){h^=c<128?(unsigned char)tolower(c):c;h*=UINT64_C(1099511628211);n++;}
  }
  *letras=n;return h;
}
static int sinal(const LegendaCue *c) {
  const char *s=c->texto;while(isspace((unsigned char)*s))s++;
  /* Upper/positioned ASS signage and pure SDH/music are not dialogue. */
  if(c->an>=4||(c->posY>=0&&c->resY>0&&c->posY<c->resY*.65f))return 1;
  if(strstr(s,"\xe2\x99\xaa")||strstr(s,"\xe2\x99\xab"))return 1;
  size_t n=strlen(s);
  return n>1&&((s[0]=='['&&s[n-1]==']')||(s[0]=='('&&s[n-1]==')'));
}
static AutoSyncMotivo atividade(const LegendaDocumento *doc,Atividade *a,Controle *c) {
  const LegendaDocumentoInfo *info=legenda_documento_info(doc);
  int n=0,i,usados=0,sinais=0;const LegendaCue *v=legenda_documento_dados(doc,&n);
  uint64_t *hashes=NULL;
  if(!info||!v)return AUTOSYNC_NO_REFERENCE;
  if(!(info->flags&LEGENDA_DOC_COMPLETO)||n>=AS_MAX_CUES)return AUTOSYNC_INCOMPLETE;
  if(info->flags&(LEGENDA_DOC_FORCED|LEGENDA_DOC_SINAIS))return AUTOSYNC_FORCED_SIGNS;
  a->v=calloc((size_t)n,sizeof *a->v);hashes=malloc((size_t)n*sizeof *hashes);
  if(!a->v||!hashes){free(hashes);return AUTOSYNC_MEMORY;}
  for(i=0;i<n;i++) {
    if(!(i%256)&&parou(c)){free(hashes);return (AutoSyncMotivo)c->terminou;}
    int letras=0;double dur=v[i].fim-v[i].inicio;
    if(sinal(&v[i])){sinais++;continue;}
    uint64_t h=textoHash(v[i].texto,&letras);
    if(letras<3||dur<.20||dur>15)continue;
    hashes[usados++]=h;
    if(a->n&&v[i].inicio<=a->v[a->n-1].fim+.02) {
      if(v[i].fim>a->v[a->n-1].fim)a->v[a->n-1].fim=v[i].fim;
    } else a->v[a->n++]=(Intervalo){v[i].inicio,v[i].fim};
  }
  if(sinais*5>n){free(hashes);return AUTOSYNC_FORCED_SIGNS;}
  if(usados<30||a->n<24){free(hashes);return AUTOSYNC_SPARSE;}
  qsort(hashes,(size_t)usados,sizeof *hashes,cmpHash);
  int maior=1,iguais=1,distintos=1;
  for(i=1;i<usados;i++) {
    if(hashes[i]==hashes[i-1]){if(++iguais>maior)maior=iguais;}
    else {iguais=1;distintos++;}
  }
  free(hashes);
  if(maior*4>usados||distintos*2<usados)return AUTOSYNC_REPEATED;
  a->inicio=a->v[0].inicio;a->fim=a->v[a->n-1].fim;
  for(i=0;i<a->n;i++)a->fala+=a->v[i].fim-a->v[i].inicio;
  double span=a->fim-a->inicio;
  if(span<180||a->fala<40||a->fala/span<.08||a->fala/span>.85)return AUTOSYNC_SPARSE;
  return AUTOSYNC_OK;
}
static double tamanho(const Atividade *a,double offset,double ini,double fim) {
  double total=0;
  for(int i=0;i<a->n;i++) {
    double x=fmax(a->v[i].inicio-offset,ini),y=fmin(a->v[i].fim-offset,fim);
    if(y>x)total+=y-x;
  }
  return total;
}
static double score(const Atividade *a,const Atividade *b,int ms,double ini,double fim) {
  int i=0,j=0;double inter=0,off=ms/1000.0;
  double ta=tamanho(a,off,ini,fim),tb=tamanho(b,0,ini,fim);
  if(ta<6||tb<6)return 0;
  while(i<a->n&&j<b->n) {
    double ai=a->v[i].inicio-off,af=a->v[i].fim-off,bi=b->v[j].inicio,bf=b->v[j].fim;
    double x=fmax(fmax(ai,bi),ini),y=fmin(fmin(af,bf),fim);
    if(y>x)inter+=y-x;
    if(af<=bf)i++;else j++;
  }
  /* Chance-adjusted Jaccard activity overlap, avoiding high speech-duty bias. */
  double den=ta+tb-inter,span=fim-ini;
  double azar=ta*tb/span/(ta+tb-ta*tb/span);
  return den>0&&azar<1?fmax(0,(inter/den-azar)/(1-azar)):0;
}
static int primeiro(const Atividade *a,double t) {
  int lo=0,hi=a->n;
  while(lo<hi){int m=(lo+hi)/2;if(a->v[m].inicio<t)lo=m+1;else hi=m;}
  return lo;
}
/* A high average overlap can hide a short cut or drift at the very end. A
 * global correction is allowed only when every dialogue interval's boundaries
 * have corresponding boundaries within the residual tolerance, both ways.
 * Different continuous cue segmentation was already merged above. Deliberate
 * fail-closed policy: translated releases with substantially different timing
 * may be refused even when their average correlation looks convincing. */
static int bordasConcordam(const Atividade *a,const Atividade *b,int ms,int tolerancia,
                           Controle *c,int *erroMs) {
  int j=0;double off=ms/1000.0,tol=tolerancia/1000.0;
  for(int i=0;i<a->n;i++) {
    if(!(i%256)&&parou(c))return 0;
    double ini=a->v[i].inicio-off,fim=a->v[i].fim-off;
    while(j<b->n&&b->v[j].inicio<ini-tol)j++;
    int encontrou=0;
    double erro=tol+1;
    for(int k=j;k<b->n&&b->v[k].inicio<=ini+tol;k++) {
      double atual=fmax(fabs(b->v[k].inicio-ini),fabs(b->v[k].fim-fim));
      if(atual<=tol&&atual<erro){encontrou=1;erro=atual;}
    }
    if(!encontrou)return 0;
    int residual=(int)ceil(erro*1000-1e-6);
    if(residual>*erroMs)*erroMs=residual;
  }
  return 1;
}
static Pico buscar(const Atividade *a,const Atividade *b,const AutoSyncConfig *cfg,
                   double ini,double fim,Controle *c) {
  Pico p={0,0,0};int raio=cfg->raioBuscaMs/AS_PASSO_MS;
  int bins=raio*2+1,*votos=calloc((size_t)bins,sizeof *votos);
  int candidatos[AS_MAX_PICOS],nc=0,i,j;
  if(!votos){c->terminou=AUTOSYNC_MEMORY;return p;}
  int lo=primeiro(a,ini-cfg->raioBuscaMs/1000.0),hi=primeiro(a,fim+cfg->raioBuscaMs/1000.0);
  int passo=(hi-lo)/128;if(passo<1)passo=1;
  for(i=lo;i<hi;i+=passo) {
    if(parou(c)){free(votos);return p;}
    int bj=primeiro(b,a->v[i].inicio-cfg->raioBuscaMs/1000.0);
    for(j=bj;j<b->n&&b->v[j].inicio<=a->v[i].inicio+cfg->raioBuscaMs/1000.0;j++) {
      if(b->v[j].inicio<ini||b->v[j].inicio>fim)continue;
      int bin=(int)lround((a->v[i].inicio-b->v[j].inicio)*1000/AS_PASSO_MS)+raio;
      if(bin>=0&&bin<bins)votos[bin]++;
    }
  }
  for(i=0;i<AS_MAX_PICOS;i++) {
    int melhor=-1;
    for(j=0;j<bins;j++)if(votos[j]>0&&(melhor<0||votos[j]>votos[melhor]))melhor=j;
    if(melhor<0)break;
    candidatos[nc++]=(melhor-raio)*AS_PASSO_MS;
    for(j=melhor-4;j<=melhor+4;j++)if(j>=0&&j<bins)votos[j]=0;
  }
  free(votos);double resultados[AS_MAX_PICOS];
  for(i=0;i<nc;i++) {
    if(parou(c))return p;
    resultados[i]=score(a,b,candidatos[i],ini,fim);
    if(resultados[i]>p.score){p.score=resultados[i];p.ms=candidatos[i];}
  }
  /* Ignore only this peak's fixed shoulder. Changing residual tolerance may
   * never hide a competing peak or lower the confidence criterion. */
  const int distancia=500;
  for(i=0;i<nc;i++)if(abs(candidatos[i]-p.ms)>distancia&&resultados[i]>p.alternativo)
    p.alternativo=resultados[i];
  return p;
}
AutoSyncResultado autosync_comparar(const LegendaDocumento *doc,const LegendaDocumento *ref,
                                   const AutoSyncConfig *config,AutoSyncCancelar cancelar,void *usuario) {
  AutoSyncResultado r={.estado=AUTOSYNC_REJECTED,.motivo=AUTOSYNC_INVALID_ARGUMENT};
  AutoSyncConfig cfg=config?*config:autosync_config(AUTOSYNC_QUICK);
  Controle c={.cancelar=cancelar,.usuario=usuario,.orcamento=cfg.orcamentoMs};
  Atividade a={0},b={0};clock_gettime(CLOCK_MONOTONIC,&c.inicio);
  const LegendaDocumentoInfo *di=legenda_documento_info(doc),*ri=legenda_documento_info(ref);
  if(!di||!ri){r.estado=AUTOSYNC_UNAVAILABLE;r.motivo=AUTOSYNC_NO_REFERENCE;goto fim;}
  r.documento=legenda_documento_hash(doc);r.referencia=legenda_documento_hash(ref);r.sessao=di->sessao;
  if(di->sessao!=ri->sessao){r.estado=AUTOSYNC_CANCELLED;r.motivo=AUTOSYNC_SESSION_CHANGED;goto fim;}
  if((cfg.modo!=AUTOSYNC_QUICK&&cfg.modo!=AUTOSYNC_THOROUGH)||
     cfg.toleranciaMs<50||cfg.toleranciaMs>1000||cfg.raioBuscaMs<1000||
     cfg.raioBuscaMs>120000||cfg.orcamentoMs<1||cfg.orcamentoMs>20000||doc==ref)goto fim;
  if(parou(&c)){r.motivo=(AutoSyncMotivo)c.terminou;goto fim;}
  r.motivo=atividade(doc,&a,&c);if(r.motivo!=AUTOSYNC_OK)goto fim;
  r.motivo=atividade(ref,&b,&c);if(r.motivo!=AUTOSYNC_OK)goto fim;
  if(di->duracaoSeg>0&&ri->duracaoSeg>0&&fabs(di->duracaoSeg-ri->duracaoSeg)>cfg.toleranciaMs/1000.0) {
    r.motivo=AUTOSYNC_REGION_DISAGREEMENT;goto fim;
  }
  double ini=fmin(a.inicio,b.inicio)-cfg.raioBuscaMs/1000.0;
  double final=fmax(a.fim,b.fim)+cfg.raioBuscaMs/1000.0;
  Pico p=buscar(&a,&b,&cfg,ini,final,&c);
  if(c.terminou){r.motivo=(AutoSyncMotivo)c.terminou;goto fim;}
  r.confianca=p.score;r.alternativa=p.alternativo;
  if(p.score<AS_MIN_SCORE){r.motivo=AUTOSYNC_LOW_CONFIDENCE;goto fim;}
  if(p.score-p.alternativo<AS_MIN_MARGEM){r.motivo=AUTOSYNC_AMBIGUOUS;goto fim;}
  if(!bordasConcordam(&a,&b,p.ms,cfg.toleranciaMs,&c,&r.erroMs)||
     !bordasConcordam(&b,&a,-p.ms,cfg.toleranciaMs,&c,&r.erroMs)) {
    r.motivo=c.terminou?(AutoSyncMotivo)c.terminou:AUTOSYNC_REGION_DISAGREEMENT;goto fim;
  }
  int nr=cfg.modo==AUTOSYNC_THOROUGH?6:3;
  double inicio=fmax(a.inicio-p.ms/1000.0,b.inicio),finais=fmin(a.fim-p.ms/1000.0,b.fim);
  if(finais-inicio<180){r.motivo=AUTOSYNC_SPARSE;goto fim;}
  int menor=p.ms,maior=p.ms;
  for(int k=0;k<nr;k++) {
    double x=inicio+(finais-inicio)*k/nr,y=inicio+(finais-inicio)*(k+1)/nr;
    Pico q=buscar(&a,&b,&cfg,x,y,&c);
    if(c.terminou){r.motivo=(AutoSyncMotivo)c.terminou;goto fim;}
    if(q.score<AS_MIN_REGIAO){r.motivo=AUTOSYNC_REGION_DISAGREEMENT;goto fim;}
    if(q.score-q.alternativo<AS_MIN_REG_MARGEM){r.motivo=AUTOSYNC_AMBIGUOUS;goto fim;}
    if(abs(q.ms-p.ms)>cfg.toleranciaMs){r.motivo=AUTOSYNC_REGION_DISAGREEMENT;goto fim;}
    if(q.ms<menor)menor=q.ms;
    if(q.ms>maior)maior=q.ms;
    r.regioes++;
  }
  if(maior-menor>r.erroMs)r.erroMs=maior-menor;
  if(r.erroMs>cfg.toleranciaMs){r.motivo=AUTOSYNC_REGION_DISAGREEMENT;goto fim;}
  r.offsetMs=p.ms;r.estado=AUTOSYNC_ACCEPTED;r.motivo=AUTOSYNC_OK;
fim:
  if(r.motivo==AUTOSYNC_SESSION_CHANGED)r.estado=AUTOSYNC_CANCELLED;
  r.tempoMs=tempoMs(&c.inicio);free(a.v);free(b.v);return r;
}

typedef struct {
  LegendaDocumento *doc,*ref;AutoSyncConfig cfg;AutoSyncResultado resultado;
  uint64_t epoch,excluidas[AS_EXCLUIDAS];int nExcluidas,manual,automatico,pendente;
} Slot;
struct AutoSync {
  pthread_mutex_t trava;pthread_cond_t acordar;pthread_t fio;
  uint64_t sessao;int parar,proximo;Slot slots[2];
};
typedef struct {AutoSync *s;int slot;uint64_t sessao,epoch;} Cancelamento;
static int vigente(void *u) {
  Cancelamento *c=u;AutoSync *s=c->s;int cancelado;
  pthread_mutex_lock(&s->trava);
  cancelado=s->parar||s->sessao!=c->sessao||s->slots[c->slot].epoch!=c->epoch;
  pthread_mutex_unlock(&s->trava);return cancelado;
}
static void limparSlot(Slot *s) {
  legenda_documento_liberar(s->doc);legenda_documento_liberar(s->ref);
  uint64_t epoch=s->epoch+1;memset(s,0,sizeof *s);s->epoch=epoch;
  s->resultado.estado=AUTOSYNC_UNAVAILABLE;s->resultado.motivo=AUTOSYNC_NO_REFERENCE;
}
static void *trabalhar(void *u) {
  AutoSync *s=u;
  for(;;) {
    pthread_mutex_lock(&s->trava);
    while(!s->parar&&!s->slots[0].pendente&&!s->slots[1].pendente)pthread_cond_wait(&s->acordar,&s->trava);
    if(s->parar){pthread_mutex_unlock(&s->trava);return NULL;}
    int k=s->slots[s->proximo].pendente?s->proximo:1-s->proximo;s->proximo=1-k;
    Slot *slot=&s->slots[k];slot->pendente=0;
    LegendaDocumento *doc=legenda_documento_reter(slot->doc),*ref=legenda_documento_reter(slot->ref);
    AutoSyncConfig cfg=slot->cfg;Cancelamento token={s,k,s->sessao,slot->epoch};
    pthread_mutex_unlock(&s->trava);
    AutoSyncResultado resultado=autosync_comparar(doc,ref,&cfg,vigente,&token);
    pthread_mutex_lock(&s->trava);
    int publicado=!s->parar&&s->sessao==token.sessao&&slot->epoch==token.epoch;
    if(publicado) {
      slot->resultado=resultado;
      if(resultado.estado==AUTOSYNC_ACCEPTED)slot->automatico=resultado.offsetMs;
    }
    pthread_mutex_unlock(&s->trava);
    /* stderr may block when an external log consumer stalls. Never hold the
     * state lock while writing logs or freeing retained worker documents. */
    if(publicado) {
      fprintf(stderr,"[autosync] subtitle_sync %s reason=%s slot=%d offset_ms=%d confidence=%.3f elapsed_ms=%d\n",
        resultado.estado==AUTOSYNC_ACCEPTED?"accepted":"rejected",autosync_motivo(resultado.motivo),k,
        resultado.offsetMs,resultado.confianca,resultado.tempoMs);
    }
    legenda_documento_liberar(doc);legenda_documento_liberar(ref);
  }
}
AutoSync *autosync_criar(void) {
  AutoSync *s=calloc(1,sizeof *s);if(!s)return NULL;
  for(int i=0;i<2;i++)s->slots[i].resultado=(AutoSyncResultado){
    .estado=AUTOSYNC_UNAVAILABLE,.motivo=AUTOSYNC_NO_REFERENCE};
  if(pthread_mutex_init(&s->trava,NULL)){free(s);return NULL;}
  if(pthread_cond_init(&s->acordar,NULL)){pthread_mutex_destroy(&s->trava);free(s);return NULL;}
  if(pthread_create(&s->fio,NULL,trabalhar,s)) {
    pthread_cond_destroy(&s->acordar);pthread_mutex_destroy(&s->trava);free(s);return NULL;
  }
  return s;
}
void autosync_destruir(AutoSync *s) {
  if(!s)return;
  pthread_mutex_lock(&s->trava);s->parar=1;pthread_cond_signal(&s->acordar);
  pthread_mutex_unlock(&s->trava);pthread_join(s->fio,NULL);
  limparSlot(&s->slots[0]);limparSlot(&s->slots[1]);
  pthread_cond_destroy(&s->acordar);pthread_mutex_destroy(&s->trava);free(s);
}
void autosync_iniciar(AutoSync *s,uint64_t sessao) {
  if(!s)return;
  pthread_mutex_lock(&s->trava);s->sessao=sessao;
  limparSlot(&s->slots[0]);limparSlot(&s->slots[1]);pthread_mutex_unlock(&s->trava);
}
static int valido(AutoSync *s,int slot){return s&&slot>=0&&slot<2;}
int autosync_selecionar(AutoSync *s,int k,LegendaDocumento *doc) {
  if(!valido(s,k))return 0;
  pthread_mutex_lock(&s->trava);const LegendaDocumentoInfo *info=legenda_documento_info(doc);
  if(doc&&(!info||info->sessao!=s->sessao)){pthread_mutex_unlock(&s->trava);return 0;}
  Slot *slot=&s->slots[k];
  if(doc==slot->doc){pthread_mutex_unlock(&s->trava);return 1;}
  legenda_documento_reter(doc);limparSlot(slot);slot->doc=doc;
  pthread_mutex_unlock(&s->trava);return 1;
}
static int permitido(const Slot *slot,const LegendaDocumento *ref) {
  uint64_t h=legenda_documento_hash(ref);
  if(!h||slot->nExcluidas>=AS_EXCLUIDAS)return 0;
  for(int i=0;i<slot->nExcluidas;i++)if(slot->excluidas[i]==h)return 0;
  return 1;
}
int autosync_referencia_permitida(AutoSync *s,int k,const LegendaDocumento *ref) {
  if(!valido(s,k))return 0;
  pthread_mutex_lock(&s->trava);const LegendaDocumentoInfo *info=legenda_documento_info(ref);
  int ok=info&&info->sessao==s->sessao&&permitido(&s->slots[k],ref);
  pthread_mutex_unlock(&s->trava);return ok;
}
int autosync_solicitar(AutoSync *s,int k,LegendaDocumento *ref,const AutoSyncConfig *cfg) {
  if(!valido(s,k))return 0;
  pthread_mutex_lock(&s->trava);Slot *slot=&s->slots[k];const LegendaDocumentoInfo *info=legenda_documento_info(ref);
  if(!slot->doc||!info||info->sessao!=s->sessao||!permitido(slot,ref)) {
    pthread_mutex_unlock(&s->trava);return 0;
  }
  legenda_documento_reter(ref);legenda_documento_liberar(slot->ref);slot->ref=ref;
  slot->cfg=cfg?*cfg:autosync_config(AUTOSYNC_QUICK);slot->epoch++;slot->pendente=1;
  slot->automatico=0;
  slot->resultado=(AutoSyncResultado){.estado=AUTOSYNC_ANALYSING,.sessao=s->sessao,
    .documento=legenda_documento_hash(slot->doc),.referencia=legenda_documento_hash(ref)};
  pthread_cond_signal(&s->acordar);pthread_mutex_unlock(&s->trava);return 1;
}
void autosync_cancelar(AutoSync *s,int k) {
  if(!valido(s,k))return;
  pthread_mutex_lock(&s->trava);Slot *slot=&s->slots[k];
  slot->epoch++;slot->pendente=0;slot->resultado.estado=AUTOSYNC_CANCELLED;
  slot->resultado.motivo=AUTOSYNC_SESSION_CHANGED;pthread_mutex_unlock(&s->trava);
}
void autosync_cancelar_pendente(AutoSync *s,int k) {
  if(!valido(s,k))return;
  pthread_mutex_lock(&s->trava);Slot *slot=&s->slots[k];
  slot->epoch++;slot->pendente=0;
  if(slot->resultado.estado!=AUTOSYNC_ACCEPTED) {
    slot->resultado.estado=AUTOSYNC_CANCELLED;
    slot->resultado.motivo=AUTOSYNC_SESSION_CHANGED;
  }
  pthread_mutex_unlock(&s->trava);
}
int autosync_manual(AutoSync *s,int k,int atrasoMs) {
  if(!valido(s,k)||atrasoMs < -120000||atrasoMs>120000)return 0;
  pthread_mutex_lock(&s->trava);s->slots[k].manual=atrasoMs;pthread_mutex_unlock(&s->trava);return 1;
}
int autosync_offset_ms(AutoSync *s,int k) {
  if(!valido(s,k))return 0;
  pthread_mutex_lock(&s->trava);int ms=s->slots[k].manual+s->slots[k].automatico;
  pthread_mutex_unlock(&s->trava);return ms;
}
void autosync_desfazer(AutoSync *s,int k) {
  if(!valido(s,k))return;
  pthread_mutex_lock(&s->trava);Slot *slot=&s->slots[k];
  slot->epoch++;slot->pendente=0;slot->automatico=0;
  slot->resultado.estado=AUTOSYNC_CANCELLED;slot->resultado.motivo=AUTOSYNC_SESSION_CHANGED;
  pthread_mutex_unlock(&s->trava);
}
int autosync_tentar_outra(AutoSync *s,int k) {
  if(!valido(s,k))return 0;
  pthread_mutex_lock(&s->trava);Slot *slot=&s->slots[k];uint64_t h=legenda_documento_hash(slot->ref);
  int ok=h&&permitido(slot,slot->ref)&&slot->nExcluidas<AS_EXCLUIDAS;
  if(ok)slot->excluidas[slot->nExcluidas++]=h;
  slot->epoch++;slot->pendente=0;slot->automatico=0;
  slot->resultado.estado=AUTOSYNC_UNAVAILABLE;slot->resultado.motivo=AUTOSYNC_EXCLUDED_REFERENCE;
  pthread_mutex_unlock(&s->trava);return ok;
}
AutoSyncResultado autosync_estado(AutoSync *s,int k) {
  AutoSyncResultado r={.estado=AUTOSYNC_UNAVAILABLE,.motivo=AUTOSYNC_INVALID_ARGUMENT};
  if(!valido(s,k))return r;
  pthread_mutex_lock(&s->trava);r=s->slots[k].resultado;pthread_mutex_unlock(&s->trava);return r;
}
