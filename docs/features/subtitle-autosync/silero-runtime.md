# Silero native runtime evidence

The native adapter uses the C API of ONNX Runtime **1.20.1**, independently of
the benchmark Python runtime. Runtime source commit:
`5c1b7ccbff7e5141c1da7a9d963d660e5741c319` (MIT).
The selected external Silero model is the pinned MIT model from the implementation
plan; no generated model or native library is included in this repository.

`src/audsilero.c` is worker-owned, with preallocated input/output tensors,
512-sample frames, 64-sample context and `[2,1,128]` recurrent state. CPU inference
has one thread, sequential execution and no spinning. The shared runtime is opened with `dlopen` only when creating a worker session
and closed after session destruction; capability probes do not load it.
The wrapper has bounded
segment storage; ORT still allocates its internal session/kernel memory.
Malformed model names/types/ranks, failed inference, nonfinite output and segment
overflow fail recoverably. Discontinuous PTS resets all accumulated segments.
Threshold is 0.5, gaps up to 300 ms merge, and segments below 200 ms are removed.
Partial final frames are zero padded and clipped to the real last sample.

## Reproduce conversion and native parity

Use Python 3.12 with `onnxruntime==1.20.1`, `onnx==1.17.0` and
`numpy==2.5.3`. Run `tools/convert-silero-model.py` on an external verified copy
of the pinned ONNX. Conversion uses Fixed optimization and type reduction.
Observed converted artifact on 2026-10-06:

- format: ORT; converter/runtime: 1.20.1
- bytes: **1,852,896**
- SHA-256: `c211d5f612376c9d7307a3569271f6ee9742d9a83597ad5a960758d5c62584fb`
- release URL: **unpublished**. Do not advertise/download a fabricated endpoint.

The conversion recipe writes a separate release manifest. Before release,
reproduce its checksum, publish the model as a separate immutable HTTPS asset,
include the upstream Silero MIT notice and update manager metadata. A minimal
runtime cannot load ONNX. Application builds using minimal ORT must set both
`NUVIO_SILERO_ORT` and `NUVIO_SILERO_MINIMAL`; until a converted artifact is
published, the model manager must report unsupported rather than fetch ONNX.

Run `tests/audsilero.sh` with `NUVIO_ORT_ROOT`, `NUVIO_SILERO_ONNX`,
`NUVIO_SILERO_MODEL`, `NUVIO_SILERO_WAV`, and `PYTHON` pointing outside the repo.
Tests compare real audio probabilities to original ONNX, with absolute tolerance
**5e-5** for host kernels (ARM NEON **1e-4**, selected with
`NUVIO_SILERO_PARITY_TOLERANCE=1e-4`), nonzero PTS, final partial frames and callback sizes 1/137/512/4096.
They also require byte-identical probability/segment reports across callback sizes,
reset/seek recurrent-state equality, and independently reconstructed segment
boundaries within one 32 ms frame. Set `NUVIO_SILERO_PARITY_SECONDS=300` for
full corpus windows.

On external AVA excerpt `914yZXz-iRs-0900-1200.wav`, first 30 seconds plus 137
samples, all 938 probabilities passed. Native 1.20.1 ONNX versus benchmark Python
1.30.0 ONNX maximum error was `6.55e-7`; native converted ORT versus original
ONNX maximum error was `4.17e-7`. These are host results, not ARM results.
Full 300-second replays of all three AVA excerpts passed every chunk size;
maximum error was `1.21e-5` across 28,125 probabilities per callback size.
A full 300-second native ORT replay took 0.68 seconds wall / 0.67 seconds user,
RTF 0.0023, process peak RSS 24,900 KiB on the development host. This includes
startup and output, and is process RSS rather than incremental TV memory.

## ARM inference and native corpus scoring

The cross-built ARMv7/NEON minimal runtime replayed the **same 4,800,000-sample**
raw PCM file as the host, at PTS 7,312,345 us: 9,375 frames and 39 segments.
Against the original ONNX Python reference, maximum absolute probability error
was **7.01546e-5**; against host converted ORT it was **8.2254e-5**. There were
zero threshold disagreements, and serialized speech segments were exactly
identical. ARM uses a separately documented **1e-4** numerical tolerance to
account for architecture-dependent kernel rounding; host remains at **5e-5**.
The final lazy-loaded adapter also passed ARM replay with chunks 1/137/512/4096,
nonzero PTS, final partial frames, reset and discontinuity equality over 938
frames (maximum error `3.27e-7`). The ARM executable has no ORT `DT_NEEDED`
entry, proving the runtime is loaded only when a session is created.
These are numerical checks of this pinned network and fixture, not universal
accuracy or timing guarantees.

The ARM replay ran through QEMU user-mode with Debian ARM soft-float glibc
2.36. QEMU with the SDK glibc 2.12 crashes before `main` even for a trivial
`puts("ok")` program, so that environment cannot validate the old TV loader.
The runtime ELF's imported symbol versions remain GLIBC_2.4, but execution on
real webOS hardware is still an open release gate. QEMU speed/RSS is not a TV
performance measurement.

`tools/benchmark-silero-native.py` scored actual host native ORT segments through
the existing benchmark parser/scorer, without substituting probability parity
for scoring. External `native-silero-worker.json` reports show:

| Corpus | Precision | Recall | Frame F1 | Boundary F1 (250 ms) |
| --- | ---: | ---: | ---: | ---: |
| Public fixtures (292.27 labeled seconds) | 0.92074 | 0.96432 | 0.94203 | 0.73558 |
| Three AVA movie windows (900 seconds) | 0.96748 | 0.68789 | 0.80407 | 0.59325 |

