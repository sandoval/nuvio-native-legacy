// Cola de sessao do AutoSync (src/legsync.c): geracao, download da principal,
// referencia embutida real (MKV de ffmpeg), offset aplicado UMA vez, desfazer,
// outra referencia, seek, troca de fonte, troca de dono e teardown.
//   bash tests/legsync.sh     SANITIZE=1 / SANITIZE=thread
#include "legsync.h"
#include "autosync.h"
#include "rede.h"
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *DIR;
// legsyncui.c (provedor do seletor do F04) sem SDL: stubs do que ele chama.
#include "legendasui.h"
static LegendasSyncProvider prov; static int provLigado;
void legendasui_definir_sync(const LegendasSyncProvider *p) { provLigado = p != NULL; if (p) prov = *p; }
const char *i18n(const char *s) { return s; }
void plrui_decimal(char *s) { (void)s; }
static const char *canalId = "";
const char *player_id_canal(void) { return canalId; }
static int capacidades = REDE_CAP_JOB;
unsigned rede_pedido_capacidades(void) { return (unsigned)capacidades; }
int rede_pedir(const RedePedido *p, RedeResposta *r) { (void)p; memset(r, 0, sizeof *r); r->erro = REDE_INDISPONIVEL; return 0; }
void rede_resposta_limpar(RedeResposta *r) { free(r->corpo); free(r->cabecalhos); r->corpo = r->cabecalhos = NULL; }

static char *arquivo(const char *nome, long *n) {
  char c[700]; FILE *f; char *b; long t;
  snprintf(c, sizeof c, "%s/%s", DIR, nome); f = fopen(c, "rb"); if (!f) return NULL;
  fseek(f, 0, SEEK_END); t = ftell(f); fseek(f, 0, SEEK_SET);
  b = malloc((size_t)t + 1); if (fread(b, 1, (size_t)t, f) != (size_t)t) { fclose(f); free(b); return NULL; }
  fclose(f); b[t] = 0; if (n) *n = t; return b;
}
// Download da legenda externa (legenda.c): "ext://atraso_ms/arquivo".
char *rede_baixar_bin(const char *url, int segundos, long *n) {
  int ms = 0; char nome[200]; (void)segundos;
  if (sscanf(url, "ext://%d/%199s", &ms, nome) != 2) return NULL;
  if (ms) usleep((useconds_t)ms * 1000);
  return arquivo(nome, n);
}

static _Atomic int travar, atrasoLeitorUs;   // escritos pelo teste, lidos pelo fio do legref
static int pedidosLeitor, paradas;   // paradas: leituras soltas pelo `parar` (cancelamento)
static pthread_mutex_t LM = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *lerMkv(void *u, const char *url, long long ini, long n, long *tam, int *status,
                             int (*parar)(void *), void *pu) {
  const char *nome = strrchr(url, '/'); char c[700]; FILE *f; unsigned char *b; long long total;
  (void)u;
  pthread_mutex_lock(&LM); pedidosLeitor++; pthread_mutex_unlock(&LM);
  while (travar && !parar(pu)) usleep(1000);
  if (atrasoLeitorUs) usleep((useconds_t)atrasoLeitorUs);
  if (parar(pu)) { pthread_mutex_lock(&LM); paradas++; pthread_mutex_unlock(&LM); return NULL; }
  snprintf(c, sizeof c, "%s/%s", DIR, nome ? nome + 1 : url);
  f = fopen(c, "rb"); if (!f) { *status = 404; return NULL; }
  fseek(f, 0, SEEK_END); total = ftell(f);
  if (ini + n > total) n = (long)(total - ini);
  b = malloc((size_t)n + 1); fseek(f, (long)ini, SEEK_SET); *tam = (long)fread(b, 1, (size_t)n, f); fclose(f);
  *status = 206; return b;
}

static unsigned agora = 1000;
#define LS_AUTO_TETO_TESTE 45000u   // src/legsync.c LS_AUTO_TETO_MS
static void passo(const char *url, int sensivel) { agora += 50; legsync_passo(url, 30.0, 60.0, sensivel, agora); }
static LegSyncVisao esperarFase(const char *url, LegSyncFase f) {
  LegSyncVisao v;
  for (int i = 0; i < 4000; i++) { passo(url, 0); v = legsync_visao(0); if (v.fase == f) return v; usleep(1000); }
  fprintf(stderr, "esperava fase %d, ficou %d motivo %d\n", f, v.fase, v.motivo);
  assert(!"timeout");
  return v;
}

static const char *MKV = "https://cdn.example/a/ff.mkv";
static const char *MKV2 = "https://cdn.example/b/mm.mkv";


