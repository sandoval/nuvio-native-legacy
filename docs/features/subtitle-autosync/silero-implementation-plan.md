# Silero subtitle AutoSync implementation handoff

Prepared 2026-10-06. This is an implementation plan, not implemented functionality.

## Objective and user requirements

Implement local subtitle synchronization using **Silero VAD** to detect speech
timings, without transcription. Make it an **opt-in setting, default off**.
When the user enables it, download the required model into persistent app data;
do not package any model weights in the application. Run inference locally.

Primary target is native **webOS TVs**. Reuse the same detector on Android's
existing PCM tap where feasible; do not make Samsung support a prerequisite.
The previous user constraint remains: added shipped binary size must not exceed
**5,000,000 bytes**. Count the executable and every added native library/runtime,
including the auxiliary decoder. Downloaded model bytes are separate. Measure
both unpacked shipped native bytes and package size; compression alone does not
demonstrate compliance. If the budget cannot be met, report the measured blocker
instead of silently exceeding it or changing the selected detector.

The user subsequently authorized a stacked PR on top of DTS if reuse is
worthwhile; this is the recommended implementation route. Preserve DTS playback
behavior and share its generic infrastructure. No server-side audio processing, ASR,
microphone capture, automatic model download on first launch, or transmission of
audio/subtitle contents is part of this work.

## Checkout context for the fresh session

- Work in `/home/sandoval/code/nuvio-native-legacy`, branch `subtitle-autosync`.
- Tracked HEAD currently equals upstream master 2.0:
  `f800f67f6fc35b733b5159f17fa23c98c508d455`. Confirm status before changing it.
- Remote `upstream`: `https://github.com/iqui27/nuvio-native-legacy.git`.
  Remote `origin`: `https://github.com/sandoval/nuvio-native-legacy.git`.
- DTS commit `2a97f7e7` was removed from the current branch; it remains on
  `sandoval/dts`. The user has now authorized implementing AutoSync as a
  **stacked PR based on DTS**, because sharing transport/demux/build code is
  worthwhile. This plan update does not itself move the checkout. Follow the
  stack workflow below in the fresh implementation session.
- This plan, `benchmark.md`, `tools/benchmark-vad.py`, `tools/vad-benchmark/`
  and `tests/vad_benchmark.py` are currently **untracked local handoff files**.
  They must accompany this plan if work moves to another checkout/session.
  Existing `logs/` and `local.properties` are unrelated local files: preserve
  them and do not expose their contents in reports or commits.
- Media, models, dependencies and results from research are external under
  `~/.cache/nuvio-vad-benchmark/`. Generated DTS deployment artifacts were moved
  under `removed-dts-artifacts-*` there; do not package them.
- Read any applicable repository instructions discovered in the new session.
  The old planning documents are background, not permission to change scope.

## Stack workflow and review scope

Implement on top of the DTS branch instead of copying its infrastructure into
an independent master-based implementation. Start by inspecting/fetching the
actual DTS branch and identifying its PR if one exists. The last inspected tip
is `2a97f7e7`; do not assume it is still the remote tip. Preserve the untracked
handoff files before switching branches or using another checkout.

Create/use an AutoSync implementation branch from that DTS base. The AutoSync PR
must target the DTS branch while DTS is unmerged, so its diff contains only
AutoSync and necessary shared refactors. Link the dependency in the description.
If DTS has already merged, base/rebase onto current upstream master instead.
After DTS merges, rebase/retarget the AutoSync PR and verify its diff again.
Do not rewrite the DTS branch or change its PR just to prepare this stack.

Prefer a small shared Range/AVIO/demux layer used by both DTS playback and the
new analysis decoder. Keep separate decoder state, buffers, cancellation and
resource budgets for independent playback/analysis sessions. Do not attach
analysis-only resampling or additional consumers to the active DTS playback
worker in a way that adds stalls or changes timing. DTS playback continues to
need its encoder/video filters; analysis never invokes those paths.

Extend the existing minimized FFmpeg build to include the extra audio decoders;
link shared libraries once rather than packaging a second FFmpeg copy. Existing
DTS components remain enabled. Measure the AutoSync increment against the DTS
base and also report total shipped size versus upstream master; the 5 MB
AutoSync budget is not waived. Run existing DTS transport/engine/playback tests
alongside new analysis tests. Default-off must leave DTS playback unchanged.
No model/media artifacts belong in Git or the application package.

