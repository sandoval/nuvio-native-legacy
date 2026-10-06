#!/bin/sh
# Transient generated fixtures only. Needs ffmpeg CLI with AAC/AC3/EAC3/Opus
# encoders and the shared minimized libraries from build-dts-ffmpeg.sh.
set -eu
cd "$(dirname "$0")/.."
PREFIX=${NUVIO_DTS_ROOT:-/tmp/nuvio-dts-host}
TMP=$(mktemp -d /tmp/nuvio-auddecode.XXXXXXXX)
SERVER_PID=''
trap 'if [ -n "$SERVER_PID" ]; then kill "$SERVER_PID" 2>/dev/null || true; fi; rm -rf "$TMP"' EXIT
${CC:-gcc} -std=gnu11 -Wall -Wextra -Werror -Wno-misleading-indentation -DNV_DTS_FFMPEG -I"$PREFIX/include" \
  src/auddecode.c src/rede.c src/redeurl.c tests/auddecode.c -o "$TMP/decoder" \
  -L"$PREFIX/lib" -Wl,--start-group -lavformat -lavcodec -lswresample -lavutil -Wl,--end-group -lm -lpthread -ldl
${CC:-gcc} -std=gnu11 -Wall -Wextra -Werror -Wno-misleading-indentation \
  src/rede.c src/redeurl.c tests/auddecode_budget.c -o "$TMP/budget" -ldl -lpthread
python3 - "$TMP/range.bin" <<'PYDATA'
import sys
open(sys.argv[1],'wb').write(bytes(1024*1024))
PYDATA
for codec in aac ac3 pcm_s16le libopus; do
  ext=mkv
  [ "$codec" = aac ] && ext=mp4
  ffmpeg -hide_banner -loglevel error -f lavfi -i 'aevalsrc=0.1*sin(2*PI*(731*t+8*t*t))+0.05*sin(2*PI*997*t):s=48000:d=9' -c:a "$codec" "$TMP/$codec.$ext"
done
if ffmpeg -hide_banner -encoders 2>/dev/null | awk '{print $2}' | grep -qx eac3; then
  ffmpeg -hide_banner -loglevel error -f lavfi -i 'aevalsrc=0.1*sin(2*PI*(731*t+8*t*t))+0.05*sin(2*PI*997*t):s=48000:d=9' -c:a eac3 "$TMP/eac3.mkv"
else
  echo 'EAC3 fixture skipped: system ffmpeg lacks EAC3 encoder' >&2
fi
ffmpeg -hide_banner -loglevel error -f lavfi -i 'sine=frequency=500:sample_rate=44100:duration=9' -c:a aac "$TMP/resample.mp4"
ffmpeg -hide_banner -loglevel error -i "$TMP/pcm_s16le.mkv" -map 0:a -map 0:a -c copy "$TMP/multi.mkv"
ffmpeg -hide_banner -loglevel error -itsoffset 7 -i "$TMP/pcm_s16le.mkv" -c copy "$TMP/nonzero.mkv"
ffmpeg -hide_banner -loglevel error -f lavfi -i 'aevalsrc=0.1*sin(2*PI*(731*t+8*t*t))+0.05*sin(2*PI*997*t):s=48000:d=30' -c:a pcm_s16le "$TMP/indexed.mkv"
printf 'invalid media packet\n' > "$TMP/bad.mkv"
python3 tests/auddecode_server.py "$TMP" "$TMP/port" "$TMP/requests" &
SERVER_PID=$!
for step in $(seq 50); do [ -s "$TMP/port" ] && break; sleep .1; done
PORT=$(cat "$TMP/port")
"$TMP/budget" "http://127.0.0.1:$PORT/range.bin"
for file in aac.mp4 ac3.mkv pcm_s16le.mkv libopus.mkv resample.mp4; do
  "$TMP/decoder" "http://127.0.0.1:$PORT/$file" -1 4 "$TMP/$file.sought.pcm" ok > "$TMP/$file.pts"
  # Full decode reference trimmed by media timestamps; avoid input -ss (preroll).
  ffmpeg -hide_banner -loglevel error -i "$TMP/$file" -af 'atrim=start=4:end=6' -ar 16000 -ac 1 -f s16le "$TMP/$file.full.pcm"
done
if [ -f "$TMP/eac3.mkv" ]; then
  "$TMP/decoder" "http://127.0.0.1:$PORT/eac3.mkv" -1 4 "$TMP/eac3.mkv.sought.pcm" ok >/dev/null
  ffmpeg -hide_banner -loglevel error -i "$TMP/eac3.mkv" -af 'atrim=start=4:end=6' -ar 16000 -ac 1 -f s16le "$TMP/eac3.mkv.full.pcm"
fi
"$TMP/decoder" "http://127.0.0.1:$PORT/indexed.mkv" -1 20 "$TMP/indexed.mkv.sought.pcm" ok >/dev/null
ffmpeg -hide_banner -loglevel error -i "$TMP/indexed.mkv" -af 'atrim=start=20:end=22' -ar 16000 -ac 1 -f s16le "$TMP/indexed.mkv.full.pcm"
"$TMP/decoder" "http://127.0.0.1:$PORT/nonzero.mkv" -1 9 "$TMP/nonzero.pcm" ok > "$TMP/nonzero.pts"
"$TMP/decoder" "http://127.0.0.1:$PORT/multi.mkv" -1 4 ignored reject
"$TMP/decoder" "http://127.0.0.1:$PORT/multi.mkv" 1 4 "$TMP/multi.pcm" ok >/dev/null
"$TMP/decoder" "http://127.0.0.1:$PORT/ignore/aac.mp4" -1 4 ignored reject
"$TMP/decoder" "http://127.0.0.1:$PORT/bad.mkv" -1 4 ignored reject
"$TMP/decoder" "http://127.0.0.1:$PORT/idle" -1 4 ignored cancel
"$TMP/decoder" 'file:///tmp/local.mkv' -1 0 ignored reject
python3 - "$TMP" <<'PY'
import array,json,sys
from pathlib import Path
root=Path(sys.argv[1])
for path in root.glob('*.sought.pcm'):
    a=array.array('h');a.frombytes(path.read_bytes())
    b=array.array('h');b.frombytes(path.with_name(path.name.replace('.sought','.full')).read_bytes())
    # Opus/AAC lossy seek warmup and resampling edge differences are bounded;
    # cross-correlation catches timestamp shifts masked by similar waveforms.
    n=min(len(a),len(b));assert n>=31900,(path,n)
    errors={lag:sum((a[i]-b[i+lag])**2 for i in range(200,n-200)) for lag in range(-64,65)}
    best=min(errors,key=errors.get)
    tolerance=16 if '.mkv.' in path.name else 1
    assert abs(best)<=tolerance,(path,best)
    assert (errors[best]/(n-400))**.5<300,(path,(errors[best]/(n-400))**.5)
    print(path.name,'full-decode alignment lag',best)
rows=[json.loads(line) for line in (root/'requests').read_text().splitlines()]
# Each decoder attempt opens a fresh source; check sequential ranges per group.
for a,b in zip(rows,rows[1:]):
    if a['path']==b['path'] and a['connection']==b['connection']:
        assert b['time']-a['time']>=.47,(a,b)
print('HTTP authentication, mono 16k, codec preroll, nonzero PTS, selected track, Range rejection and cancellation passed.')
PY
