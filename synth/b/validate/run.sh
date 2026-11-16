#!/bin/sh
# usage: ENGINE_SIM=/path/to/Engine_Sim sh run.sh [js] presets...  (tag "v12t" = v12 with the turbo forced on)
# Renders the validation scenario with the C port, dumping its per-frame params (ES_DUMP_PARAMS); with "js" the
# same params are replayed through the real Engine_Sim graph in headless Chrome. Then: python3 compare.py tags --png x.png
set -e
cd "$(dirname "$0")"
B=$(pwd)/../build/es-synth-host
B=${SYNTH_BIN:-$B}
JS=0
if [ "$1" = js ]; then JS=1; shift; fi
ARGS=""
for p in "$@"; do
  T=""
  case $p in *t) T=1 ;; esac
  n=${p%t}
  python3 scenario.py "$n" "sc_$p.txt" $T
  ES_VALIDATE=1 ES_DUMP_PARAMS="dump_$p.txt" "$B" --out "wav:c_$p.wav" --script "sc_$p.txt" --preset "$n" --volume 0.8
  ARGS="$ARGS js_$p.wav 30 0.8 dump_$p.txt"
done
if [ $JS = 1 ]; then node replay.mjs $ARGS; fi