## Existing architecture: reuse it

| Files | Relevant behavior |
|---|---|
| `src/ajustes.c`, `src/ajustes.h` | Existing default-off `AJ_LEG_SYNC_AUDIO`, local persisted key `legendaSyncAudioLocal`. Getter currently returns zero outside Android. Check row definitions/visibility as well as getter. The preference is explicitly excluded from account sync. |
| `src/player.c` | Calls `legsync_audio_habilitar(ajustes_legenda_sync_audio())` during player updates. Contains seek/buffering/source lifecycle integration. |
| `src/legsync.c`, `src/legsyncui.c`, `src/legendasui.c` | Own subtitle selection, automatic reference selection, explicit audio action, progress, accepted automatic offset and Undo. Prefer embedded subtitle reference when available; audio is the fallback. |
| `src/audsync.c`, `src/audsync.h` | One session worker, bounded four-second mono PCM ring, timestamp continuity, cancellation generations, reference construction. Currently directly embeds/calls `AudVad`. |
| `src/audvad.c`, `src/audvad.h` | Current DSP detector **and** reusable `AudSeg` representation / `audalign_estimar` candidate alignment. Separate detection from alignment rather than throwing this module away. |
| `src/autosync.c`, `src/autosync.h` | Final acceptance engine, not a VAD. Only ACCEPTED results change the subtitle offset. Keep its confidence, competing-peak, regional agreement and boundary safeguards. |
| `src/video_android.c`, Android `AudioSyncSink.kt` / `AudioSyncTap.kt` | Existing timestamped decoded playback PCM source. Tap must stay nonblocking; passthrough is explicitly unavailable. |
| `src/video.c` | webOS uMediaServer/ACB player. Passes media URI to hardware pipeline; does **not** deliver playback PCM to the app. |
| `src/legref.c`, `src/mkv.c`, `src/rede.c` / `.h` | Existing cancellable HTTP/Range and indexed MKV subtitle extraction. Useful patterns, not a complete audio demuxer/decoder. |
| `src/dados.c` / `.h`, `src/atualizacao.c` | Discovered writable app data directory, atomic writes; updater contains an existing SHA-256 implementation to assess extracting into a reusable helper. |
| `tools/arm.sh`, `tools/Dockerfile`, Android CMake, other host build scripts | webOS ARM toolchain/build integration. `src/*.c` is compiled directly; a C++ runtime requires explicit library build/link steps. Existing curl is dynamically loaded: do not add a rigid `-lcurl` dependency. |

Read `docs/plans/player-1.8/AUDIOSYNC-CAPACIDADE.md` and the headers above.
Its optional Whisper/ASR proposal is **not** this implementation.

Current flow: timestamped mono 16 kHz PCM → VAD segments → candidate offset
on a 50 ms activity grid → speech reference and cropped subtitle documents →
existing AutoSync acceptance → primary subtitle automatic offset. Manual offset
and Undo remain independent. Positive offset **advances** subtitles.

Current audio window is 300 seconds; search radius is ±30 seconds. The final
engine requires at least 180 seconds of overlap, sufficient speech, and other
checks. Android listening therefore takes about five minutes of playback.
Do not promise instant correction or shorten the window without separate
alignment evidence. webOS auxiliary decoding can process a media window ahead
of real time, subject to playback resource limits.

## Evidence and limitations already established

The pinned benchmark compares actual Nuvio DSP, WebRTC/libfvad and Silero with
threshold 0.5, gap merge 300 ms, minimum speech 200 ms. On three five-minute
AVA-Speech movie excerpts, Silero achieved precision 0.967, recall 0.688,
frame F1 0.804 and boundary F1 0.593 (±250 ms). It had the fewest false positives;
music-condition recall was only 0.566. These are **VAD results, not evidence of
correct subtitle offsets**. AVA labels include singing as speech. The subset
is deliberately condition-covering, not representative of all movies or a
verified held-out split for Silero. No runtime/decoder measurements on TV exist.

Reproduce without adding media to Git:

```sh
python3 tools/benchmark-vad.py --corpus ava-speech --setup
python3 tests/vad_benchmark.py
```

