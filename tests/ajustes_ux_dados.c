/* Busca e comparacao locais: nao inicia UI, rede ou sincronizacao. */
#include "../src/ajustes.c"
#include <assert.h>
#include <limits.h>
#include <unistd.h>

static AjusteBuscaResultado resultados[AJ_N];

static int indiceResultado(int op, int n) {
  int i;
  for (i = 0; i < n; i++) if (resultados[i].op == op) return i;
  return -1;
}

/* #221 sobreviveu a reorganizacao da tela: default, escopo local, prazo
 * efetivo, entrada unica em Reproducao e descoberta pela busca. */
static void prazoDosAddonsIntegrado(void) {
  int prazoAntes = valor[AJ_FONTE_PRAZO], manualAntes = valor[AJ_FONTE_MANUAL];
  int idiomaAntes = valor[AJ_IDIOMA], vezes = 0, n, indice;
  const char *categoria = "", *grupo = "";
  char texto[80];
  assert(valor[AJ_FONTE_PRAZO] == 1 && valorPadrao[AJ_FONTE_PRAZO] == 1);
  assert(ajustes_fonte_prazo_ms() == 5000);
  assert(!dePerfil(AJ_FONTE_PRAZO));
  assert(!strcmp(uxEscopo(AJ_FONTE_PRAZO), "Só nesta TV"));
  assert(uxTemPadrao(AJ_FONTE_PRAZO) && !uxDiferente(AJ_FONTE_PRAZO));
  assert(OPCOES[AJ_FONTE_PRAZO].n == 4);
  assert(!strcmp(OPCOES[AJ_FONTE_PRAZO].valores[1], "5 s"));
  assert(familiaPreviaOpcao(AJ_FONTE_PRAZO) == AJPV_REPRO);
  for (int i = 0; i < AJ_N_TELA; i++) {
    if (TELA[i].tipo == IT_SEC) { categoria = TELA[i].titulo; grupo = ""; }
    if (TELA[i].tipo == IT_ROT) grupo = TELA[i].titulo;
    if (TELA[i].tipo == IT_OPC && TELA[i].op == AJ_FONTE_PRAZO) {
      vezes++;
      assert(!strcmp(categoria, "Reprodução"));
      assert(!strcmp(grupo, "Escolha da fonte"));
    }
  }
  assert(vezes == 1);
  valor[AJ_IDIOMA] = IDIOMA_PT + 1;
  valor[AJ_FONTE_MANUAL] = 1; // escolha automatica habilita o prazo
  n = ajustes_buscar("Espera pelos add-ons", resultados, AJ_N);
  indice = indiceResultado(AJ_FONTE_PRAZO, n);
  assert(indice >= 0 && !resultados[indice].bloqueado);
  assert(strstr(resultados[indice].caminho, "Reprodução"));
  assert(!strcmp(resultados[indice].valor, "5 s"));
  vezes = 0;
  for (int i = 0; i < n; i++) if (resultados[i].op == AJ_FONTE_PRAZO) vezes++;
  assert(vezes == 1);
  static const int esperado[] = {3000, 5000, 8000, 0};
  for (int i = 0; i < 4; i++) {
    valor[AJ_FONTE_PRAZO] = i;
    assert(ajustes_fonte_prazo_ms() == esperado[i]);
    uxValorTexto(AJ_FONTE_PRAZO, i, texto, sizeof texto);
    assert(!strcmp(texto, i18n(V_FONTE_PRAZO[i])));
  }
  valor[AJ_FONTE_PRAZO] = prazoAntes;
  valor[AJ_FONTE_MANUAL] = manualAntes;
  valor[AJ_IDIOMA] = idiomaAntes;
  assert(ajustes_fonte_prazo_ms() == 5000);
}

