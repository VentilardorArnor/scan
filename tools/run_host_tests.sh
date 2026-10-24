#!/usr/bin/env bash
# Testes de host: compilam e rodam no PC, sem placa e sem ESP-IDF.
# Na primeira vez baixa Unity e cJSON (versões fixadas em test/host/CMakeLists.txt).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cmake -S "$ROOT/test/host" -B "$ROOT/build-host" -G Ninja >/dev/null
cmake --build "$ROOT/build-host"
ctest --test-dir "$ROOT/build-host" --output-on-failure
