#!/usr/bin/env python3
"""tools/add_protect.py synth/<id> ...: put the shared speaker protection (synth/common/protect.h) into an engine's
main.c as the last stage before every output (ALSA, WAV, raw), and its Makefile's include path. Idempotent; for
engines whose main.c follows the shared shape (synth_render(&s, buf, n) then the output branches)."""
import re, sys, pathlib
for d in map(pathlib.Path, sys.argv[1:]):
    m = (d / 'main.c').read_text()
    if 'protect.h' not in m:
        m = m.replace('#include "synth.h"\n', '#include "synth.h"\n#include "protect.h" /* synth/common: speaker protection */\n', 1)
        m, n1 = re.subn(r'(\n(\s*)Alsa alsa = \{0\};\n)', r'\1\2Protect prot;\n', m, count=1)
        m, n2 = re.subn(r'(\n(\s*)synth_init\(&s, p, volume\);\n(\s*)signal\(SIGINT)', r'\n\2protect_init(&prot);\n\2synth_init(&s, p, volume);\n\3signal(SIGINT', m, count=1)
        m, n3 = re.subn(r'(\n(\s*)synth_render\(&s, buf, n\);\n)', r'\1\2/* (the tone too) */\n', m, count=1)
        # after the render / tone branch, before the output: one protect_run for every output
        m, n4 = re.subn(r'(\n(\s*)if \(fp\) \{\n(\s*)if \(fwrite\(buf)', r'\n\2protect_run(&prot, buf, n); /* speaker protection, last stage */\1', m, count=1)
        assert n1 == n2 == n4 == 1, (d, n1, n2, n3, n4)
        (d / 'main.c').write_text(m)
    mk = (d / 'Makefile').read_text()
    if '-I../common' not in mk:
        mk = re.sub(r'^(FLAGS = .*)$', r'\1 -I../common', mk, count=1, flags=re.M)
        mk = mk.replace('out_alsa.h\n', 'out_alsa.h ../common/protect.h\n')
        (d / 'Makefile').write_text(mk)
    print(d, 'ok')