// --- R4: trocador de teste (faixas.c de mentira) ---------------------------------
static const char *cand[8]; static int nCand, trocas, voltou; static const char *candIdioma = "pt";
static const char *MKV3 = "https://cdn.example/c/semcues.mkv";
static int trocadorTeste(const char *idioma, const uint64_t *tent, int n, int voltar, char *nome, unsigned tam) {
  char url[300];
  if (voltar) { voltou++; snprintf(nome, tam, "orig"); legsync_primaria_externa("ext://0/ext_ruim.srt", "pt", "Ruim"); return 1; }
  if (strcmp(idioma, candIdioma)) return 0;
  for (int i = 0; i < nCand; i++) {
    snprintf(url, sizeof url, "ext://0/%s", cand[i]);
    int jaFoi = 0; for (int k = 0; k < n; k++) if (tent[k] == legsync_hash_url(url)) jaFoi = 1;
    if (jaFoi) continue;
    trocas++;
    snprintf(nome, tam, "Prov-%s", cand[i]);
    legsync_primaria_externa(url, idioma, "Prov");
    return 1;
  }
  return 0;
}
// Cues de duracao e intervalo irregulares: nenhum atraso fecha com a referencia.
static void gerarRuim(const char *nome) {
  char c[700]; FILE *f; unsigned x = 12345u; double t = 5.0;
  snprintf(c, sizeof c, "%s/%s", DIR, nome); f = fopen(c, "wb"); assert(f);
  for (int i = 1; i <= 160; i++) {
    double d; int h, m, s, ms, h2, m2, s2, ms2;
    x = x * 1103515245u + 12345u; t += 0.4 + (double)((x >> 16) % 5000) / 1000.0;
    x = x * 1103515245u + 12345u; d = 0.3 + (double)((x >> 16) % 3000) / 1000.0;
    h = (int)(t / 3600); m = (int)(t / 60) % 60; s = (int)t % 60; ms = (int)((t - (int)t) * 1000);
    h2 = (int)((t + d) / 3600); m2 = (int)((t + d) / 60) % 60; s2 = (int)(t + d) % 60; ms2 = (int)(((t + d) - (int)(t + d)) * 1000);
    fprintf(f, "%d\n%02d:%02d:%02d,%03d --> %02d:%02d:%02d,%03d\nTexto qualquer %d\n\n", i, h, m, s, ms, h2, m2, s2, ms2, i);
  }
  fclose(f);
}
static LegSyncVisao esperarAuto(const char *url, int autoFase) {
  LegSyncVisao v;
  for (int i = 0; i < 6000; i++) { passo(url, 0); v = legsync_visao(0); if (v.autoFase == autoFase) return v; usleep(1000); }
  fprintf(stderr, "esperava autoFase %d, ficou %d (fase %d motivo %d)\n", autoFase, v.autoFase, v.fase, v.motivo);
  assert(!"timeout");
  return v;
}

// O documento de referencia em `t` e o que a pessoa ve com o offset total.
static void conferirNaTela(int total) {
  long n; char *b = arquivo("emb.srt", &n); LegendaDocumentoInfo i = { .flags = LEGENDA_DOC_COMPLETO };
  LegendaDocumento *emb = legenda_documento_bytes(b, n, &i); int casou = 0;
  free(b);
  for (double t = 30; t < 590; t += 0.37) {
    LegendaCue a, e;
    int na = legenda_cues(t, total, &a, 1), ne = legenda_documento_cues(emb, t, 0, &e, 1);
    if (na && ne) {
      // A externa e a traducao "Fala traduzida numero N"; o embutido "Line number N".
      int x = -1, y = -2; sscanf(a.texto, "Fala traduzida numero %d", &x); sscanf(e.texto, "Line number %d", &y);
      if (x != y) fprintf(stderr, "t=%.2f '%s' x '%s'\n", t, a.texto, e.texto);
      assert(x == y); casou++;
    }
  }
  assert(casou > 50);
  legenda_documento_liberar(emb);
}

// O mesmo para o FILME de 2 h (filme_emb.srt x filme_ext_mais2500.srt).
static void conferirFilme(int total) {
  long n; char *b = arquivo("filme_emb.srt", &n); LegendaDocumentoInfo i = { .flags = LEGENDA_DOC_COMPLETO };
  LegendaDocumento *emb = legenda_documento_bytes(b, n, &i); int casou = 0, errou = 0;
  free(b);
  for (double t = 60; t < 7150; t += 1.37) {
    LegendaCue a, e;
    int na = legenda_cues(t, total, &a, 1), ne = legenda_documento_cues(emb, t, 0, &e, 1);
    if (na && ne) {
      int x = -1, y = -2; sscanf(a.texto, "Fala traduzida numero %d", &x); sscanf(e.texto, "Film line %d", &y);
      if (x == y) casou++; else errou++;
    } else if (na != ne) errou++;
  }
  if (errou) fprintf(stderr, "filme: %d instantes com a fala errada na tela (offset %d)\n", errou, total);
  assert(casou > 500 && !errou);
  legenda_documento_liberar(emb);
}

