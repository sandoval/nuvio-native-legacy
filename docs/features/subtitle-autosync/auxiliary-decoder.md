# Auxiliary audio decoder

`auddecode` is a separate worker-owned FFmpeg demux/decode/resample session. It
never invokes the DTS AAC encoder or modifies playback. The existing validated
64-bit `rede` HTTP Range transport is shared, with an optional per-analysis
budget; DTS retains its existing concurrent prefetch queue and legacy wrapper.
Custom AVIO allocation, finite probing, container whitelist and overflow-safe
seek arithmetic are shared through `mediaio_ffmpeg.h`. There is one FFmpeg library build, extended by `tools/build-dts-ffmpeg.sh`.

The public interface in `src/auddecode.h` emits borrowed mono PCM16 at 16 kHz and
absolute container media timestamps in microseconds. Consumers must map the
player timeline to container timestamps where these differ. Track indices are
absolute demux indices. A negative track index is allowed only for a single
audio stream. Native player ordinals or language labels alone do not establish
this mapping; multi-track sources without proven mapping are unavailable.

| Source | Decoder behavior |
| --- | --- |
| Seekable HTTP(S) MP4/MKV | Validated 206 Content-Range and known content size required |
| AAC, AC3, PCM16 | Host generated-fixture decoding and seek comparison passed |
| PCM24/32, floating PCM | Built, not yet fixture-proven |
| EAC3 | Built; fixture test runs when host FFmpeg provides its encoder |
| Opus in MKV | Host seek comparison passed within container 1 ms timestamp precision |
| HLS/DASH, live, local files, unknown duration, Range ignored | Refused |
| DRM or malformed packets | Decode/open fails with no usable result |
| Ambiguous track | Refused |

Each attempt stops after 64 MiB of received source bodies or 120 seconds elapsed.
Every HTTP redirect hop counts as a request, with at least 500 ms between starts.
Rejected and redirect bodies count toward the limit. libcurl callbacks reserve
16 KiB under the body ceiling to account for bytes already received when aborting.
There is no speculative analysis prefetch. Range payloads are <=1 MiB; AVIO has a
32 KiB buffer and output PCM a fixed 64 KiB buffer. Decoder allocations depend on
codec metadata and still require native peak-RAM measurements.

Seek uses stream indexes/timestamps with two seconds requested preroll. Actual
cluster preroll over 15 seconds is refused, preventing an unindexed container
from falling back to scanning its entire prefix. Decoder timestamps and
resampler delay establish the first sample; subsequent sample counts preserve
continuity. Missing timestamps, jumps exceeding stream timestamp precision plus two
samples (capped at 4 ms), channel/format changes
and oversized decoded frames fail the attempt. PCM is clipped to the requested
window; resampler delay is flushed at EOF. Cancellation is concurrent-safe;
open/next/metrics/destroy belong to the worker, and destroy follows worker join.

## Reproduction

```sh
NUVIO_DTS_HOST=1 CC=gcc NUVIO_DTS_ROOT=/tmp/nuvio-dts-host \
  tools/build-dts-ffmpeg.sh
sh tests/dts_range.sh
sh tests/dts_engine.sh
sh tests/auddecode.sh
```

Fixtures are generated temporarily outside Git. Dependencies are a host C
compiler, Python 3, shared minimized FFmpeg libraries and a FFmpeg CLI containing
AAC/AC3/Opus encoders. EAC3 fixtures are explicitly skipped when the system CLI
lacks an EAC3 encoder. No FFmpeg CLI or media belongs in the application package.

The host test compares sought PCM with a full-decode reference trimmed at media
timestamps. For generated two-tone/chirp fixtures, AAC and PCM seek alignment
matched to the sample, 44.1 kHz AAC differed by less than one resampled sample,
AC3 differed by 5 samples (0.3125 ms), and Opus differed by 16 samples (1 ms).
MKV's millisecond time base and fractional codec delay prevent claiming exact
sample parity there. After compensating at most that documented timestamp
precision, PCM RMS difference is bounded to 300 PCM16 units; this permits lossy
codec warmup and fractional resampler phase differences. It is not a TV latency
or real-media synchronization acceptance measurement.

HTTP fixtures additionally check authentication, nonzero container start,
selected stream, ambiguous mapping refusal, rejected Range, malformed source,
idle cancellation, received-body/deadline bounds and request spacing. DTS
regressions cover >2 GiB ranges, redirects/credential stripping, repeated seek,
short ranges, persistent prefetch and byte-identical AAC conversion. Actual TV
PCM/RTF/RAM/playback-impact and shipped-size gates remain release requirements.
