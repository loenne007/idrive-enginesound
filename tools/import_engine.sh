#!/bin/sh
# tools/import_engine.sh <id> <git ref>: copy synth/ of an agent's branch into synth/<id>/ and wire it into this
# layout: its car binary as tool/lfb-enginesound/es-synth-<id>, the shared loader / glibc check, the shared speaker
# protection (tools/add_protect.py).
set -eu
id=$1; ref=$2
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
git -C "$root" archive "$ref" synth | tar -x -C "$tmp"
rm -rf "$root/synth/$id"
mkdir -p "$root/synth/$id"
cp -R "$tmp/synth/." "$root/synth/$id/"
rm -rf "$tmp" "$root/synth/$id/common" "$root/synth/$id/build"
cd "$root/synth/$id"
rm -f protect.h   # the shared one in synth/common is used
python3 - "$id" <<'PY'
import re, sys
mk = open('Makefile').read()
mk = re.sub(r'car: build/es-synth\n(\t.*\n)+?(?=\S|\n|$)', '''# the engine's id: this folder's name; the car binary is tool/lfb-enginesound/es-synth-<id>
ENGINE := $(notdir $(CURDIR))
OUT := ../../tool/lfb-enginesound/es-synth-$(ENGINE)
car: build/es-synth
\tcp build/es-synth $(OUT)
\tfile $(OUT)
\tpython3 check_glibc.py $(OUT)
''', mk, count=1)
open('Makefile', 'w').write(mk)
PY
sed -i.bak -e 's# protect\.h# ../common/protect.h#g' -e 's# -I\. -I\.\./common# -I../common#' Makefile && rm -f Makefile.bak
cp ../../tools/check_glibc.py check_glibc.py
python3 ../../tools/add_protect.py .
make car