// A ilha de narracao (legsync_pil_passo): "Sincronizando…" curto, fechada
// enquanto roda, UM aviso final (ok ou nao sincronizada), nada ao cancelar.
static LegSyncVisao pv(int fase, int autoFase) { LegSyncVisao x; memset(&x, 0, sizeof x); x.fase = fase; x.autoFase = autoFase; x.offsetAutoMs = 2500; return x; }
static void pilula_sequencia(int fim, int ok, int cancela) {
  LegSyncPil p; char t[200]; unsigned now = 1000; int r, aberturas = 0, ant = 0, visivel;
  LegSyncVisao rodando = pv(LEGSYNC_LENDO, 1), v;
  memset(&p, 0, sizeof p); t[0] = 0;
  p.rastreia = 1; p.estado = LEGSYNC_PIL_PROCURANDO; p.desde = now; p.iniciou = now;
  // Procurando -> Sincronizando
  now += 1000; LegSyncVisao ag = pv(LEGSYNC_AGUARDANDO, 0); legsync_pil_passo(&p, &ag, now, "OS", t, sizeof t); assert(p.estado == LEGSYNC_PIL_PROCURANDO);
  now += 100; legsync_pil_passo(&p, &rodando, now, "OS", t, sizeof t); assert(p.estado == LEGSYNC_PIL_SINCRONIZANDO);
  // 3 s depois a ilha fecha (espera) e fica fechada por minutos
  now += LEGSYNC_PIL_SINC_MS - 1; legsync_pil_passo(&p, &rodando, now, "OS", t, sizeof t); assert(p.estado == LEGSYNC_PIL_SINCRONIZANDO);
  now += 2; r = legsync_pil_passo(&p, &rodando, now, "OS", t, sizeof t);
  assert((r & LEGSYNC_PIL_ESCONDEU) && p.estado == LEGSYNC_PIL_OFF && p.espera);
  for (int i = 0; i < 400; i++) {
    now += 500; r = legsync_pil_passo(&p, &rodando, now, "OS", t, sizeof t);
    assert(!r && p.estado == LEGSYNC_PIL_OFF);   // nenhuma reabertura intermediaria
  }
  if (cancela) {
    v = pv(LEGSYNC_INDISPONIVEL, 0);
    for (int i = 0; i < 20; i++) { now += 500; r = legsync_pil_passo(&p, &v, now, "OS", t, sizeof t); assert(!r && p.estado == LEGSYNC_PIL_OFF); }
    assert(!p.espera); return;
  }
  v = ok ? pv(LEGSYNC_ACEITA, 2) : pv(LEGSYNC_RECUSADA, fim);
  now += 500; r = legsync_pil_passo(&p, &v, now, "OS", t, sizeof t);
  assert((r & LEGSYNC_PIL_FINAL_TARDE) && p.estado == LEGSYNC_PIL_APLICADA && !p.espera);
  assert(p.final == (ok ? 1 : 0));
  assert(ok ? strstr(t, "sincronizada") && strstr(t, "+2") : strstr(t, "n\xc3\xa3o sincronizada") != NULL);
  // o aviso fica o tempo normal e some; sem nova abertura
  unsigned dur = ok ? LEGSYNC_PIL_APLICADA_MS : LEGSYNC_PIL_SEMSYNC_MS;
  now += dur - 10; r = legsync_pil_passo(&p, &v, now, "OS", t, sizeof t); assert(!(r & LEGSYNC_PIL_ZERAR));
  now += 20; r = legsync_pil_passo(&p, &v, now, "OS", t, sizeof t); assert(r & LEGSYNC_PIL_ZERAR);
}