AVA results match the pinned prior benchmark. Its music-condition recall remains
0.56553. These are speech detection scores, not subtitle offset acceptance.

## Runtime build and release gates

`tools/build-silero-runtime.sh` uses MinSizeRel, CPU only, minimal ORT,
operator/type reduction and exceptions enabled. Never add `--disable_exceptions`:
ORT documents that it replaces error handling with `abort()`.
The converted model requires `com.microsoft.FusedConv`, so disabling all contrib
operators is invalid. Host prebuilt full runtime is **16,559,416 bytes** and
fails the 5 MB native budget by itself; it is only a test dependency.

The actual webOS SDK cross-build succeeded with the same converter configuration.
The stripped ARM runtime is **1,655,560 bytes**, including static libstdc++ and
libgcc. ELF dependencies are only `libdl.so.2`, `librt.so.1`, `libpthread.so.0`,
`libm.so.6`, `libc.so.6`, and `ld-linux.so.3`; all versioned imports are
`GLIBC_2.4`. No additional `libstdc++`, `libgcc_s`, or `libatomic` is needed.
This leaves room for the decoder inside the budget, subject to the final
application/package delta measured against the DTS base. Header/library testing
installation is external at `/tmp/nuvio-silero-runtime/install`.
`tools/build-silero-runtime.sh` produces the same include/lib installation layout.
 SDK host CMake required replacement because its libssl.so.1.1
was absent; pinned pip CMake 3.31.6 was used. GitLab Eigen download was denied;
its exact pinned source commit `e7248b26a1ed53fa030c5c459f7ea095dfd276ac`
was acquired from the eigen-mirror GitHub mirror and passed as preinstalled Eigen
(`NUVIO_SILERO_EIGEN`). The SDK ships glibc 2.12: `getauxval` is absent and
`AT_HWCAP2` is missing from its older headers. `tools/patch-silero-runtime.py`
applies an explicit source shim to ORT CPU detection and MLAS; it uses a finite
read of `/proc/self/auxv` and the Linux UAPI tag 26. The adapter uses the same
helper. Runtime links libstdc++ and libgcc statically, retaining exceptions,
rather than importing a newer C++ runtime into an old TV process. Static runtime
bytes count toward the budget.

ORT 1.20.1 ARM MLAS CMake enables `-mfpu=neon`; the adapter checks
Linux `AT_HWCAP/HWCAP_NEON` from `/proc/self/auxv` before creating a session. Older ARM CPUs are
unsupported. This does not establish compatibility with every webOS TV.

A successful host model load is not release approval. Release still requires:
ARM reduced runtime size and loader ABI, same-network ARM inference parity,
combined runtime plus auxiliary decoder native increment <=5,000,000 bytes,
actual TV performance/playback impact and real subtitle offset acceptance.
Until these gates and model publication pass, the feature remains unavailable
on the production TV build; do not silently use DSP as a substitute.

References: [ORT custom/minimal builds](https://onnxruntime.ai/docs/build/custom.html),
[ORT format conversion](https://onnxruntime.ai/docs/performance/model-optimizations/ort-format-models.html),
[Silero model source](https://github.com/snakers4/silero-vad/tree/1e261b036686cd0017d500ee96acd1c4ba572a9d).

## Same-network specialized evaluator assessment

If the combined minimal ORT plus decoder exceeds the native byte budget, an
exact fixed-network evaluator is plausible. The original pinned ONNX contains:

| Stage | External tensor shape / operation |
| --- | --- |
| STFT | convolution `[258,1,256]`, stride 128; magnitude from real/imaginary outputs |
| Encoder | convolutions `[128,129,3]`, `[64,128,3]`, `[64,64,3]`, `[128,64,3]`, ReLU; strides 1/2/2/1 |
| Recurrent | LSTM input/recurrent weights `[512,128]`, two 512-element biases, state `[2,1,128]` |
| Probability | convolution `[1,128,1]`, sigmoid and reduction |

Most remaining operators are exported shape/control-flow (`If`, slices,
reshape, gather, squeeze), with two LSTM nodes representing conditional graph
branches. A maintainable implementation would explicitly document the selected
16 kHz branch, gate order, padding and recurrent state; parse only bounded
initializer tensors from the verified external original ONNX, and never embed
or generate compiled weight arrays. Estimated arithmetic is roughly 0.7 million
multiply-accumulates per 32 ms frame (about 22 million per audio second).
A scalar/NEON evaluator could be tens of kilobytes of native code, but this is
an engineering estimate, not a measured implementation or performance claim.
It requires fresh numerical parity on all reference audio and ARM, plus exact
bounds checks on the external tensor reader before being a detector option.
No speculative evaluator or different network is enabled in this implementation.

The optional Eigen mirror archive used for the blocked GitLab download is
`https://github.com/eigen-mirror/eigen/archive/e7248b26a1ed53fa030c5c459f7ea095dfd276ac.tar.gz`,
SHA-256 `f9dd558b4e0c4b8cafdec90b902c1722d40cf6230a77d53c947af1cfa27d1afa`.
Extract it outside the repository and set `NUVIO_SILERO_EIGEN` to its directory.
For SDK container builds, use Python 3.12, pinned converter dependencies,
CMake 3.31.6, git/patch and shared `:z` mounts when multiple jobs share sources.
Set `NUVIO_SILERO_BUILD`, `NUVIO_SILERO_OPS`, `PYTHON`, `CC` and `CXX` to the
external build/config paths and SDK tools, then run the build script. Its output
is code/runtime/license files only; the generated model remains a separate
external release artifact.