static void escoposPorValor(void) {
  int copia[AJ_N], v, temaSalvo = valor[AJ_TEMA];
  int seguroAntes = SEGURO, corAntes = ajustes_cor_viva();
  float legendaAntes = legendaEspera;
  char audioAntes[32], legAntes[32];
  snprintf(audioAntes, sizeof audioAntes, "%s", ling_audio());
  snprintf(legAntes, sizeof legAntes, "%s", ling_legenda());
  memcpy(copia, valor, sizeof copia);
  assert(dePerfil(AJ_TEMA));
  for (v = 0; v < AJ_N_TEMAS_OPC; v++)
    assert(!strcmp(uxEscopoValor(AJ_TEMA, v),
      v >= AJ_TEMA_DINAMICA ? "Só nesta TV" : "Conta/perfil"));
  assert(!strcmp(uxEscopoValor(AJ_TMDB_IDIOMA, 0), "Só nesta TV"));
  assert(!strcmp(uxEscopoValor(AJ_TMDB_IDIOMA, 1), "Conta/perfil"));
  /* "Da conta" muda a origem efetiva, nao retira a escolha do perfil. */
  assert(dePerfil(AJ_LEG_LINGUA) && dePerfil(AJ_AUD_LINGUA));
  assert(!strcmp(uxEscopoValor(AJ_LEG_LINGUA, 0), "Conta/perfil"));
  assert(!strcmp(uxEscopoValor(AJ_AUD_LINGUA, 0), "Conta/perfil"));
  assert(!strcmp(uxEscopoValor(AJ_LEG_LINGUA, LING_OPC_ORIGINAL), "Conta/perfil"));
  assert(!strcmp(uxEscopoValor(AJ_AUD_LINGUA, LING_OPC_ORIGINAL), "Conta/perfil"));
  assert(!strcmp(uxEscopo(-1), "Só nesta TV"));
  assert(!strcmp(uxEscopo(AJ_N), "Só nesta TV"));
  /* Consultar candidatos/padrao nao restaura, aplica ou altera preferencias. */
  assert(!memcmp(copia, valor, sizeof copia));
  assert(SEGURO == seguroAntes && ajustes_cor_viva() == corAntes);
  assert(legendaEspera == legendaAntes);
  assert(!strcmp(audioAntes, ling_audio()) && !strcmp(legAntes, ling_legenda()));
  valor[AJ_TEMA] = AJ_TEMA_DINAMICA;
  assert(!strcmp(uxEscopo(AJ_TEMA), "Só nesta TV"));
  // O padrao da 2.0 e a Imersiva, que e desta TV (a conta nao a conhece).
  assert(!strcmp(uxEscopoValor(AJ_TEMA, uxValorPadrao(AJ_TEMA)), "Só nesta TV"));
  assert(!strcmp(uxEscopoValor(AJ_TEMA, 0), "Conta/perfil"));
  assert(valor[AJ_TEMA] == AJ_TEMA_DINAMICA);
  valor[AJ_TEMA] = AJ_TEMA_DINAMICA - 1;
  assert(!strcmp(uxEscopo(AJ_TEMA), "Conta/perfil"));
  valor[AJ_TEMA] = temaSalvo;
  assert(!memcmp(copia, valor, sizeof copia));
}

static void padroesEValores(void) {
  int op, copia[AJ_N];
  char texto[160], pequeno[4], normal[80];
  assert(sizeof valor == sizeof valorPadrao);
  assert(!memcmp(valor, valorPadrao, sizeof valor));
  for (op = 0; op < AJ_N; op++) {
    assert(!uxDiferente(op));
    if (OPCOES[op].tipo == OP_ACAO || OPCOES[op].tipo == OP_LEITURA ||
        CHAVE[op][0] == '-' || op == AJ_PERFIL_PESQ)
      assert(!uxTemPadrao(op));
    else {
      assert(uxTemPadrao(op));
      assert(uxValorPadrao(op) == valorPadrao[op]);
    }
  }
  assert(!uxTemPadrao(-1) && !uxTemPadrao(AJ_N));
  assert(!uxDiferente(-1) && !uxDiferente(AJ_N));
  valor[AJ_FOCO_TRAILER] = 1 - valorPadrao[AJ_FOCO_TRAILER];
  assert(uxDiferente(AJ_FOCO_TRAILER));
  assert(uxValorPadrao(AJ_FOCO_TRAILER) == 1);
  valor[AJ_FOCO_TRAILER] = valorPadrao[AJ_FOCO_TRAILER];
  /* Um valor que o modo seguro suspende continua sendo a preferencia salva. */
  valor[AJ_RESOLUCAO] = 1;
  SEGURO = 1;
  assert(uxDiferente(AJ_RESOLUCAO));
  assert(!ajustes_4k());
  uxValorTexto(AJ_RESOLUCAO, valor[AJ_RESOLUCAO], texto, sizeof texto);
  assert(strstr(texto, "4K"));
  SEGURO = 0;
  valor[AJ_RESOLUCAO] = valorPadrao[AJ_RESOLUCAO];
  assert(!strcmp(uxEscopo(AJ_IDIOMA), "Só nesta TV"));
  assert(!strcmp(uxEscopo(AJ_RESOLUCAO), "Só nesta TV"));
  assert(!strcmp(uxEscopo(AJ_VIDRO), "Só nesta TV"));
  assert(!strcmp(uxEscopo(AJ_AUD_LINGUA), "Conta/perfil"));
  assert(!strcmp(uxEscopo(AJ_FOCO_TRAILER), "Conta/perfil"));
  assert(!strstr(uxEscopo(AJ_FOCO_TRAILER), "sincronizado"));

  memcpy(copia, valor, sizeof copia);
  uxValorTexto(AJ_FOCO_TRAILER, 0, texto, sizeof texto);
  assert(!strcmp(texto, i18n("Ligado")));
  uxValorTexto(AJ_AUD_LINGUA, LING_OPC_ORIGINAL, texto, sizeof texto);
  assert(!strcmp(texto, i18n("Original do título")));
  uxValorTexto(AJ_HERO_TRAILER_ESPERA, 22, texto, sizeof texto);
  assert(strstr(texto, "2,2") || strstr(texto, "2.2"));
  uxValorTexto(AJ_SEEKR_AJUSTE, -5, texto, sizeof texto);
  assert(!strcmp(texto, "-5 s"));
  uxValorTexto(AJ_FIL_LIMITE, 12, texto, sizeof texto);
  assert(!strcmp(texto, "12"));
  uxValorTexto(AJ_FOCO_TRAILER, INT_MAX, texto, sizeof texto);
  assert(!strcmp(texto, i18n("Ligado")));
  uxValorTexto(AJ_N, 0, texto, sizeof texto);
  assert(!texto[0]);
  uxValorTexto(AJ_IDIOMA, 0, NULL, 0);
  assert(!memcmp(copia, valor, sizeof copia));
  uxTextoCopiar(pequeno, sizeof pequeno, "áéí");
  assert(!strcmp(pequeno, "á"));
  uxNormalizar("  MEMO\xCC\x81RIA / ÁUDIO · AÇÃO  ", normal, sizeof normal);
  assert(!strcmp(normal, "memoria audio acao"));
  uxNormalizar("\xF0\x9F", normal, sizeof normal);
  assert(!normal[0]);
}

