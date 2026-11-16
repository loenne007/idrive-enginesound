#!/bin/sh
# tools/import_engine.sh <id> <git ref> [<path>]: copy <path> (default synth) of a branch into synth/<id>/ and wire it
# into this layout: its car binary as tool/lfb-enginesound/es-synth-<id>, the shared loader / glibc check
# (tools/check_glibc.py), the shared speaker protection (tools/add_protect.py). <id>: a lower-case name (a-z, 0-9, _).
# Then: its binary in tool/lfb-enginesound/component.json "files", its entry in SOUND_ENGINES (apps/lfb_enginesound/core.js)
# and in the soundEngine setting (app.json).
set -eu
id=$1; ref=$2; src=${3:-synth}
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
git -C "$root" archive "$ref" "$src" | tar -x -C "$tmp"
rm -rf "$root/synth/$id"
mkdir -p "$root/synth/$id"
cp -R "$tmp/$src/." "$root/synth/$id/"
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
\tpython3 ../../tools/check_glibc.py $(OUT)
''', mk, count=1)
open('Makefile', 'w').write(mk)
PY
sed -i.bak -e 's# protect\.h# ../common/protect.h#g' -e 's# -I\. -I\.\./common# -I../common#' Makefile && rm -f Makefile.bak
rm -f check_glibc.py   # the shared one in tools/ is used
python3 ../../tools/add_protect.py .
make car
