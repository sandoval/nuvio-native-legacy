#ifndef NV_AUDSOURCE_H
#define NV_AUDSOURCE_H
/* UI-thread bridge to an independent, cancellable analysis decoder. Playback
 * owns neither its decoder nor its buffers. No work starts before tap enable. */
void audsource_update(const char *url, const char *headers, int stream,
                      int selection, double position, int capable);
void audsource_stop(void);
void audsource_destroy(void);
#endif