static void buscaECaminhos(void) {
  int n, i, copia[AJ_N];
  AjusteBuscaResultado primeiro, limitado[2];
  char longa[2048];
  valor[AJ_IDIOMA] = IDIOMA_PT + 1;
  memcpy(copia, valor, sizeof copia);
  n = ajustes_buscar("MEMÓRIA PARA IMAGENS", resultados, AJ_N);
  assert(n > 0 && resultados[0].op == AJ_TEX_MB);
  assert(resultados[0].avancado);
  assert(strstr(resultados[0].caminho, "Desempenho desta TV"));
  primeiro = resultados[0];
  n = ajustes_buscar("memo\xCC\x81ria para imagens", resultados, AJ_N);
  assert(n > 0 && !memcmp(&primeiro, &resultados[0], sizeof primeiro));
  n = ajustes_buscar("DUBLADO", resultados, AJ_N);
  assert(n > 0 && resultados[0].op == AJ_AUD_LINGUA);
  assert(strstr(resultados[0].caminho, "Idiomas e legendas"));
  n = ajustes_buscar("CC", resultados, AJ_N);
  assert(n > 0 && resultados[0].op == AJ_LEG_LINGUA);
  n = ajustes_buscar("subtitle", resultados, AJ_N);
  assert(n > 0 && resultados[0].op == AJ_LEG_LINGUA);
  n = ajustes_buscar("travando", resultados, AJ_N);
  assert(indiceResultado(AJ_VIDRO, n) >= 0);
  assert(indiceResultado(AJ_TEX_MB, n) >= 0);
  n = ajustes_buscar("trailer", resultados, AJ_N);
  assert(indiceResultado(AJ_HERO_TRAILER, n) >= 0);
  assert(indiceResultado(AJ_FOCO_TRAILER, n) >= 0);
  assert(indiceResultado(AJ_TRAILER_QUAL, n) >= 0);
  for (i = 0; i < n; i++) {
    int j;
    assert(resultados[i].titulo[0] && resultados[i].caminho[0]);
    for (j = 0; j < i; j++) assert(resultados[i].op != resultados[j].op);
  }
  memset(limitado, 0xa5, sizeof limitado);
  primeiro = limitado[1];
  assert(ajustes_buscar("trailer", limitado, 1) == 1);
  assert(!memcmp(&primeiro, &limitado[1], sizeof primeiro));
  assert(ajustes_buscar("trailer", NULL, AJ_N) == 0);
  assert(ajustes_buscar("trailer", resultados, 0) == 0);
  assert(ajustes_buscar("trailer", resultados, -5) == 0);
  assert(ajustes_buscar("", resultados, AJ_N) == 0);
  assert(ajustes_buscar("  \t  ", resultados, AJ_N) == 0);
  assert(ajustes_buscar(NULL, resultados, AJ_N) == 0);
  assert(ajustes_buscar("\xF0\x9F", resultados, AJ_N) == 0);
  memset(longa, 'z', sizeof longa - 1); longa[sizeof longa - 1] = 0;
  assert(ajustes_buscar(longa, resultados, AJ_N) == 0);
  assert(!memcmp(copia, valor, sizeof copia));
}

