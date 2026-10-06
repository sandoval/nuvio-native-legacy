# Silero AutoSync implementation and release status

Implemented on `subtitle-autosync` over DTS commit `2a97f7e772a224019e0abe900631d148d4350754`.
This is an opt-in implementation, default off. It is **not ready for TV release**:
the converted model has no published URL, actual TV performance is unmeasured,
and correct offset acceptance on real subtitles has not yet passed.

## Behavior

The settings preference `legendaSyncAudioLocal` remains local to the device.
Explicit enable downloads only the pinned model; persisted startup intent checks
the cache offline. Downloads are bounded, cancellable, verified by exact size,
SHA-256 and runtime model validation, then atomically installed in persistent
`subtitle-autosync` data. Disabling cancels pending work and keeps an accepted
offset available for Undo. Retry is explicit; removal disables first and waits
for inference file leases, including cleanup of obsolete versions and partials.
An unavailable runtime or unpublished model gives an unavailable setting.

Production detection uses Silero exclusively. DSP remains behind an explicit
legacy test definition. The worker uses 512-sample frames, 64-sample context,
threshold 0.5, 300 ms gap merge and 200 ms minimum speech. The four-second PCM
ring stays copy-only. The 300-second listening window, ±30-second search and
existing final AutoSync confidence/boundary safeguards remain unchanged.
Embedded subtitle references retain precedence. Only accepted results change
the primary subtitle's automatic offset; manual/secondary offsets stay separate.

webOS analysis uses a separate decoder worker for the player's URL, headers and
selected track. DTS shares generic Range/AVIO/container setup but keeps its own
prefetch, decoder and playback state. Analysis has no prefetch or encoder,
64 MiB received-body and 120-second limits, and at most two request starts per
second. Pause cancels acquisition; resume starts a fresh window at current PTS.

## Supported paths and evidence

| Path | Implemented/proved | Remaining limitation |
| --- | --- | --- |
| webOS ARMv7 with NEON | Reduced ORT builds with SDK; independent MP4/MKV decoder; package size audit | Actual TV loader, PCM, CPU/RAM and playback impact unmeasured; model unpublished |
| Native uMS track | Single audio stream; zero container origin required | Multi-track/origin mapping refused until proved |
| DTS track | Absolute FFmpeg stream index and container PTS | TV integration unmeasured |
| Android | Existing timestamped nonblocking PCM tap retained; shared Silero session code | Default build has no ORT; no new Android runtime packaging or device proof |
| AAC/AC3/PCM16/Opus | Host seek/full-decode comparisons passed | EAC3 and other PCM types build, but fixtures/device proof incomplete |
| HLS/DASH/live/DRM/nonseekable or ambiguous track | Specific refusal, no correction | Unsupported |
| Samsung | Unavailable | Outside this implementation |

## Validation

- Model manager: default-off, one download, offline verified reuse, checksum,
  truncation, incompatible format, generation cancellation, persistence refusal,
  crash/old-version cleanup and leased removal.
- Native Silero: real WAV probability and segment parity, arbitrary callback
  sizes, partial frame, nonzero PTS, reset/seek and recoverable missing model.
  Host full AVA windows have maximum probability error `1.21e-5`.
- ARM QEMU: 9,375 frames over 300 seconds, maximum difference `8.2254e-5`
  against host converted ORT, zero threshold disagreements and identical segments.
  Modern Debian ARM glibc was needed: SDK glibc 2.12 under QEMU crashed even a
  trivial program before main. ELF inspection alone cannot prove TV loading.
- Native VAD on public corpus: precision 0.921, recall 0.964, frame F1 0.942,
  boundary F1 0.736. Native AVA: precision 0.967, recall 0.688, frame F1 0.804,
  boundary F1 0.593. Parameters/labels match the existing benchmark. These are
  detection scores, not evidence of correct subtitle corrections.
- Real Sintel soundtrack/official subtitles and Elephants Dream soundtrack/demo
  captions: six matched 300-second windows with injected +2.5/-1.2 seconds were
  refused for sparse speech or competing candidate peaks. Five music/effects,
  wrong movie, drift, cut and silence controls were refused. No false acceptance,
  but no positive acceptance:
  **the real subtitle offset release gate failed**. Do not relax safeguards to
  make these cases pass. See [full offset evidence](real-offset-validation.md);
  verified subtitle editorial timing, more dialogue/language and TV coverage remain.
- Existing suites: AutoSync 562 checks, audio session 24 cases and subtitle
  session 19 cases passed. DTS Range/engine/playback/pipeline/subtitle regressions,
  production no-DSP gate, source lifecycle and benchmark/package parser tests ran.
- Android JVM check could not run: required cached Gradle/Kotlin dependencies
  were absent. Kotlin tap code was not changed.