See `benchmark.md`. Existing external reports are in
`~/.cache/nuvio-vad-benchmark/ava-speech/results/`; files may be absent in a
different environment. The manifest and URLs are the reproducible source.

## Milestone 1: prove the runtime and PCM path before building UI

### Selective reuse from the DTS branch

Inspect the fixed commit `2a97f7e7`, rather than relying on a moving branch:
`git show 2a97f7e7:src/dts/dts_engine.c`. Extract generic components into the
shared infrastructure on the DTS base. Avoid duplicating it in a second module
or cherry-picking the complete DTS commit into a master-based AutoSync branch.

| Existing DTS code | Reuse for AutoSync |
|---|---|
| `src/rede.c` / `.h`: `rede_baixar_trecho64_cab` and helpers | Strongest candidate for nearly direct reuse: 64-bit byte offsets (important for >2 GiB files on ARM), bounded bodies, validated Content-Range/206, provider headers, redirect credential handling and cancellable transfers. Rename DTS-specific helpers and re-run transport tests. |
| `src/dts/dts_engine.c`: custom AVIO `read_range` / `seek_range`, initialization, demux track metadata | Share Range-backed FFmpeg I/O and MKV/MP4 demuxing. Give analysis a separate smaller budgeted queue; preserve playback prefetch: existing defaults use four workers and up to 32 MiB of speculative payload plus an active 1 MiB cache. Account for fetched/discarded bytes, not just consumed ranges. |
| Same engine: decoder send/receive loop, resampling and timestamp/seek reset patterns | Adapt, not copy as a complete backend. It currently selects only DTS and outputs encoded AAC; stop at decoded PCM, select the actual codec/track, resample to mono 16 kHz, remove encoder/FIFO assumptions and validate timestamp/preroll behavior. Current resampling allocates per decoded frame; replace that with bounded reusable scratch storage. |
| `tools/build-dts-ffmpeg.sh` | Reuse the pinned-source checksum, minimal cross/host build and license provenance structure. Extend the shared build with required audio decoders. Retain AAC encoder, DTS decoder/parser/filter, subtitle decoding and video bitstream filters for existing DTS consumers; the analysis path does not use them. AAC parser/encoder in the old script does **not** provide AAC decoding. |
| `tests/dts_engine_avio.c`, `tests/dts_engine_server.py`, `tests/dts_range.c`, associated runners | Port applicable exact-byte/seek/short-range/cancel/header/redirect fixtures to generic transport tests. Confirm filenames with `git ls-tree -r --name-only 2a97f7e7 tests`. Do not carry prefetch throughput expectations or DTS-to-AAC output assertions into analysis tests. |

The `dts_engine_next` public interface returns encoded packets, not a usable
PCM callback. Neither `dts_engine_available` (requires DTS decode/AAC encode/
video bitstream filters) nor full `dts_engine_open` (video/DTS playback setup)
is suitable unchanged. Keep `dts_playback`, `dts_pipeline`, starfish/NDL
adapter, overlays/subtitles, debug app identity and player fallback/settings
behavior as inherited from the DTS base; they are not the analysis backend.
Share generic transport without reworking those playback features. Existing tests are useful evidence,
not proof of new codec support or TV performance.

1. Build a host C-callable Silero adapter using ONNX Runtime's C API. Pin the
   inference runtime release independently of the Python benchmark environment;
   Python wheel version 1.30.0 is not proof that webOS supports that release.
2. Cross-build a CPU-only, operator/type-reduced runtime for the actual webOS
   ARM ABI and SDK. Try a minimal ORT-format build using the pinned 16 kHz model.
   Generate the required operator/type configuration during offline conversion;
   pin converter and runtime versions. **Minimal ORT builds do not load ONNX**:
   the downloadable artifact must be the converted `.ort`, with its own size
   and checksum. Validate probability parity with the original ONNX model.
3. Measure executable/library deltas, loader dependencies, glibc/libstdc++/
   libatomic requirements, CPU feature assumptions, peak native RAM and RTF.
   Do not assume NEON or compatibility with every older TV without probing.
   Preserve recoverable errors; avoid size options that turn runtime failures
   into process aborts. The Python benchmark's RSS is not native runtime RAM.