static void bloqueadosESegredos(void) {
  int n, i, copia[AJ_N];
  const char *marcador = "sentinelaqzprivadaux2026";
  char texto[160];
  valor[AJ_HERO_TRAILER] = 1;
  valor[AJ_TMDB_LIGADO] = 1;
  memcpy(copia, valor, sizeof copia);
  n = ajustes_buscar("som do trailer no destaque", resultados, AJ_N);
  i = indiceResultado(AJ_HERO_TRAILER_SOM, n);
  assert(i >= 0 && resultados[i].bloqueado);
  assert(strstr(resultados[i].caminho, "Trailers"));
  n = ajustes_buscar("idioma dos metadados", resultados, AJ_N);
  i = indiceResultado(AJ_TMDB_IDIOMA, n);
  assert(i >= 0 && resultados[i].bloqueado);
  /* A build de teste nao configura o servico social. */
  assert(!recomenda_ativo());
  n = ajustes_buscar("perfil", resultados, AJ_N);
  assert(indiceResultado(AJ_PERFIL_PESQ, n) < 0);
  assert(indiceResultado(AJ_PERFIL_EDITAR, n) < 0);

  snprintf(fanartChave, sizeof fanartChave, "%s", marcador);
  snprintf(seekrChave, sizeof seekrChave, "%s", marcador);
  snprintf(pstToken, sizeof pstToken, "%s", marcador);
  snprintf(pstInst, sizeof pstInst, "https://%s.example.invalid", marcador);
  snprintf(pstModelo, sizeof pstModelo, "https://%s.example.invalid/{imdb}", marcador);
  snprintf(p2pEndereco, sizeof p2pEndereco, "http://%s.example.invalid", marcador);
  xtream_definir_usuario(marcador);
  /* Prova que a ajuda legada realmente contem um valor privado nesta fixture. */
  assert(strstr(ajudaOpcao(AJ_XTREAM_USUARIO), marcador));
  assert(ajustes_buscar(marcador, resultados, AJ_N) == 0);
  n = ajustes_buscar("usuário Xtream", resultados, AJ_N);
  i = indiceResultado(AJ_XTREAM_USUARIO, n);
  assert(i >= 0 && !strcmp(resultados[i].valor, "Abrir"));
  n = ajustes_buscar("chave", resultados, AJ_N);
  assert(indiceResultado(AJ_FANART_CHAVE, n) >= 0);
  assert(indiceResultado(AJ_SEEKR_CHAVE, n) >= 0);
  for (i = 0; i < n; i++) {
    assert(!strstr(resultados[i].valor, marcador));
    assert(!strstr(resultados[i].titulo, marcador));
    assert(!strstr(resultados[i].caminho, marcador));
  }
  uxValorTexto(AJ_XTREAM_USUARIO, 0, texto, sizeof texto);
  assert(!strcmp(texto, "Abrir"));
  uxValorTexto(AJ_PERFIL_ATIVO, 0, texto, sizeof texto);
  assert(!strcmp(texto, "Ver detalhes"));
  assert(!memcmp(copia, valor, sizeof copia));
  xtream_esquecer();
}

/* F07: seek cache option. Appended, local, default off; on a TV without an
 * app-controlled disk cache (this host build, LG, Samsung) the row is
 * inactive, says so and the accessor never asks the backend for a cache. */
static void zoomTpk(void) {
  int i, vezes = 0;
  { int n = 0;   // Silero: visibility with explicit runtime/model capability gating
    for (i = 0; i < AJ_N_TELA; i++) if (TELA[i].tipo == IT_OPC && TELA[i].op == AJ_LEG_SYNC_AUDIO) n++;
#if defined(NV_ANDROID) || defined(NV_WEBOS) || defined(NV_LINUX_DESKTOP)
    assert(n == 1);
#else
    assert(n == 0);
#endif
  }
  assert(!strcmp(CHAVE[AJ_TRAILER_ZOOM_TPK], "trailerZoomTpkLocal"));
  assert(valorPadrao[AJ_TRAILER_ZOOM_TPK] == 1);   // Desligado
  assert(!strcmp(OPCOES[AJ_TRAILER_ZOOM_TPK].valores[1], "Desligado"));
  assert(somenteDesteAparelho(AJ_TRAILER_ZOOM_TPK) && !dePerfil(AJ_TRAILER_ZOOM_TPK));
  for (i = 0; i < AJ_N_TELA; i++) if (TELA[i].tipo == IT_OPC && TELA[i].op == AJ_TRAILER_ZOOM_TPK) vezes++;
#ifdef NV_TPK
  assert(vezes == 1);
#else
  assert(vezes == 0);   // hidden outside the native .tpk
#endif
}
static void cacheSeek(void) {
  int antes = valor[AJ_CACHE_SEEK], vezes = 0, n, i;
  const char *categoria = "";
  assert(!strcmp(CHAVE[AJ_CACHE_SEEK], "cacheSeekLocal"));
  assert(valorPadrao[AJ_CACHE_SEEK] == 0 && OPCOES[AJ_CACHE_SEEK].n == 4);
  assert(!strcmp(OPCOES[AJ_CACHE_SEEK].valores[0], "Desligado"));
  assert(!strcmp(OPCOES[AJ_CACHE_SEEK].valores[3], "1 GB"));
  assert(!dePerfil(AJ_CACHE_SEEK) && somenteDesteAparelho(AJ_CACHE_SEEK));
  assert(!strcmp(uxEscopo(AJ_CACHE_SEEK), "Só nesta TV"));
  assert(familiaPreviaOpcao(AJ_CACHE_SEEK) == AJPV_REPRO);
  for (i = 0; i < AJ_N_TELA; i++) {
    if (TELA[i].tipo == IT_SEC) categoria = TELA[i].titulo;
    if (TELA[i].tipo == IT_OPC && TELA[i].op == AJ_CACHE_SEEK) { vezes++; assert(!strcmp(categoria, "Reprodução")); }
  }
  assert(vezes == 1);
  assert(inativa(AJ_CACHE_SEEK));
  assert(!strcmp(textoValor(AJ_CACHE_SEEK), "Não disponível nesta TV"));
  assert(strstr(ajudaOpcao(AJ_CACHE_SEEK), "Não disponível nesta TV"));
  for (i = 0; i < 4; i++) { valor[AJ_CACHE_SEEK] = i; assert(ajustes_cache_seek_mb() == 0); }
  valor[AJ_CACHE_SEEK] = antes;
  n = ajustes_buscar("cache de seek", resultados, AJ_N);
  i = indiceResultado(AJ_CACHE_SEEK, n);
  assert(i >= 0 && resultados[i].bloqueado);
}

