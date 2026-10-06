#!/bin/bash
# Cola de sessao do AutoSync (legsync.c) com referencia embutida real.
#   bash tests/legsync.sh    SANITIZE=1 (ASan/UBSan) ou SANITIZE=thread
set -euo pipefail
cd "$(dirname "$0")/.."
T=${TMPDIR:-/tmp}; FX="$T/nv-legref-fx"
bash tests/legref_fixtures.sh "$FX"
D=$(mktemp -d "$T/nv-legsync.XXXXXX"); trap 'rm -rf "$D"' EXIT
flags=()
if [ "${SANITIZE:-0}" = 1 ]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
elif [ "${SANITIZE:-0}" = thread ]; then flags+=(-fsanitize=thread -fno-omit-frame-pointer); fi
# LS_RITMO=0: sem o teto de 8 Ranges/s (o teste le do disco).
cc -DAUDSYNC_LEGACY_DSP "${flags[@]}" -DLS_RITMO=0 -Isrc -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 -O1 -g -Wall -Wextra src/legsync.c src/legsyncui.c src/legref.c src/autosync.c src/audsync.c src/audvad.c src/legenda.c \
  src/assrender.c tests/legsync.c -pthread -lm -o "$D/t"
"$D/t" "$FX"