4. Build an independent webOS audio acquisition spike. There is no working
   uMS PCM tap in master. Use an auxiliary demux/decode pipeline reading the
   **same media URL, request headers and selected audio track** as playback.
   It must not replace the player or change its audio output/passthrough.
5. Start with seekable HTTP(S) MP4 and Matroska, and AAC/AC3/EAC3/PCM decoding;
   include Opus if the combined size budget permits. Use a minimized LGPL
   FFmpeg library build shared with DTS. The **analysis path** uses only demux,
   audio decode and resample; it does not encode or route playback. Preserve
   components needed by DTS, and package no FFmpeg CLI. Prefer custom AVIO over the
   existing cancellable HTTP client, with bounded Range requests. A normal
   standalone FFmpeg binary is not an acceptable size/runtime shortcut.
6. Seek using demux timestamps/indexes; preserve packet/frame PTS, stream
   time bases, codec delay/preroll and resampler delay. Never derive media time
   from wall clock or raw downloaded byte position. The benchmark found an
   Opus seek offset: use full-decode reference comparisons to prove alignment.
   Map the player's selected audio track to the demux track reliably; refuse
   ambiguous mappings rather than analyzing another language/track.
7. Prove correct mono 16 kHz PCM on host fixtures and a TV. For the first webOS
   release, unsupported live/HLS/DASH/DRM/nonseekable sources return a specific
   unavailable reason; never scan/download a whole remote movie as a fallback.
   A server ignoring Range must be detected before consuming a huge response.
8. Record runtime **plus decoder** size and playback impact before declaring
   feasibility. Initial targets: additional native peak RAM ≤64 MiB and
   processing RTF ≤0.10 on the reference TV, with no sustained buffer/frame
   regression. These are proposed acceptance targets, not measurements. Set
   explicit finite network/read/time budgets from the spike; begin with a
   conservative 64 MiB source-body and 120-second active-work cap per attempt,
   at most two Range requests/second. Stop/pause analysis when playback is
   sensitive; resource-budget exhaustion means no correction.

If minimal ORT cannot fit, assess a specialized evaluator for the **same pinned
Silero network** with external weights, numerical parity and maintainable
source/licensing. This is an explicit fallback investigation, not a switch to
WebRTC or an untested custom model. Do not evade the size constraint by silently
downloading native executable libraries. A model-only download is the requested
architecture. Document any unresolved size/ABI/PCM blocker before UI completion.

## Milestone 2: model artifact and opt-in lifecycle

Introduce a model manager, e.g. `src/audmodel.c` / `.h`, separate from playback
and inference. Model state is global to the local installation; inference
state is per audio analysis session. Reuse `legendaSyncAudioLocal` for user
intent rather than introducing a conflicting second enable switch.

- Settings label: **Automatic subtitle sync by audio**. Explain that processing
  happens on the TV and first enable downloads a small speech detection model.
  Show the exact download size from the release manifest, not an estimate for
  a different model format. No ASR/language selection is necessary.
- Off: no model requests, inference sessions or auxiliary decode work. Do not
  preload the runtime/model at startup. Existing subtitle/manual sync remains.
- Enable: persist user intent, start asynchronous download immediately, show
  progress and Cancel. Effective audio sync stays inactive until a verified
  model loads successfully. Never block the settings/UI/player thread.
- States: off, downloading, verifying, ready, failed. Failed state offers
  Retry and an understandable reason. Cancel behaves as disabling the feature.
  Unsupported builds show why the feature is unavailable, rather than a toggle
  that promises success. An unsupported current media file does not prevent
  installing the model for other supported files.
- Pin HTTPS URL, immutable model/version ID, format, exact bytes, SHA-256 and
  runtime compatibility in app code/release metadata. Reject oversized bodies,
  truncated transfers, checksum mismatches and incompatible model formats.
  Use bounded cancellable I/O from `rede.h`; do not execute downloaded code.
- Put files under a dedicated persistent `dados_dir()` subdirectory, not the
  deployment/package directory or the evictable artwork cache. If writable
  data discovery falls back to the package directory, treat model persistence
  as unavailable rather than placing weights in the installed package.
  Check free space for temporary plus final artifact with margin. Download to
  `.partial`, verify, then atomically rename; clean incomplete files after crashes.
