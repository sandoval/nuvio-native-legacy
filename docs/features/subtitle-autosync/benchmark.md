# Speech timing benchmark

Run from the checkout with Python 3.12–3.14, a C compiler (`CC` is supported),
and FFmpeg:

```sh
python3 tools/benchmark-vad.py --setup
```

`--setup` creates `~/.cache/nuvio-vad-benchmark/venv`, installs pinned NumPy and
ONNX Runtime versions there, and runs the benchmark. It does not change the
application's dependencies or install PyTorch. Subsequent runs can reuse it:

```sh
~/.cache/nuvio-vad-benchmark/venv/bin/python tools/benchmark-vad.py
```

Use `--limit 3 --repeats 2` for a smoke run; omit `--limit` for all 31 clips.
`--cache /some/external/directory` relocates every generated/downloaded file.
The script rejects directories inside this checkout, including symlinks that
resolve inside it. Downloaded WAVs, annotations, model, source archive, licenses,
compiled libraries and reports are never placed in the repository. Only the
benchmark code and its URL/checksum manifest belong in version control.

## Sources

- [TEN VAD public test set](https://github.com/TEN-framework/ten-vad/tree/22a3bcd4509d0faaa8eef4881e8af5f39c178950/testset):
  30 WAV clips and manual binary interval labels (`.scv`, the upstream spelling).
  The publisher identifies LibriSpeech, GigaSpeech and DNS Challenge as source
  collections, but supplies no per-clip clean/noise/music category. Do not infer
  categories from file numbers or claim movie-domain coverage. The TEN repository
  license adds conditions to Apache 2.0; original recordings may carry their own
  terms. WAVs and labels are downloaded for local evaluation, not redistributed.
- [pyannote meeting sample](https://github.com/pyannote/pyannote-audio/tree/b749285c5cdd4636b2edc7f766f1352c8dde9369/tutorials/assets):
  a 30-second WAV and RTTM speaker-turn annotation. Overlapping speaker turns
  are unioned into speech; the remainder of the excerpt is non-speech. These
  speaker-turn boundaries are not a precise phonetic speech-boundary reference.

The manifest pins source commits and SHA-256/size for every download. A corrupt
cache file is downloaded again; a mismatched response stops the benchmark.
Upstream README/license files are saved alongside the data for provenance.
The report separates each dataset so the meeting sample cannot silently change
the interpretation of the TEN scores.

## AVA-Speech movie excerpts

Run the movie-domain comparison with:

```sh
python3 tools/benchmark-vad.py --corpus ava-speech --setup
```

Or reuse the original benchmark's Python environment:

```sh
~/.cache/nuvio-vad-benchmark/venv/bin/python tools/benchmark-vad.py --corpus ava-speech
```

This uses a separate `~/.cache/nuvio-vad-benchmark/ava-speech/` cache and reports
directory. The manifest `tools/vad-benchmark/ava-speech.json` pins the official
CSV annotations and three source movies from the
[CVDF mirror](https://github.com/cvdfoundation/ava-dataset).
It extracts the original movie's **900–1200 seconds**, producing three
five-minute mono 16 kHz WAVs (15 minutes total). CSV times are shifted by
900 seconds before scoring. Audio is decoded from the beginning and trimmed
on the normalized media timeline to avoid container/Opus input-seek offsets.
Full source videos are downloaded and verified,
so the initial download is approximately 640 MB; subsequent runs reuse them.

Selection was fixed before detector evaluation: the first three entries in
the official filename list whose first five annotated minutes contain more
than ten seconds of **each** label and whose source file is below 300 MB:
`-IELREHX_js`, `1j20qq1JyX4`, and `914yZXz-iRs`. This is a condition-covering
convenience sample, not a random or representative sample of the whole dataset.
It is not the paper's full benchmark or an independently verified held-out
test split for the pretrained models.

The [AVA-Speech paper](https://arxiv.org/html/1808.00606v2) defines four mutually
exclusive conditions: no speech, clean speech, speech with music, and speech
with noise. Noise can also include music. **Singing and music with lyrics count
as speech**; laughs/coughs/grunts do not. All speech conditions are unioned for
binary VAD scoring, so a change in background condition does not create an
extra speech boundary. Unknown annotation gaps are excluded. Reports also
include recall and labelled duration separately for each speech condition,
with no-speech false-positive rate reported independently.

Detector settings, segment cleanup, timing and frame/boundary scoring are the
same as the original comparison. Source videos, derived WAVs, labels and
reports stay outside version control; this does not redistribute movie media.

Measured on 2026-10-06, x86_64 host, five repetitions per detector/excerpt:

| Detector | Precision | Recall | F1 | No-speech false-positive rate | Boundary F1 ±250 ms | Wall RTF |
|---|---:|---:|---:|---:|---:|---:|
| Nuvio | 0.856 | 0.690 | 0.764 | 0.116 | 0.418 | 0.00027 |
| WebRTC | 0.594 | 0.923 | 0.723 | 0.634 | 0.399 | 0.00007 |
| Silero | 0.967 | 0.688 | 0.804 | 0.023 | 0.593 | 0.00285 |

| Detector | Clean speech recall | Speech with music recall | Speech with noise recall |
|---|---:|---:|---:|
| Nuvio | 0.848 | 0.493 | 0.829 |
| WebRTC | 0.939 | 0.880 | 0.969 |
| Silero | 0.805 | 0.566 | 0.759 |

The excerpts contain 449.30 seconds of no speech, 117.17 seconds of clean
speech, 193.43 seconds of speech with music and 140.10 seconds of speech with
noise. Silero has the highest F1 and fewest false positives in this subset;
WebRTC catches more speech but labels 63.4% of no-speech time as speech.
The music category is a substantial source of misses for Nuvio and Silero.
These measurements do not isolate whether misses are singing, spoken dialogue,
or background interference. They also do not measure subtitle alignment success.

## Detectors

- **Nuvio:** the actual `src/audvad.c` implementation, compiled with a small host
  adapter. Each recording gets a fresh `AudVad`, its media timeline starts at
  zero, and the end of the recording flushes an open speech segment.
- **WebRTC:** pinned [libfvad](https://github.com/dpirch/libfvad), compiled from
  source with the same compiler/optimization as Nuvio. Mono PCM16 at 16 kHz,
  20 ms frames, aggressiveness **2** by default (`--webrtc-mode 0..3`).
- **Silero:** pinned 16 kHz ONNX model, CPU provider, one intra/inter-op thread,
  threshold **0.5** (`--silero-threshold`). The runner implements the documented
  512-sample input, 64-sample context and recurrent state directly in NumPy,
  without importing PyTorch or executing downloaded Python code. State resets
  between recordings and repetitions.

FFmpeg converts each recording to mono 16 kHz PCM16 once, outside the timed region.
All approaches see identical samples. Final incomplete frames are handled:
WebRTC/Silero pad their last inference frame, then clip intervals to the original
audio length. Nuvio uses its own final partial-frame/flush behavior.

All output intervals receive the same additional cleanup: merge gaps ≤300 ms,
then discard intervals shorter than 200 ms. Override with `--merge-gap` and
`--min-speech`. Nuvio already has its native onset/hangover/merging rules;
WebRTC has internal hangover, and Silero has model context. This compares usable
segment output, not three mathematically identical raw frame classifiers.
Do not optimize thresholds against these evaluation labels; use a separate
development set if tuning is needed.

## Reports and interpretation

Outputs are in `~/.cache/nuvio-vad-benchmark/results/`:

- `summary.md`: compact comparison.
- `summary.csv`: aggregate numbers for analysis.
- `report.json`: settings, source revisions, normalized-file checksums, build
  fingerprint, dependency versions, per-recording intervals/scores/timings,
  aggregate scores and separate dataset summaries.

Frame scoring uses **10 ms midpoint samples**, the same grid for every detector,
with no forgiveness collar. Metrics include precision, recall, micro F1,
macro recording F1, missed speech and non-speech false-positive rate. SCV gaps
or unannotated tails are excluded rather than silently labelled non-speech.

Boundary scoring counts starts and ends separately, matching chronologically
one-to-one within **±250 ms**. File edges and boundaries adjacent to unknown
labels are excluded. Boundary F1 penalizes missing/extra edges; boundary mean
absolute error includes **only matched** edges, so read the two together.

Timing reports the median of five repetitions after an unmeasured warm-up.
Real-time factor (RTF) is processing seconds / audio seconds; lower is faster.
Wall and process-CPU times include the runner and exclude downloads, audio IO,
scoring and model loading. The report also records initialization separately.
Native kernel timings include detector initialization/flush; Silero kernel
timings surround each Python-to-ONNX Runtime inference call. Differences in
wrapper overhead mean these are not pure cross-language instruction benchmarks.

Each detector runs in a separate worker process. Peak RSS includes the host
Python interpreter, NumPy and (for Silero) ONNX Runtime; it is **not** detector-only
RAM. Combined host library size and ONNX model bytes are recorded, but neither
proves the final incremental webOS executable/package size is under 5 MB.

This is a small, labelled host comparison. AVA excerpts evaluate movie
soundtracks; the default corpus contains short non-movie clips. Neither run
measures ARM TV CPU load, decoding cost, subtitle offset acceptance or playback
smoothness. The current AutoSync engine requires much longer dialogue windows
than most of the default clips. Evaluate the alignment pipeline separately
before choosing a shipping detector or changing its acceptance rules.

## Benchmark verification

Scoring/label-parser tests require no downloads or third-party Python packages:

```sh
python3 tests/vad_benchmark.py
```
