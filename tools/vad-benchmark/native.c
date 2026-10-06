/* Host-only adapter for the VAD benchmark. Not linked into the application. */
#define _POSIX_C_SOURCE 200809L
#include "audvad.h"
#include "fvad.h"
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* Return pairs of start/end seconds, or -1 for allocation/output overflow.
 * Kernel time includes initialization, processing and flush; not Python/FFI.
 * The final incomplete WebRTC frame is zero padded, then clipped to real audio.
 */
int bench_nuvio(const int16_t *pcm, int n, double *pairs, int capacity,
                double *seconds) {
    double begin = now();
    AudVad *v = calloc(1, sizeof *v);
    if (!v) return -1;
    audvad_iniciar(v);
    audvad_pcm(v, pcm, n, 0);
    audvad_fechar(v);
    int count = v->nseg;
    if (v->transbordou || count > capacity) { free(v); return -1; }
    for (int i = 0; i < count; ++i) {
        pairs[2*i] = v->seg[i].inicio;
        pairs[2*i+1] = v->seg[i].fim;
    }
    free(v);
    *seconds = now() - begin;
    return count;
}

int bench_webrtc(const int16_t *pcm, int n, double *pairs, int capacity,
                 double *seconds, int mode) {
    double begin = now();
    Fvad *vad = fvad_new();
    if (!vad) return -1;
    if (fvad_set_sample_rate(vad, 16000) || fvad_set_mode(vad, mode)) {
        fvad_free(vad); return -1;
    }
    int count = 0, start = -1;
    for (int i = 0; i < n; i += 320) {
        int16_t tail[320] = {0};
        const int16_t *frame = pcm+i;
        if (n-i < 320) {
            for (int k = 0; k < n-i; ++k) tail[k] = pcm[i+k];
            frame = tail;
        }
        int speech = fvad_process(vad, frame, 320);
        if (speech < 0) { fvad_free(vad); return -1; }
        if (speech && start < 0) start = i;
        if (!speech && start >= 0) {
            if (count >= capacity) { fvad_free(vad); return -1; }
            pairs[2*count] = start/16000.0;
            pairs[2*count+1] = i/16000.0;
            ++count; start = -1;
        }
    }
    if (start >= 0) {
        if (count >= capacity) { fvad_free(vad); return -1; }
        pairs[2*count] = start/16000.0;
        pairs[2*count+1] = n/16000.0;
        ++count;
    }
    fvad_free(vad);
    *seconds = now() - begin;
    return count;
}