int main(int argc, char **argv) {
  LegSyncVisao v; int casos = 0;
  DIR = argc > 1 ? argv[1] : "/tmp/nv-legref-fx";
  legsync_teste_leitor(lerMkv, NULL);
  legsync_teste_auto(0);   // as secoes 1..11 testam as ACOES manuais; o automatico e a secao 12

  // Antes de criar e no slot 1: honesto.
  assert(legsync_visao(0).fase == LEGSYNC_INDISPONIVEL);
  assert(legsync_offset_ms(250) == 250);
  legsync_iniciar(MKV);
  assert(legsync_visao(1).fase == LEGSYNC_DEPOIS);
  assert(legsync_visao(0).motivo == LEGSYNC_M_SEM_EXTERNA);
  legsync_primaria_outra(1); assert(legsync_visao(0).motivo == LEGSYNC_M_EMBUTIDA);
  assert(!legsync_acao(LEGSYNC_ACAO_RAPIDA)); casos++;

  // 1. Externa +2,5 s; Rapida le a faixa embutida e aceita +2500.
  legsync_primaria_externa("ext://0/ext_mais2500.srt", "pt", "Provedor");
  v = esperarFase(MKV, LEGSYNC_PRONTA);
  assert(legsync_offset_ms(250) == 250);          // nada aceito ainda: so o manual
  assert(legsync_acao(LEGSYNC_ACAO_RAPIDA));
  v = esperarFase(MKV, LEGSYNC_ACEITA);
  assert(abs(v.offsetAutoMs - 2500) <= 25);
  assert(!strcmp(v.idiomaRef, "eng"));
  // Offset total = manual + automatico, UMA vez; o manual muda, o automatico fica.
  assert(legsync_offset_ms(250) == 250 + v.offsetAutoMs);
  assert(legsync_offset_ms(-500) == -500 + v.offsetAutoMs);
  assert(legsync_offset_ms(0) == v.offsetAutoMs);
  conferirNaTela(legsync_offset_ms(0));            // a fala certa no instante certo
  casos++;

  // 2. Desfazer: so o manual volta a valer.
  assert(legsync_acao(LEGSYNC_ACAO_DESFAZER));
  v = legsync_visao(0); assert(v.fase == LEGSYNC_DESFEITA && v.offsetAutoMs == 0);
  assert(legsync_offset_ms(250) == 250); casos++;

  // 3. Completa com a mesma referencia (sem reler o arquivo): aceita de novo.
  { int p0 = pedidosLeitor; assert(legsync_acao(LEGSYNC_ACAO_COMPLETA));
    v = esperarFase(MKV, LEGSYNC_ACEITA); assert(abs(v.offsetAutoMs - 2500) <= 25 && pedidosLeitor == p0); casos++; }

  // 4. Seek durante a analise: cancela, mostra pausada e retoma sozinha.
  { int pegou = 0;
    for (int k = 0; k < 200 && !pegou; k++) {
      legsync_acao(LEGSYNC_ACAO_DESFAZER);
      assert(legsync_acao(LEGSYNC_ACAO_COMPLETA));
      passo(MKV, 1);
      v = legsync_visao(0);
      if (v.fase == LEGSYNC_PAUSADA) pegou = 1;
      else esperarFase(MKV, LEGSYNC_ACEITA);
    }
    assert(pegou);
    assert(legsync_offset_ms(0) == 0);           // nada aplicado enquanto pausada
    for (int k = 0; k < 10; k++) { passo(MKV, 1); assert(legsync_visao(0).fase == LEGSYNC_PAUSADA); }
    v = esperarFase(MKV, LEGSYNC_ACEITA); assert(abs(v.offsetAutoMs - 2500) <= 25); casos++; }

  // 5. Outra referencia: ff.mkv so tem uma faixa de texto -> sem outra, offset zerado.
  assert(legsync_acao(LEGSYNC_ACAO_OUTRA));
  v = esperarFase(MKV, LEGSYNC_INDISPONIVEL);
  assert(v.motivo == LEGSYNC_M_SEM_OUTRA && v.acoes == 0 && legsync_offset_ms(100) == 100); casos++;

  // 6. Troca de fonte no meio da sessao (mm.mkv): geracao nova, a escolha fica,
  //    o automatico zera; a outra faixa (ASS) serve de referencia.
  passo(MKV2, 0);
  v = legsync_visao(0); assert(v.fase == LEGSYNC_PRONTA && legsync_offset_ms(0) == 0);
  // A principal e "pt": a faixa 4 (pt) vem primeiro. Ela e a propria externa
  // remuxada (+2,5 s), entao contra ela o par ja esta junto: aceito ~0.
  assert(legsync_acao(LEGSYNC_ACAO_RAPIDA));
  v = esperarFase(MKV2, LEGSYNC_ACEITA); assert(abs(v.offsetAutoMs) <= 25);
  // Outra referencia: a faixa 3 (ASS ingles, a do video) -> +2500; o letreiro nunca.
  assert(legsync_acao(LEGSYNC_ACAO_OUTRA));
  v = esperarFase(MKV2, LEGSYNC_ACEITA); assert(abs(v.offsetAutoMs - 2500) <= 25); casos++;

  // 7. Dono do overlay mudou por fora (desligar): o automatico nao vale mais.
  legsync_acao(LEGSYNC_ACAO_OUTRA);   // faixa 4 excluida -> volta a nada
  legsync_primaria_externa("ext://0/ext_menos1200.srt", "en", "Outro");
  esperarFase(MKV2, LEGSYNC_PRONTA);
  assert(legsync_acao(LEGSYNC_ACAO_RAPIDA));
  v = esperarFase(MKV2, LEGSYNC_ACEITA); assert(abs(v.offsetAutoMs + 1200) <= 25);
  legenda_desligar();
  assert(legsync_offset_ms(300) == 300 && legsync_visao(0).motivo == LEGSYNC_M_SEM_EXTERNA); casos++;

  // 8. Download atrasado de uma escolha antiga nao vira a principal.
  legsync_primaria_externa("ext://300/ext_mais2500.srt", "pt", "Lenta");
  legsync_primaria_externa("ext://0/ext_menos1200.srt", "en", "Rapida");
  esperarFase(MKV2, LEGSYNC_PRONTA);
  usleep(400000);   // a lenta chega agora e e descartada
  assert(legsync_acao(LEGSYNC_ACAO_RAPIDA));
  v = esperarFase(MKV2, LEGSYNC_ACEITA); assert(abs(v.offsetAutoMs + 1200) <= 25); casos++;

  // 9. Leitura da referencia pausa no seek e com buffer curto.
  legsync_iniciar(MKV);
  legsync_primaria_externa("ext://0/ext_mais2500.srt", "pt", "P");
  esperarFase(MKV, LEGSYNC_PRONTA);
  atrasoLeitorUs = 3000;
  assert(legsync_acao(LEGSYNC_ACAO_RAPIDA));
  usleep(30000); passo(MKV, 1); usleep(20000);
  { int p0, p1; pthread_mutex_lock(&LM); p0 = pedidosLeitor; pthread_mutex_unlock(&LM);
    for (int k = 0; k < 6; k++) { passo(MKV, 1); usleep(20000); }
    pthread_mutex_lock(&LM); p1 = pedidosLeitor; pthread_mutex_unlock(&LM);
    assert(p1 <= p0 + 1);
    agora += 50; legsync_passo(MKV, 30, 5.0, 0, agora);    // buffer de 5 s: ainda pausa
    usleep(60000);
    pthread_mutex_lock(&LM); assert(pedidosLeitor <= p1 + 1); pthread_mutex_unlock(&LM); }
  atrasoLeitorUs = 0;
  v = esperarFase(MKV, LEGSYNC_ACEITA); casos++;

  // 9b. CANCELAMENTO com a leitura da referencia PRESA no meio: fechar o
  //     player, trocar a fonte, trocar a faixa (embutida, outra externa,
  //     desligar). Cada um precisa soltar o Range em curso pelo `parar`, nao
  //     aplicar nada depois, e o automatico nunca vazar para a escolha nova.
  for (int caso = 0; caso < 5; caso++) {
    int p0, pa0, ok = 0;
    legsync_iniciar(MKV);
    legsync_primaria_externa("ext://0/ext_mais2500.srt", "pt", "P");
    esperarFase(MKV, LEGSYNC_PRONTA);
    travar = 1;
    pthread_mutex_lock(&LM); p0 = pedidosLeitor; pa0 = paradas; pthread_mutex_unlock(&LM);
    assert(legsync_acao(LEGSYNC_ACAO_RAPIDA));
    for (int k = 0; k < 2000; k++) {   // o leitor entrou e esta preso
      pthread_mutex_lock(&LM); ok = pedidosLeitor > p0; pthread_mutex_unlock(&LM);
      if (ok) break; usleep(1000);
    }
    assert(ok); passo(MKV, 0); assert(legsync_visao(0).fase == LEGSYNC_LENDO);
    switch (caso) {
      case 0: legsync_encerrar(); break;                         // player fechou
      case 1: passo(MKV2, 0); break;                             // fonte trocou
      case 2: legsync_primaria_outra(1); break;                  // foi para a embutida
      case 3: legsync_primaria_externa("ext://0/ext_menos1200.srt", "en", "Q"); break;  // outra externa
      case 4: legsync_primaria_outra(0); break;                  // desligou
    }
    ok = 0;
    for (int k = 0; k < 2000; k++) {
      pthread_mutex_lock(&LM); ok = paradas > pa0; pthread_mutex_unlock(&LM);
      if (ok) break; usleep(1000);
    }
    assert(ok);                                                  // Range solto pelo cancelamento
    travar = 0;
    for (int k = 0; k < 300; k++) {                              // nada atrasado chega a ser aplicado
      passo(caso == 1 ? MKV2 : MKV, 0);
      v = legsync_visao(0);
      assert(v.fase != LEGSYNC_ACEITA && v.fase != LEGSYNC_LENDO && v.fase != LEGSYNC_ANALISANDO);
      assert(legsync_offset_ms(40) == 40);
      usleep(1000);
    }
    if (caso == 0) assert(v.fase == LEGSYNC_INDISPONIVEL && v.motivo == LEGSYNC_M_SEM_EXTERNA);
    if (caso == 1) assert(v.fase == LEGSYNC_PRONTA);             // a escolha da pessoa fica
    if (caso == 2) assert(v.motivo == LEGSYNC_M_EMBUTIDA);
    if (caso == 3) assert(v.fase == LEGSYNC_PRONTA);             // a nova escolha, sem automatico herdado
    if (caso == 4) assert(v.motivo == LEGSYNC_M_SEM_EXTERNA);
  }
  // 9c. "Sincronizando..." para sempre (foto da TV, 04/10): leitura da referencia
  //     presa, nada fecha o plano. Passado o teto o plano desiste, a legenda
  //     fica como esta e o estado e "nao deu" (autoFase 3), nao "trabalhando".
  legsync_teste_auto(1);
  travar = 1;
  legsync_iniciar(MKV);
  legsync_primaria_externa("ext://0/ext_mais2500.srt", "pt", "P");
  for (int k = 0; k < 40; k++) { passo(MKV, 0); usleep(2000); }
  assert(legsync_visao(0).autoFase == 1);
  agora += 50000;
  passo(MKV, 0);
  assert(legsync_visao(0).autoFase == 3);
  { char t[200]; v = legsync_visao(0);                     // a pilula nao pode dizer "ok"
    assert(!legsync_pilula_final(&v, "P", t, sizeof t) && strstr(t, "n\xc3\xa3o sincronizada") && legsync_offset_ms(0) == 0); }
  travar = 0; legsync_encerrar(); legsync_teste_auto(0); casos++;
  // Fechar o player LOGO depois de pedir a analise (referencia ja lida): o
  // resultado da sessao velha nunca aparece na sessao nova do mesmo arquivo.
  for (int k = 0; k < 20; k++) {
    legsync_iniciar(MKV);
    legsync_primaria_externa("ext://0/ext_mais2500.srt", "pt", "P");
    esperarFase(MKV, LEGSYNC_PRONTA);
    assert(legsync_acao(LEGSYNC_ACAO_RAPIDA));
    if (k & 1) esperarFase(MKV, LEGSYNC_ACEITA);
    legsync_encerrar();
    legsync_iniciar(MKV);
    legsync_primaria_externa("ext://0/ext_mais2500.srt", "pt", "P");
    for (int j = 0; j < 100; j++) {
      passo(MKV, 0); v = legsync_visao(0);
      assert(v.fase != LEGSYNC_ACEITA && legsync_offset_ms(0) == 0);
      usleep(500);
    }
  }
  casos++;

  // 9c. PROVEDOR do seletor de legendas (legsyncui.c -> legendasui): linha so
  //     no slot principal, so com externa ativa, nunca em canal ao vivo; as
  //     acoes da linha sao as de agora e executar roda a escolhida.
  { const char *rot[8]; int n;
    legsync_ui_ligar(); assert(provLigado);
    legsync_iniciar(MKV);
    assert(!prov.estado(0, prov.u) && !prov.estado(1, prov.u));        // sem externa: sem linha
    legsync_primaria_outra(1); assert(!prov.estado(0, prov.u));       // embutida: sem linha
    legsync_primaria_externa("ext://0/ext_mais2500.srt", "pt", "P");
    esperarFase(MKV, LEGSYNC_PRONTA);
    assert(prov.estado(0, prov.u) && !prov.estado(1, prov.u));         // segundo idioma: depois
    canalId = "xtream:1:2"; assert(!prov.estado(0, prov.u)); canalId = "";
    assert(strstr(prov.estado(0, prov.u), "Sincronizando"));            // R4: uma linha so, sem menu
    assert(prov.acoes(0, rot, 8, prov.u) == 0);                         // nada a escolher antes de sincronizar
    assert(prov.acoes(1, rot, 8, prov.u) == 0);
    assert(legsync_acao(LEGSYNC_ACAO_COMPLETA));
    v = esperarFase(MKV, LEGSYNC_ACEITA); assert(abs(v.offsetAutoMs - 2500) <= 25);
    assert(!strcmp(prov.estado(0, prov.u), "Sincronizada"));
    n = prov.acoes(0, rot, 8, prov.u);
    assert(n == 1 && !strcmp(rot[0], "Desfazer"));
    prov.executar(0, 0, prov.u);                                        // Desfazer
    assert(legsync_visao(0).fase == LEGSYNC_DESFEITA && legsync_offset_ms(0) == 0);
    prov.executar(0, 99, prov.u);                                       // indice velho: nada
    casos++; }

  // 12. R4: SINCRONIA AUTOMATICA, sem a pessoa operar nada.
  legsync_teste_auto(1);
  gerarRuim("ext_ruim.srt"); gerarRuim("ext_ruim2.srt");
  legsync_definir_trocador(trocadorTeste);
  { const char *rot[8];
    // 12a. A boa de primeira: sincroniza sozinha, sem trocar nada.
    legsync_iniciar(MKV); trocas = 0;
    legsync_primaria_externa("ext://0/ext_mais2500.srt", "pt", "Boa");
    v = esperarAuto(MKV, 2);
    assert(v.fase == LEGSYNC_ACEITA && abs(v.offsetAutoMs - 2500) <= 25 && !v.autoTrocou && trocas == 0);
    // O que o renderer usa (player.c: legenda_cues(pos, legsync_offset_ms(manual)))
    // e o aceito, e a pilula diz o numero que esta valendo.
    conferirNaTela(legsync_offset_ms(0));
    { char t[200];
      assert(legsync_pilula_final(&v, "Boa", t, sizeof t) && !strncmp(t, "Legenda sincronizada \xc2\xb7 Boa \xc2\xb7 +2.", 30)); }
    assert(!strcmp(prov.estado(0, prov.u), "Sincronizada"));
    assert(prov.acoes(0, rot, 8, prov.u) == 1 && !strcmp(rot[0], "Desfazer"));
    prov.executar(0, 0, prov.u);
    for (int k = 0; k < 50; k++) passo(MKV, 0);                         // desfeita: nao reinicia sozinha
    v = legsync_visao(0); assert(v.fase == LEGSYNC_DESFEITA && legsync_offset_ms(0) == 0 && trocas == 0);

    // 12b. A escolhida (ruim) nao fecha: troca para outra do mesmo idioma que sincroniza.
    legsync_iniciar(MKV); trocas = 0; cand[0] = "ext_ruim2.srt"; cand[1] = "ext_mais2500.srt"; nCand = 2;
    legsync_primaria_externa("ext://0/ext_ruim.srt", "pt", "Ruim");
    v = esperarAuto(MKV, 2);
    assert(v.autoTrocou && !strcmp(v.autoNome, "Prov-ext_mais2500.srt") && abs(v.offsetAutoMs - 2500) <= 25);
    assert(trocas == 2 && strstr(prov.estado(0, prov.u), "Prov-ext_mais2500.srt"));

    // 12c. Nenhuma sincroniza: volta a escolha original, nada alterado, estado honesto.
    legsync_iniciar(MKV); trocas = 0; voltou = 0; cand[0] = "ext_ruim2.srt"; nCand = 1;
    legsync_primaria_externa("ext://0/ext_ruim.srt", "pt", "Ruim");
    v = esperarAuto(MKV, 3);
    assert(voltou == 1 && !v.autoTrocou && legsync_offset_ms(0) == 0);
    assert(!strcmp(prov.estado(0, prov.u), "N\xc3\xa3o deu para sincronizar"));
    assert(prov.acoes(0, rot, 8, prov.u) == 0);

    // 12d. Arquivo sem referencia (sem faixa de texto): nao ha como julgar, nao troca nada.
    legsync_iniciar(MKV3); trocas = 0; cand[0] = "ext_mais2500.srt"; nCand = 1;
    legsync_primaria_externa("ext://0/ext_ruim.srt", "pt", "Ruim");
    v = esperarAuto(MKV3, 3); assert(trocas == 0 && legsync_offset_ms(0) == 0);
    { char t[200]; assert(!legsync_pilula_final(&v, "Ruim", t, sizeof t) && strstr(t, "n\xc3\xa3o sincronizada")); }

    // 12e. Outro idioma nao entra: sem candidata do mesmo idioma, volta sozinha ao fim.
    legsync_iniciar(MKV); trocas = 0; cand[0] = "ext_mais2500.srt"; nCand = 1; candIdioma = "en";
    legsync_primaria_externa("ext://0/ext_ruim.srt", "pt", "Ruim");
    v = esperarAuto(MKV, 3); assert(trocas == 0); candIdioma = "pt";

    // 12f. REGRESSAO "fala que ta ok e ta fora de sincronia" (dono, 04/10).
    //      Traducao de verdade: outro corte das falas, bordas com folga de
    //      quadro, +2,0 s do video. O plano automatico RODA o AutoSync contra a
    //      embutida (sem a pessoa operar nada), a engine recusa (fail-closed: as
    //      bordas nao casam todas), e entao: nada muda no que o renderer desenha
    //      (so o manual) e a pilula diz "nao sincronizada" com o aviso, nunca
    //      "Legenda aplicada" com o check como antes.
    { int p0; char t[200];
      pthread_mutex_lock(&LM); p0 = pedidosLeitor; pthread_mutex_unlock(&LM);
      legsync_iniciar(MKV); trocas = 0; nCand = 0;
      legsync_primaria_externa("ext://0/ext_traduzida_mais2000.srt", "pt", "OpenSubtitles");
      v = esperarAuto(MKV, 3);
      pthread_mutex_lock(&LM); assert(pedidosLeitor > p0); pthread_mutex_unlock(&LM);   // leu a referencia
      assert(v.fase == LEGSYNC_RECUSADA && v.motivo == LEGSYNC_M_CONFIANCA);          // a engine julgou e recusou
      assert(legsync_offset_ms(0) == 0 && legsync_offset_ms(300) == 300);              // renderer: so o manual
      assert(!legsync_pilula_final(&v, "OpenSubtitles", t, sizeof t));
      assert(!strcmp(t, "Legenda aplicada \xc2\xb7 OpenSubtitles \xc2\xb7 n\xc3\xa3o sincronizada"));
      // No instante t do video a externa desenhada e a que o arquivo traz em t:
      // nenhum deslocamento escondido.
      { long n; char *b = arquivo("ext_traduzida_mais2000.srt", &n);
        LegendaDocumentoInfo i = { .flags = LEGENDA_DOC_COMPLETO };
        LegendaDocumento *ext = legenda_documento_bytes(b, n, &i); int vistos = 0;
        free(b);
        for (double x = 30; x < 590; x += 0.41) {
          LegendaCue a, e;
          int na = legenda_cues(x, legsync_offset_ms(0), &a, 1), ne = legenda_documento_cues(ext, x, 0, &e, 1);
          assert(na == ne); if (na) { assert(!strcmp(a.texto, e.texto)); vistos++; }
        }
        assert(vistos > 50); legenda_documento_liberar(ext); }
      // Recusa com a mesma faixa do arquivo: a pilula final nao vira "sincronizada"
      // com o tempo nem depois de mais passos.
      for (int k = 0; k < 50; k++) passo(MKV, 0);
      v = legsync_visao(0); assert(!legsync_pilula_final(&v, "OpenSubtitles", t, sizeof t)); }

    // 12g. REGRESSAO "em todos os filmes ela fala que ta ok e ta fora de
    //      sincronia" (dono, 04/10). Um longa de 2 h (1332 falas) com a
    //      externa +2,5 s e a embutida no MKV. A referencia custa UM Range por
    //      fala (cada fala mora num Cluster) e a TV le no maximo LS_RITMO = 8
    //      Ranges/s: o relogio do player aqui anda 125 ms por Range, o melhor
    //      caso da TV (sem latencia de rede). Antes, o teto de 45 s do plano
    //      vencia aos ~360 Ranges, cancelava a leitura, deixava a legenda +2,5 s
    //      e a pilula dizia "Legenda aplicada" com o check. Agora o teto so
    //      vence sem PROGRESSO: a leitura termina, a engine aceita, o renderer
    //      desenha a fala certa e a pilula diz "sincronizada".
    { const char *FILME = "https://cdn.example/d/filme.mkv";
      int p0, ranges = 0; unsigned t0; char t[200];
      atrasoLeitorUs = 1500;
      legsync_iniciar(FILME); trocas = 0; nCand = 0;
      pthread_mutex_lock(&LM); p0 = pedidosLeitor; pthread_mutex_unlock(&LM);
      legsync_primaria_externa("ext://0/filme_ext_mais2500.srt", "pt", "OpenSubtitles");
      t0 = agora;
      for (int k = 0; k < 400000; k++) {
        unsigned alvo;
        pthread_mutex_lock(&LM); ranges = pedidosLeitor - p0; pthread_mutex_unlock(&LM);
        alvo = t0 + (unsigned)ranges * 125u + (unsigned)k / 8u;   // 1/LS_RITMO s por Range
        agora = alvo > agora ? alvo : agora + 1;
        legsync_passo(FILME, 30.0, 60.0, 0, agora);
        v = legsync_visao(0);
        if (v.autoFase != 1) break;
        usleep(200);
      }
      atrasoLeitorUs = 0;
      printf("[12g] filme: %d Ranges, relogio do player %u s, autoFase %d, fase %d, offset em vigor %d ms\n",
             ranges, (agora - t0) / 1000u, v.autoFase, v.fase, legsync_offset_ms(0));
      assert(ranges > 1200 && agora - t0 > 3u * LS_AUTO_TETO_TESTE);  // ~um Range por fala: > 2 min a 8/s
      assert(v.autoFase == 2 && v.fase == LEGSYNC_ACEITA && abs(v.offsetAutoMs - 2500) <= 25);
      assert(legsync_offset_ms(0) == v.offsetAutoMs);
      conferirFilme(legsync_offset_ms(0));                      // a fala certa no instante certo
      assert(legsync_pilula_final(&v, "OpenSubtitles", t, sizeof t) && strstr(t, "sincronizada \xc2\xb7 OpenSubtitles \xc2\xb7 +2."));
      legsync_encerrar(); }
    casos++; }
  legsync_teste_auto(0); legsync_definir_trocador(NULL);

  // 10. Fim de sessao e corridas de teardown: leitura presa, download pendente,
  //     analise em curso, 40 sessoes seguidas; depois destruir com tudo no ar.
  for (int k = 0; k < 40; k++) {
    legsync_iniciar(k & 1 ? MKV : MKV2);
    legsync_primaria_externa(k % 3 ? "ext://0/ext_mais2500.srt" : "ext://20/ext_menos1200.srt", "pt", "P");
    for (int j = 0; j < 30 && legsync_visao(0).fase != LEGSYNC_PRONTA; j++) { passo(k & 1 ? MKV : MKV2, 0); usleep(1000); }
    legsync_acao(k & 2 ? LEGSYNC_ACAO_COMPLETA : LEGSYNC_ACAO_RAPIDA);
    passo(k & 1 ? MKV : MKV2, k % 5 == 0);
    if (k % 4 == 0) legsync_encerrar();
  }
  travar = 1;
  legsync_iniciar(MKV);
  legsync_primaria_externa("ext://0/ext_mais2500.srt", "pt", "P");
  esperarFase(MKV, LEGSYNC_PRONTA);
  legsync_acao(LEGSYNC_ACAO_RAPIDA); usleep(20000);
  legsync_primaria_externa("ext://200/ext_menos1200.srt", "en", "P");   // download no ar
  legsync_destruir();
  travar = 0;
  assert(legsync_offset_ms(77) == 77 && legsync_visao(0).fase == LEGSYNC_INDISPONIVEL);
  usleep(400000);    // o download pendente termina depois do destruir: sem efeito
  casos++;

  // 11. Plataforma sem pedido cancelavel: indisponivel, sem fingir.
  legsync_teste_leitor(NULL, NULL); capacidades = 0;
  legsync_iniciar(MKV);
  legsync_primaria_externa("ext://0/ext_mais2500.srt", "pt", "P");
  for (int j = 0; j < 200 && legsync_visao(0).motivo != LEGSYNC_M_PLATAFORMA; j++) usleep(1000);
  v = legsync_visao(0);
  assert(v.fase == LEGSYNC_INDISPONIVEL && v.motivo == LEGSYNC_M_PLATAFORMA && !legsync_acao(LEGSYNC_ACAO_RAPIDA));
  legsync_destruir(); casos++;
  pilula_sequencia(3, 1, 0); pilula_sequencia(3, 0, 0); pilula_sequencia(1, 0, 1); casos += 3;
  printf("legsync: %d casos ok\n", casos);
  return 0;
}
