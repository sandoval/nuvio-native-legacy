#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
cc -Isrc -Wall -Wextra -Werror src/audsource.c tests/audsource.c -pthread -lm -o "$D/test"
"$D/test"