/* AutoSync is appended after the existing local setting. Its intent defaults
 * off and stays on this TV; unsupported builds keep the row visible only on
 * platforms where the setting is exposed, with actions inert until supported. */
static void audioSyncSettings(void) {
  int n = 0, rows = 0, retry = 0, remove = 0, saved = valor[AJ_LEG_SYNC_AUDIO];
  assert(AJ_LEG_SYNC_AUDIO == AJ_LEG_LINGUA2 + 1);
  assert(AJ_AUDMODEL_RETRY == AJ_RELOGIO_12H + 1);
  assert(AJ_AUDMODEL_REMOVE == AJ_AUDMODEL_RETRY + 1);
  assert(AJ_AUDMODEL_REMOVE == AJ_N - 1);
  assert(!strcmp(CHAVE[AJ_LEG_SYNC_AUDIO], "legendaSyncAudioLocal"));
  assert(valorPadrao[AJ_LEG_SYNC_AUDIO] == 1 && !ajustes_legenda_sync_audio());
  assert(somenteDesteAparelho(AJ_LEG_SYNC_AUDIO) && !dePerfil(AJ_LEG_SYNC_AUDIO));
  assert(!strcmp(CHAVE[AJ_AUDMODEL_RETRY], "-audmodelRetry"));
  assert(!strcmp(CHAVE[AJ_AUDMODEL_REMOVE], "-audmodelRemove"));
  assert(OPCOES[AJ_AUDMODEL_RETRY].tipo == OP_ACAO && OPCOES[AJ_AUDMODEL_REMOVE].tipo == OP_ACAO);
  assert(!uxTemPadrao(AJ_AUDMODEL_RETRY) && !uxTemPadrao(AJ_AUDMODEL_REMOVE));
  for (int i = 0; i < AJ_N_TELA; i++) if (TELA[i].tipo == IT_OPC) {
    rows += TELA[i].op == AJ_LEG_SYNC_AUDIO;
    retry += TELA[i].op == AJ_AUDMODEL_RETRY;
    remove += TELA[i].op == AJ_AUDMODEL_REMOVE;
  }
#if defined(NV_ANDROID) || defined(NV_WEBOS) || defined(NV_LINUX_DESKTOP)
  assert(rows == 1 && retry == 1 && remove == 1);
#else
  assert(rows == 0 && retry == 0 && remove == 0);
#endif
  if (!audmodel_supported()) {
    assert(inativa(AJ_LEG_SYNC_AUDIO));
    assert(inativa(AJ_AUDMODEL_RETRY) && inativa(AJ_AUDMODEL_REMOVE));
    assert(strstr(ajudaOpcao(AJ_LEG_SYNC_AUDIO), "não está disponível"));
    /* A user who has an old saved-on preference must still be able to turn it
     * off after installing a build without the runtime. */
    valor[AJ_LEG_SYNC_AUDIO] = 0;
    assert(ajustes_legenda_sync_audio() && !inativa(AJ_LEG_SYNC_AUDIO));
  }
  valor[AJ_LEG_SYNC_AUDIO] = saved;
  assert(!ajustes_legenda_sync_audio());
}