- A verified cache is reused offline on later enable/restart. Revalidate its
  integrity before inference. Corrupt/missing cache leaves enabled intent in
  failed/not-ready state, with explicit Retry; never change subtitle offsets.
  Do not repeatedly retry/download on every UI frame or app startup.
- Disable invalidates the download/analysis generation, cancels I/O and engine
  work, detaches PCM, releases inference resources, and prevents stale callbacks
  from installing or applying anything. Retain verified cached weights for reuse.
  An explicit **Remove downloaded model** action disables first, releases users
  of the file, removes artifacts and reports freed storage.
- Preserve an already accepted offset when disabled (existing behavior); Undo
  remains explicit. Fix the current possibility of an in-flight final AutoSync
  result surviving disable. Restart/app close must join workers safely.
- Package the appropriate code license notices, but no model file, generated
  C weight array, compressed weights, ONNX initializer blob or converted model.
  Tests must inspect actual distributable contents to enforce this.

Initial research model (MIT; provenance pinned in benchmark manifest):

```text
commit: 1e261b036686cd0017d500ee96acd1c4ba572a9d
URL: https://raw.githubusercontent.com/snakers4/silero-vad/1e261b036686cd0017d500ee96acd1c4ba572a9d/src/silero_vad/data/silero_vad_16k_op15.onnx
bytes: 1289603
sha256: 7ed98ddbad84ccac4cd0aeb3099049280713df825c610a8ed34543318f1b2c49
```

Verify the download against the manifest before use. For a converted artifact,
generate and pin its independent checksum and exact size; never reuse an ONNX
checksum for `.ort`. Publish it at a stable release/CDN URL as a separate model
asset before calling the feature usable. Define the release recipe, licensing
notice and URL in the implementation; do not invent a nonexistent endpoint.

## Milestone 3: streaming Silero adapter and session integration

Add `src/audsilero.c` / `.h` with a C interface for create/reset/feed/flush/
destroy; own runtime session and all scratch/recurrent buffers in the worker.
Use fixed/bounded allocations and a CPU provider with one inference thread;
disable background thread spinning. Keep inference outside playback locks.

For the pinned model, match `Silero.run` in `tools/benchmark-vad.py`:

- Input mono PCM16 at **16 kHz**, converted to float by division by 32768.
- Inference frame **512 samples / 32 ms**, previous context **64 samples**,
  initial zero recurrent state **[2,1,128]**; sampling-rate input is int64 16000.
  Concatenate context/frame, run inference, retain new state and last 64 samples.
  Check model names, types, dimensions, finite outputs and successful return codes.
- Accumulate arbitrary callback chunk sizes into inference frames. Timestamp
  each frame from its first media sample and sample counts, not arrival time.
  Reset state, context, pending samples and segment state after seeks, overflow,
  audio format/track/source changes, buffering discontinuity, or new session.
- Start with probability threshold **0.5**, merge gaps ≤300 ms, drop speech
  segments <200 ms, final partial frame zero-padded for inference and clipped
  to actual media end. Preserve these benchmark defaults; changes require
  separately evaluated tuning data. Do not also add the old DSP onset/release
  delays to Silero output. Use bounded `AudSeg` storage and fail on overflow.

Refactor `audsync.c` to consume a detector-neutral segment result. Keep the DSP
path for its existing tests/unsupported build compatibility, but the enabled
Silero feature must not silently fall back to DSP if weights/runtime fail.
`audalign_estimar`, document cropping and final `autosync_comparar` remain the
alignment/acceptance authority. Capability requires user enabled + model ready
+ runtime supported + PCM source/track available; distinguish each failure.

Keep `audsync_pcm`'s bounded copy-only producer contract. Extend source lifecycle
for the webOS auxiliary worker, using the same session/track/seek generations
as `legsync`. Sample only one analysis window at a time; cancel/pause promptly
on source/track changes, new subtitle, manual sync changes, seek/buffer, player
close and setting disable. Do not alter the user's playback audio settings.

In `legsync`, retain embedded-reference precedence and activate Silero audio
fallback automatically only when opted in, model ready and source capable.
Expose the existing explicit audio-sync action too. Rejected/ambiguous/no-speech
analysis changes **nothing**. Scope is the primary external subtitle; do not
automatically shift the secondary subtitle. Keep manual + automatic sign/math,
Undo and stale-session guards. Explain download/listening/analysis/failure
states in localized settings and subtitle UI; no repeated disruptive toast.

