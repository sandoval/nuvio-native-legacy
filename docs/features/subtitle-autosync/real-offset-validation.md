# Native real subtitle offset validation

Measured on the Linux x86_64 host on 2026-10-06, using full ONNX Runtime
1.20.1 and the pinned original Silero ONNX model. This is not a TV measurement
or a successful release acceptance gate.

## Reproduce

No model, soundtrack, subtitle file, decoded PCM, or report is stored in Git.
The opt-in preparation downloads about 200 MB into the specified external cache.
Python 3 and FFmpeg are required; inference additionally needs the external
ONNX Runtime 1.20.1 headers/library and pinned model.

```sh
python3 tools/setup-autosync-real.py \
  --cache "$HOME/.cache/nuvio-vad-benchmark/e2e-real" --download
NUVIO_ORT_ROOT=/path/to/onnxruntime-linux-x64-1.20.1 \
NUVIO_SILERO_ONNX=/path/to/silero-16k.onnx \
NUVIO_E2E_CORPUS="$HOME/.cache/nuvio-vad-benchmark/e2e-real" \
  bash tests/audsync_real.sh
```

The runner verifies source fixture size/hash before decoding mono PCM16 at
16 kHz, verifies the model hash, and uses the real model manager to validate
its isolated persistent cache offline. Production `audsync` runs the real
Silero detector, the existing `audalign_estimar` candidate guard and reference
cropping. Only if that succeeds does `autosync_comparar` run with the existing
QUICK configuration and 30-second audio search radius. No thresholds are
modified. Each window contains exactly 4,800,000 PCM samples (300 seconds).
Producer chunks contain 997 samples to exercise non-frame-aligned input;
PTS comes from source sample position. There were zero ring drops.

The runner writes `native-offset-results.json` in the external cache. It returns
exit 2 when any matched positive window fails to recover the injected offset
within 250 ms or any negative window is accepted. Rejection is recorded as
rejection, not converted into a passing positive test.

## Sources and scope

[Sintel's official download page](https://durian.blender.org/download/) links
the Xiph mirror. [Xiph's Sintel directory](https://media.xiph.org/sintel/)
provides lossless stereo masters, an independent music/effects mix without
dialogue, and [Blender-authored subtitles](https://media.xiph.org/sintel/subtitles/).
The subtitle README identifies Blender Foundation copyright and CC BY 3.0.
Fixture hashes and exact sizes are pinned in the setup script. The audio
checksums match Xiph's published SHA256SUMS.txt.

[Blender's Elephants Dream project](https://orange.blender.org/) identifies
[Xiph's audio masters](https://media.xiph.org/ED/) as the original lossless
soundtrack. The master README identifies the DVD stereo mix and CC BY 2.5.
The original Blender subtitle download host returned HTTP 403 in this
environment. The added ED matching captions are the real WebVTT film dialogue
used in [Dolby's official player caption example](https://optiview.dolby.com/docs/theoplayer/examples/subtitles-metadata/closed-captions-subtitles/),
from its linked THEOplayer CDN. They are explicitly identified as demo captions,
not represented as an independently verified Blender original subtitle master.
Their exact bytes/hash are pinned; they remain external. The timing proximity
of native candidate offsets supports matching-film timing but does not prove
frame-exact editorial alignment.

Sintel has sparse dialogue. ED supplies denser dialogue and a second film.
Negative controls use the actual no-dialogue music/effects track, ED audio with
Sintel captions, zeros in place of the audio, and explicit edits to real ED
caption timing: 2.5% drift across the window or an 8-second step after 150 s.
The drift/cut cases test failure of a constant-offset correction; they are
not claimed to be naturally occurring alternate movie cuts. Translated
captions cannot serve as a wrong-language negative because this system only
compares activity timings, not words or spoken language.

## Results

| Cases | Windows | Native result |
|---|---:|---|
| Sintel, injected +2500 / -1200 ms; starts 60 / 300 s | 3 | Refused: insufficient detected speech |
| ED matched demo captions, +2500 / -1200 ms; starts 60 / 240 s | 3 | Refused: competing candidate peaks too close |
| Sintel music/effects, ED wrong movie, ED drift/cut, silence | 5 | Refused; no false acceptance |

The ED positive candidate estimates were +2200, -1500, and +2150 ms, with
scores 0.464, 0.464, and 0.477. Their competing-peak margins were
0.070, 0.070, and 0.052, below the existing 0.08 ambiguity guard.
The public session maps that rejection to `low_confidence`; the candidates
were not accepted. The matched-film
candidate residual errors of 300–350 ms also exceed the proposed 250 ms
positive acceptance tolerance. The final acceptance engine was not reached
in these fixtures because the earlier candidate guard rejected them.

**Release acceptance remains failed: 0/6 positive cases recovered an accepted
offset; 0/5 negative cases were falsely accepted.** Additional real paired
movie/subtitle windows, baseline subtitle editorial timing verification,
wrong-language audio tracks, singing, and TV runs are still needed. Silero VAD
probability parity is separate evidence and cannot substitute for this gate.
