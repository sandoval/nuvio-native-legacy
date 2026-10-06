#ifndef NV_AUTOSYNC_H
#define NV_AUTOSYNC_H
#include "legenda.h"
/* Positive offsets advance subtitles, like Legenda's atrasoMs. */
typedef enum { AUTOSYNC_QUICK, AUTOSYNC_THOROUGH } AutoSyncModo;
typedef enum {
  AUTOSYNC_UNAVAILABLE, AUTOSYNC_ANALYSING, AUTOSYNC_ACCEPTED,
  AUTOSYNC_REJECTED, AUTOSYNC_CANCELLED
} AutoSyncEstado;
typedef enum {
  AUTOSYNC_OK, AUTOSYNC_NO_REFERENCE, AUTOSYNC_INCOMPLETE,
  AUTOSYNC_FORCED_SIGNS, AUTOSYNC_SPARSE, AUTOSYNC_REPEATED,
  AUTOSYNC_LOW_CONFIDENCE, AUTOSYNC_AMBIGUOUS, AUTOSYNC_REGION_DISAGREEMENT,
  AUTOSYNC_SESSION_CHANGED, AUTOSYNC_BUDGET, AUTOSYNC_MEMORY,
  AUTOSYNC_EXCLUDED_REFERENCE, AUTOSYNC_INVALID_ARGUMENT
} AutoSyncMotivo;
typedef struct {
  AutoSyncModo modo;
  int toleranciaMs; /* residual alignment error, NOT search range */
  int raioBuscaMs;  /* maximum absolute constant offset, <=120000 */
  int orcamentoMs;  /* wall-time bound, <=20000 */
} AutoSyncConfig;
typedef struct {
  AutoSyncEstado estado;
  AutoSyncMotivo motivo;
  int offsetMs, regioes, erroMs, tempoMs;
  double confianca, alternativa;
  uint64_t documento, referencia, sessao;
} AutoSyncResultado;
typedef int (*AutoSyncCancelar)(void *usuario);
AutoSyncConfig autosync_config(AutoSyncModo modo);
const char *autosync_motivo(AutoSyncMotivo motivo); /* stable English event reason */
/* Pure comparison never changes playback/selection. Run on a worker. */
AutoSyncResultado autosync_comparar(const LegendaDocumento *doc,
                                   const LegendaDocumento *referencia,
                                   const AutoSyncConfig *config,
                                   AutoSyncCancelar cancelar, void *usuario);
/* One background worker per player context, two independent language slots.
 * No network, track switch or playback wait. UI polls on its own thread and
 * uses manual+automatic as render offset. Context retains documents. */
typedef struct AutoSync AutoSync;
AutoSync *autosync_criar(void);
void autosync_destruir(AutoSync *sync); /* cancel + join at player teardown */
void autosync_iniciar(AutoSync *sync, uint64_t sessao);
int autosync_selecionar(AutoSync *sync, int slot, LegendaDocumento *doc);
int autosync_solicitar(AutoSync *sync, int slot, LegendaDocumento *referencia,
                       const AutoSyncConfig *config);
void autosync_cancelar(AutoSync *sync, int slot);
// Cancel pending work atomically; preserve an accepted automatic offset.
void autosync_cancelar_pendente(AutoSync *sync, int slot);
int autosync_manual(AutoSync *sync, int slot, int atrasoMs);
int autosync_offset_ms(AutoSync *sync, int slot);
void autosync_desfazer(AutoSync *sync, int slot); /* preserves manual offset */
/* Excludes last reference in this session/selection; cancels stale work and
 * undoes auto correction. Caller picks another real reference and requests it. */
int autosync_tentar_outra(AutoSync *sync, int slot);
int autosync_referencia_permitida(AutoSync *sync, int slot,
                                  const LegendaDocumento *referencia);
AutoSyncResultado autosync_estado(AutoSync *sync, int slot);
#endif
