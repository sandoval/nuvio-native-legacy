#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
cc -Isrc -DAUDMODEL_TEST -DNUVIO_SILERO_ORT -Wall -Wextra -Werror src/audmodel.c src/sha256.c tests/audmodel.c -pthread -Wl,--wrap=pthread_create -o "$D/test"
"$D/test"
