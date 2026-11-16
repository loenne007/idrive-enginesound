#!/bin/sh
set -eu
cd "$(dirname "$0")"
make host
${CC:-cc} -std=c99 -O2 -Wall -Wextra test_core.c synth.c engine.c presets.c -lm -o build/test-core
./build/test-core
./render_all.sh
python3 check.py
./build/es-synth-host --bench 60