## Milestone 4: verification and release acceptance

1. Model manager tests: default-off zero downloads, enable starts exactly one,
   offline verified reuse, cancel/disable race, retry, disk-full/read-only,
   oversized/truncated/wrong-hash/incompatible artifacts, removal while in use,
   crash leftovers and model version migration. Assert no stale result applies.
2. Native inference parity: replay real external WAVs against the pinned Python
   ONNX reference, compare probabilities within a documented numerical tolerance
   and segment boundaries within one 32 ms frame. Replay differently sized
   chunks, nonzero PTS, final partial frame, discontinuities and reset state.
   Verify converted ORT and cross-built ARM output, not just host ONNX output.
3. Keep existing DSP synthetic tests independent. Run relevant suites:
   `tests/autosync.sh`, `tests/audsync.sh`, `tests/legsync.sh`, Android tap checks
   if touched, plus benchmark parser/scoring tests. Fedora's newer GCC may
   expose a pre-existing integer/pointer warning in legacy fixtures; diagnose
   and document it rather than weakening application compiler checks globally.
4. Auxiliary decoder tests: small generated MP4/MKV fixtures, nonzero container
   start, codec delay, selected-track mapping, Range rejection, malformed packets,
   cancellation, source authentication headers, resampling and budget limits.
   Compare sought-window PCM timestamps with full-decode references. Keep
   corpus downloads outside Git. `mkvmerge` is installed in this environment;
   portability requires documenting FFmpeg/mkvmerge fixture dependencies.
5. Run the native backend through the same public and AVA benchmark corpora,
   preserving parameters and recording versions/compiler/runtime/format.
   Then validate **end-to-end offset acceptance** using ≥300-second real audio
   windows paired with matching timed subtitles, with known injected ±offsets.
   Include silence, music/singing, wrong movie/language, sparse/repeated dialogue,
   cuts and drift. Correct accepted offsets should recover injected shifts
   within ±250 ms; negative/ambiguous cases must leave offsets unchanged.
   Do not substitute AVA speech labels for real subtitle cues in this gate.
6. ARM package checks: no weight artifacts in the IPK/deploy tree, packaged
   native delta ≤5 MB, no unexpected loader dependencies, license notices and
   exact downloadable artifact provenance. If feasible, add a repeatable size
   assertion to build tooling. Do not deploy just because a build script can;
   use `tools/arm.sh --build` / `--ipk --build` until deployment is authorized.
7. Real TV checks: first enable/progress/cancel/remove/restart/offline cache,
   supported and unsupported streams, correct audio track, passthrough unchanged,
   seeks and buffering, late disable, offset/Undo. Record RTF, incremental native
   peak RAM, source bytes, playback buffer/frame impact and package delta.
   Host success alone does not establish webOS support.

Deliver implementation, reproducible runtime/model/decoder build recipes,
settings/localization, native/manager/integration tests, package/size evidence,
and a short supported-platform/container/codec matrix. Update capability docs
with **proved** behavior. Limit DTS changes to necessary shared refactors with
regression coverage; no model/media files in Git.

## Primary references

- [Silero upstream](https://github.com/snakers4/silero-vad) and the pinned model
  URLs/checksums in `tools/vad-benchmark/manifest.json`.
- [ONNX Runtime custom/minimal builds](https://onnxruntime.ai/docs/build/custom.html).
- [ORT format conversion](https://onnxruntime.ai/docs/performance/model-optimizations/ort-format-models.html)
  and [operator/type reduction](https://onnxruntime.ai/docs/reference/operators/reduced-operator-config-file.html).
- [LG audio stream access discussion](https://forum.webostv.developer.lge.com/t/accessing-audio-data-stream/9355):
  public web-app APIs do not offer playback PCM capture; for this native app,
  the stronger direct evidence is its current `video.c` and capability document.
- [AVA-Speech paper](https://arxiv.org/html/1808.00606v2) and local `benchmark.md`.

Start the fresh session by reading this plan, checking branch/status and opening
the listed source headers. Implement Milestone 1 first; do not mistake a model
download toggle or Android-only inference proof for completed webOS AutoSync.
