#!/bin/sh
# the four 40 s demo drives (real B47 rpm), mono 16-bit WAV under 4 MB each
set -eu
cd "$(dirname "$0")"
make host
mkdir -p samples
for p in i6 v8 v10 v12; do
 ./build/es-synth-host --out "wav:build/$p-stereo.wav" --demo 40 --preset "$p"
 python3 - "$p" <<'PY'
import sys,wave,array
p=sys.argv[1]
with wave.open('build/'+p+'-stereo.wav','rb') as w:
 a=array.array('h',w.readframes(w.getnframes()))
 if sys.byteorder!='little':a.byteswap()
 a=array.array('h',a[::2])
 if sys.byteorder!='little':a.byteswap()
 with wave.open('samples/'+p+'.wav','wb') as o:
  o.setparams((1,2,48000,0,'NONE','not compressed'));o.writeframes(a.tobytes())
PY
done
