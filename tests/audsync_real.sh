#!/bin/bash
# External corpus files stay in an explicit cache; this never downloads them.
set -euo pipefail
cd "$(dirname "$0")/.."
: "${NUVIO_ORT_ROOT:?external ORT 1.20.1 installation required}"
: "${NUVIO_SILERO_ONNX:?pinned original ONNX file required}"
: "${NUVIO_E2E_CORPUS:?external directory from tools/setup-autosync-real.py required}"
python3 tools/setup-autosync-real.py --cache "$NUVIO_E2E_CORPUS"
work=$(mktemp -d);trap 'rm -rf "$work"' EXIT
mkdir -p "$work/data/subtitle-autosync"
python3 - "$NUVIO_SILERO_ONNX" "$work/data/subtitle-autosync/silero-16k-op15-1e261b036686.onnx" <<'PY'
import hashlib,sys,shutil
assert hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest()=='7ed98ddbad84ccac4cd0aeb3099049280713df825c610a8ed34543318f1b2c49'
shutil.copyfile(sys.argv[1],sys.argv[2])
PY
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -DNUVIO_SILERO_ORT -Isrc -I"$NUVIO_ORT_ROOT/include" \
 tests/audsync_real.c src/audsync.c src/audvad.c src/audsilero.c src/audmodel.c src/sha256.c \
 src/autosync.c src/legenda.c src/assrender.c -ldl -pthread -lm -o "$work/real"
export LD_LIBRARY_PATH="$NUVIO_ORT_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export NUVIO_E2E_DATA="$work/data"
python3 tests/audsync_real.py "$work/real" "$NUVIO_E2E_CORPUS"
