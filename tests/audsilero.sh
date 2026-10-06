#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
: "${NUVIO_ORT_ROOT:?external ONNX Runtime installation required}"
: "${NUVIO_SILERO_ONNX:?pinned external original ONNX required}"
: "${NUVIO_SILERO_MODEL:?external native ORT/ONNX model required}"
: "${NUVIO_SILERO_WAV:?external real mono 16 kHz WAV required}"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
${CC:-cc} -std=c11 -Wall -Wextra -Werror -DNUVIO_SILERO_ORT -Isrc \
  -I"$NUVIO_ORT_ROOT/include" tests/audsilero_native.c src/audsilero.c \
  -L"$NUVIO_ORT_ROOT/lib" -Wl,-rpath,"$NUVIO_ORT_ROOT/lib" -ldl -lm -o "$work/native"
${PYTHON:-python3} tests/audsilero_parity.py "$work/native" "$NUVIO_SILERO_ONNX" "$NUVIO_SILERO_MODEL" "$NUVIO_SILERO_WAV"