Tests use external corpora and models. No media, model, native runtime or generated
weight arrays are checked in. See [runtime recipe](silero-runtime.md),
[decoder recipe](auxiliary-decoder.md) and [benchmark provenance](benchmark.md).

## Reproduction

```sh
bash tests/audmodel.sh
bash tests/audsource.sh
bash tests/audsync_gate.sh
bash tests/autosync.sh
bash tests/audsync.sh
bash tests/legsync.sh
bash tests/ajustes_ux_dados.sh
python3 tests/check_autosync_package.py
python3 tests/vad_benchmark.py
python3 tools/idiomas.py
```

For actual native corpora, compile `tests/audsilero_native.c` with
`src/audsilero.c`, `-DNUVIO_SILERO_ORT`, the external 1.20.1 C API include,
`-ldl -lm`, and configure `LD_LIBRARY_PATH` for that external runtime. Run
`tools/benchmark-silero-native.py --binary EXE --model MODEL.ort --cache CACHE`
against each benchmark's prepared external directory.

```sh
python3 tools/setup-autosync-real.py --cache /external/real --download
NUVIO_ORT_ROOT=/external/ort-1.20.1 \
NUVIO_SILERO_ONNX=/external/silero-16k.onnx \
NUVIO_E2E_CORPUS=/external/real bash tests/audsync_real.sh
```

The real-fixture command intentionally returns failure when positive offset
acceptance fails. Blender/Xiph media is external; the subtitle's attribution and
CC BY 3.0 notice are downloaded with it.

## Shipped size measurement

Identical ARM SDK GCC 14.2, stripped binaries, default libass and P2P disabled.
The extended FFmpeg is linked statically once; these totals include the decoder,
application, existing DTS adapters and the optional reduced runtime.

| Measurement | Bytes |
| --- | ---: |
| Upstream master native total (`f800f67f`) | 42,353,748 |
| DTS base native total (`2a97f7e7`) | 44,599,884 |
| AutoSync native total | 46,698,880 |
| **AutoSync increment over DTS** | **2,098,996** |
| Total increment over upstream master | 4,345,132 |
| Reduced ARM ORT library, included above | 1,655,560 |
| Actual DTS IPK | 57,576,918 |
| Actual AutoSync IPK | 58,630,390 |
| Compressed IPK increment | 1,053,472 |

The AutoSync native increment passes the **5,000,000-byte** ceiling with
2,901,004 bytes remaining. The actual IPK was unpacked and matched the
inspected deploy tree native bytes; no model/partial/static archive artifacts
were present. [Machine-readable evidence](package-size-evidence.json) records
every ELF and both payloads. This is a measured build variant, not a promise
about different optional components or future runtime/model versions.

Application ELF dependencies contain no ORT, libstdc++, libgcc_s, libatomic
or rigid libcurl link. ORT opens lazily through `$ORIGIN/lib`. The reduced
runtime imports only dl/rt/pthread/m/c/loader and `GLIBC_2.4`. Runtime and Silero
license notices are shipped; no model weights or FFmpeg CLI are shipped.

## Release build and model publication

`tools/build-dts-ffmpeg.sh` extends the shared minimized FFmpeg build.
`tools/build-silero-runtime.sh` builds reduced CPU ORT 1.20.1 outside Git using
the converted model's operator/type configuration. Build with the matching
SDK image containing that FFmpeg installation and identical optional components
for the DTS baseline. The runtime library is opened lazily, with no ORT
DT_NEEDED dependency in the application.

```sh
NUVIO_PROPERTIES=/dev/null NUVIO_CONTAINER_RUNTIME=podman \
NUVIO_BUILD_PLATFORM=linux/amd64 NUVIO_SDK_IMAGE=YOUR_EXTENDED_SDK \
NUVIO_P2P_MOTOR=none NUVIO_SILERO_ORT=1 \
NUVIO_SILERO_ROOT=/external/runtime/install \
NUVIO_AUTOSYNC_BASELINE_DIR=/external/dts/deploy/app \
bash tools/arm.sh --ipk --build
```

No deployment was performed. The optional runtime flag defaults to zero.
The converted ORT is 1,852,896 bytes with SHA-256
`c211d5f612376c9d7307a3569271f6ee9742d9a83597ad5a960758d5c62584fb`.
Publish it separately at an immutable HTTPS URL with the Silero MIT notice,
set its external conversion manifest to `published`, and run
`tools/verify-silero-release.py MANIFEST --header PREFIX/include/audmodel-release.h`.
The tool downloads/verifies the remote bytes and writes URL metadata only.
Until publication, the minimal-runtime build refuses enable rather than
downloading incompatible ONNX. Publishing the model is not sufficient to lift
the remaining TV and real-offset release gates.