// Cada categoria e cada submenu (ROT) tem a SUA arte: nenhum indice de
// AJ_ARTE_SEC se repete, todo bloco de ajustes_ux_tela.inc tem coluna, e a
// opcao devolve a arte do bloco dela (o primeiro bloco fica com a da categoria).
static void artePorSubmenu(void) {
  int visto[40] = { 0 }, s, g, i;
  montarTela();
  assert(nSecoes == 11);
  for (s = 0; s < 11; s++) {
    int blocos = 0, ultimo = -1;
    assert(AJ_ARTE_SEC[s][0] >= 0);
    for (g = 0; g < AJ_ARTE_BLOCOS; g++) {
      int n = AJ_ARTE_SEC[s][g];
      if (n < 0) continue;
      assert(n < 40 && !visto[n]);
      visto[n] = 1;
    }
    for (i = secIni[s] + 1; i < secFim(s); i++) {
      if (TELA[i].tipo == IT_ROT) { blocos++; continue; }
      if (TELA[i].tipo != IT_OPC) continue;
      g = blocos > 0 ? blocos - 1 : 0;
      assert(g < AJ_ARTE_BLOCOS && AJ_ARTE_SEC[s][g] >= 0);
      assert(ajCenaArteOp(TELA[i].op) == AJ_ARTE_SEC[s][g]);
      if (g != ultimo) { assert(g == ultimo + 1); ultimo = g; }
    }
  }
  // A mesma imagem nao pede troca; submenu ou categoria diferente pede.
  assert(ajCenaChave(AJS_REPRODUCAO, AJ_DV) == ajCenaChave(AJS_REPRODUCAO, AJ_ATMOS));
  assert(ajCenaChave(AJS_REPRODUCAO, AJ_DV) != ajCenaChave(AJS_REPRODUCAO, AJ_PAUSA_OVERLAY));
  // O primeiro bloco tem cena propria (o que ele controla), diferente da
  // visao geral da categoria; cada bloco com cena tem a sua.
  assert(ajCenaChave(AJS_REPRODUCAO, AJ_FONTE_MANUAL) != ajCenaChave(AJS_REPRODUCAO, -1));
  assert(ajCenaChave(AJS_REPRODUCAO, AJ_FONTE_MANUAL) == ajCenaChave(AJS_REPRODUCAO, AJ_FONTE_PRAZO));
  { int vistoC[AJC_N] = { 0 }, ultimaC = -1, k2;
    for (k2 = 0; k2 < AJ_N_TELA; k2++) {
      int c;
      if (TELA[k2].tipo != IT_OPC) continue;
      c = ajCenaSecao(TELA[k2].op);
      if (c < 0 || c == ultimaC) continue;
      assert(c < AJC_N && (!vistoC[c] || TELA[k2].op == AJ_TMDB_IDIOMA));   // um bloco, uma cena: nada repetido em outro lugar
      vistoC[c] = 1; ultimaC = c;
    } }
  assert(ajCenaChave(AJS_CONTAS, -1) != ajCenaChave(AJS_CARTAZES, -1));
  assert(ajCenaChave(AJS_HOME, AJ_CW_LIGADO) == ajCenaChave(AJS_HOME, AJ_CW_ORDEM));
}
int main(void) {
  char dir[] = "/tmp/nuvio-aj-ux-dados-XXXXXX";
  assert(AJ_DISCORD == AJ_ICONE_APP + 1 && AJ_TAMANHO_AJUSTES == AJ_DISCORD + 1 && AJ_LOGO_TRAILER == AJ_TAMANHO_AJUSTES + 1 && AJ_LEG_LINGUA2 == AJ_LOGO_TRAILER + 1 && AJ_LEG_SYNC_AUDIO == AJ_LEG_LINGUA2 + 1 && AJ_CACHE_SEEK == AJ_LEG_SYNC_AUDIO + 1 && AJ_TRAILER_ZOOM_TPK == AJ_CACHE_SEEK + 1 && AJ_PLUGINS == AJ_TRAILER_ZOOM_TPK + 1 && AJ_JF_LIGADO == AJ_PLUGINS + 1 && AJ_JF_SAIR == AJ_PLUGINS + 4 && AJ_AVANCADAS == AJ_JF_SAIR + 1 && AJ_LEG2_POS == AJ_AVANCADAS + 1 && AJ_EM_SERVIDOR == AJ_LEG2_BORDA + 1 && AJ_PX_SAIR == AJ_EM_SERVIDOR + 5 && AJ_FONTE_PRIORIDADE == AJ_PX_SAIR + 1 && AJ_FONTE_HDR == AJ_ABERTURA - 2 && AJ_LOGO_APP == AJ_FONTE_HDR + 1 && AJ_ENQUETES == AJ_ABERTURA + 1 && AJ_ENQUETES == AJ_N - 11);
  assert(!strcmp(CHAVE[AJ_LOGO_APP], "logoAppLocal") && !strcmp(CHAVE[AJ_ABERTURA], "aberturaAppLocal") && OPCOES[AJ_LOGO_APP].n == 2 && OPCOES[AJ_ABERTURA].n == 3 && valorPadrao[AJ_LOGO_APP] == 0 && valorPadrao[AJ_ABERTURA] == 0 && somenteDesteAparelho(AJ_LOGO_APP) && somenteDesteAparelho(AJ_ABERTURA));
  { int u; for (u = AJ_LOGO_APP; u <= AJ_ABERTURA; u++) { int vz = 0, k; for (k = 0; k < AJ_N_TELA; k++) if (TELA[k].tipo == IT_OPC && TELA[k].op == u) vz++; assert(vz == 1); } }
  assert(!strcmp(CHAVE[AJ_FONTE_PRIORIDADE], "fontePrioridadeLocal") && !strcmp(CHAVE[AJ_FONTE_HDR], "fonteHdrLocal") && OPCOES[AJ_FONTE_PRIORIDADE].n == 3 && OPCOES[AJ_FONTE_HDR].n == 3 && valorPadrao[AJ_FONTE_PRIORIDADE] == 0 && valorPadrao[AJ_FONTE_HDR] == 0 && somenteDesteAparelho(AJ_FONTE_HDR));
  // N3: "Receber enquetes" is the LAST option: local, On by default, in "Notificações".
  assert(AJ_ENQUETES == AJ_ABERTURA + 1 && AJ_ENQUETES == AJ_N - 11);
  assert(!strcmp(CHAVE[AJ_ENQUETES], "enquetesLocal") && OPCOES[AJ_ENQUETES].n == 2 && valorPadrao[AJ_ENQUETES] == 0);
  assert(somenteDesteAparelho(AJ_ENQUETES) && !dePerfil(AJ_ENQUETES));
  { int vezesE = 0, k; for (k = 0; k < AJ_N_TELA; k++) if (TELA[k].tipo == IT_OPC && TELA[k].op == AJ_ENQUETES) vezesE++; assert(vezesE == 1); }
  assert(indiceResultado(AJ_ENQUETES, ajustes_buscar("enquete", resultados, AJ_N)) >= 0);
  /* #231: Cinemeta fora da busca. Ultima opcao, local, Ligado de fabrica (comportamento de antes). */
  assert(AJ_BUSCA_CINEMETA == AJ_ENQUETES + 1 && AJ_BUSCA_CINEMETA == AJ_N - 10);
  assert(!strcmp(CHAVE[AJ_BUSCA_CINEMETA], "buscaCinemetaLocal") && OPCOES[AJ_BUSCA_CINEMETA].n == 2);
  assert(valorPadrao[AJ_BUSCA_CINEMETA] == 0 && ajustes_busca_cinemeta());
  assert(somenteDesteAparelho(AJ_BUSCA_CINEMETA) && !dePerfil(AJ_BUSCA_CINEMETA));
  { int vz = 0, k; for (k = 0; k < AJ_N_TELA; k++) if (TELA[k].tipo == IT_OPC && TELA[k].op == AJ_BUSCA_CINEMETA) vz++; assert(vz == 1); }
  assert(indiceResultado(AJ_BUSCA_CINEMETA, ajustes_buscar("cinemeta", resultados, AJ_N)) >= 0);
  /* OLED: esmaecer quando parado (padrao 5 min) e brilho da interface do player (padrao 80%). Ultimas, locais. */
  assert(AJ_ESMAECER == AJ_BUSCA_CINEMETA + 1 && AJ_BRILHO_PLAYER == AJ_N - 8 && AJ_BRILHO_PLAYER == AJ_ESMAECER + 1);
  assert(!strcmp(CHAVE[AJ_ESMAECER], "esmaecerLocal") && !strcmp(CHAVE[AJ_BRILHO_PLAYER], "brilhoPlayerLocal"));
  assert(OPCOES[AJ_ESMAECER].n == 6 && OPCOES[AJ_BRILHO_PLAYER].n == 4);   /* 2.0: Desligado, 30 s, 1, 2, 5, 10 min */
  assert(valorPadrao[AJ_ESMAECER] == 3 && valorPadrao[AJ_BRILHO_PLAYER] == 1);
  assert(ajustes_esmaecer() == 3 && ajustes_brilho_player() == 1);
  assert(somenteDesteAparelho(AJ_ESMAECER) && somenteDesteAparelho(AJ_BRILHO_PLAYER));
  { int vE = 0, vB = 0, k; for (k = 0; k < AJ_N_TELA; k++) if (TELA[k].tipo == IT_OPC) { vE += TELA[k].op == AJ_ESMAECER; vB += TELA[k].op == AJ_BRILHO_PLAYER; } assert(vE == 1 && vB == 1); }
  assert(indiceResultado(AJ_ESMAECER, ajustes_buscar("oled", resultados, AJ_N)) >= 0);
  /* 2.0: "Novidades 2.0" e a ULTIMA opcao: acao em Sobre e ajuda, sem chave gravada. */
  assert(AJ_NOVIDADES20 == AJ_BRILHO_PLAYER + 1 && AJ_NOVIDADES20 == AJ_N - 7);
  assert(!strcmp(CHAVE[AJ_NOVIDADES20], "-novidades20") && valorPadrao[AJ_NOVIDADES20] == 0);
  { int vz = 0, k; for (k = 0; k < AJ_N_TELA; k++) if (TELA[k].tipo == IT_OPC && TELA[k].op == AJ_NOVIDADES20) vz++; assert(vz == 1); }
  assert(indiceResultado(AJ_NOVIDADES20, ajustes_buscar("novidades", resultados, AJ_N)) >= 0);
  /* Retomada (05/10): "Manter o video pronto ao sair" e a ULTIMA opcao. Avancada, local,
     DESLIGADA de fabrica (instalacao antiga sem a chave = nao retem), nunca no perfil seguro,
     e so com a saida para a home valendo. */
  assert(AJ_MANTER_VIDEO == AJ_NOVIDADES20 + 1 && AJ_MANTER_VIDEO == AJ_N - 6);
  /* 2.0: tela de descanso (estilo e fonte da vitrine), LOCAIS, no fim. */
  assert(AJ_DESCANSO_ESTILO == AJ_MANTER_VIDEO + 1 && AJ_DESCANSO_FONTE == AJ_RELOGIO_12H - 1);
  // Formato do relogio (2.0): local, no fim, padrao 24 h.
  assert(AJ_RELOGIO_12H == AJ_N - 3 && !strcmp(CHAVE[AJ_RELOGIO_12H], "relogio12hLocal") && valorPadrao[AJ_RELOGIO_12H] == 0);
  assert(!strcmp(CHAVE[AJ_DESCANSO_ESTILO], "descansoEstiloLocal") && !strcmp(CHAVE[AJ_DESCANSO_FONTE], "descansoFonteLocal"));
  assert(valorPadrao[AJ_DESCANSO_ESTILO] == 0 && valorPadrao[AJ_DESCANSO_FONTE] == 0);
  assert(somenteDesteAparelho(AJ_DESCANSO_ESTILO) && somenteDesteAparelho(AJ_DESCANSO_FONTE));
  assert(!strcmp(CHAVE[AJ_MANTER_VIDEO], "manterVideoLocal") && OPCOES[AJ_MANTER_VIDEO].n == 2);
  assert(valorPadrao[AJ_MANTER_VIDEO] == 1 && !ajustes_manter_video());
  assert(somenteDesteAparelho(AJ_MANTER_VIDEO) && !dePerfil(AJ_MANTER_VIDEO) && uxAvancada(AJ_MANTER_VIDEO));
  assert(familiaPreviaOpcao(AJ_MANTER_VIDEO) == AJPV_INTERFACE);
  { int vz = 0, k; for (k = 0; k < AJ_N_TELA; k++) if (TELA[k].tipo == IT_OPC && TELA[k].op == AJ_MANTER_VIDEO) vz++; assert(vz == 1); }
  { int m0 = valor[AJ_MANTER_VIDEO], r0 = valor[AJ_RELOGIO], s0 = valor[AJ_SAIDA_PLAYER], ps0 = perfilSeguro;
    valor[AJ_RELOGIO] = 0; valor[AJ_SAIDA_PLAYER] = 0; valor[AJ_MANTER_VIDEO] = 0; perfilSeguro = 0;
    assert(ajustes_manter_video() && !inativa(AJ_MANTER_VIDEO));
    perfilSeguro = 1; assert(!ajustes_manter_video()); perfilSeguro = 0;
    valor[AJ_SAIDA_PLAYER] = 1; assert(!ajustes_manter_video() && inativa(AJ_MANTER_VIDEO));
    valor[AJ_SAIDA_PLAYER] = 0; valor[AJ_RELOGIO] = 1; assert(!ajustes_manter_video() && inativa(AJ_MANTER_VIDEO));
    valor[AJ_MANTER_VIDEO] = m0; valor[AJ_RELOGIO] = r0; valor[AJ_SAIDA_PLAYER] = s0; perfilSeguro = ps0; }
  assert(strstr(ajudaOpcao(AJ_MANTER_VIDEO), "2 minutos") && strstr(ajudaOpcao(AJ_MANTER_VIDEO), "trailer"));
  assert(indiceResultado(AJ_MANTER_VIDEO, ajustes_buscar("retomar", resultados, AJ_N)) >= 0);
  // R4: second subtitle position/style, local, appended; default = as before (top, same as primary).
  assert(!strcmp(CHAVE[AJ_LEG2_POS], "legenda2PosLocal") && OPCOES[AJ_LEG2_POS].n == 2 && valorPadrao[AJ_LEG2_POS] == 0);
  assert(AJ_LEG2_TAMANHO == AJ_LEG2_POS + 1 && AJ_LEG2_COR == AJ_LEG2_POS + 2 && AJ_LEG2_FUNDO == AJ_LEG2_POS + 3 && AJ_LEG2_BORDA == AJ_LEG2_POS + 4);
  assert(somenteDesteAparelho(AJ_LEG2_POS) && !dePerfil(AJ_LEG2_BORDA));
  for (int i = AJ_LEG2_POS; i <= AJ_LEG2_BORDA; i++) { int vezes2 = 0, k; for (k = 0; k < AJ_N_TELA; k++) if (TELA[k].tipo == IT_OPC && TELA[k].op == i) vezes2++; assert(vezes2 == 1 && valorPadrao[i] == 0); }
  { int a0 = valor[AJ_LEG2_POS], t0 = valor[AJ_LEG2_TAMANHO];
    assert(!ajustes_leg2_junto() && ajustes_leg2_tamanho() == 0 && ajustes_leg2_cor() == -1 && ajustes_leg2_fundo() == -1 && ajustes_leg2_borda() == -1);
    valor[AJ_LEG2_POS] = 1; valor[AJ_LEG2_TAMANHO] = 3; valor[AJ_LEG2_COR] = 2; valor[AJ_LEG2_FUNDO] = 5; valor[AJ_LEG2_BORDA] = 3;
    assert(ajustes_leg2_junto() && ajustes_leg2_tamanho() == 100 && ajustes_leg2_cor() == 1 && ajustes_leg2_fundo() == 4 && ajustes_leg2_borda() == 2);
    valor[AJ_LEG2_POS] = a0; valor[AJ_LEG2_TAMANHO] = t0; valor[AJ_LEG2_COR] = valor[AJ_LEG2_FUNDO] = valor[AJ_LEG2_BORDA] = 0; }
  assert(!strcmp(CHAVE[AJ_DISCORD], "-discord"));
  assert(!uxTemPadrao(AJ_DISCORD));
  assert(familiaPreviaOpcao(AJ_DISCORD) == AJPV_RASTREIO);
  int nDiscord = ajustes_buscar("Discord", resultados, AJ_N);
  assert(indiceResultado(AJ_DISCORD, nDiscord) >= 0);
  int discordCount=0;
  for (int i=0;i<AJ_N_TELA;i++) if(TELA[i].tipo==IT_OPC && TELA[i].op==AJ_DISCORD) discordCount++;
  assert(discordCount==1);
  artePorSubmenu();
  cacheSeek();
  audioSyncSettings();
  zoomTpk();
  prazoDosAddonsIntegrado();
  padroesEValores();
  escoposPorValor();
  assert(mkdtemp(dir));
  assert(setenv("NUVIO_DADOS", dir, 1) == 0);
  dados_iniciar(dir);
  buscaECaminhos();
  bloqueadosESegredos();
  puts("ajustes_ux_dados: busca, padroes, escopo, dependencias e segredos ok");
  return 0;
}
