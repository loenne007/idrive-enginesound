#!/usr/bin/env python3
"""mono.py left|mix <stereo.wav> <mono.wav>: a 16-bit stereo render as a mono WAV: its left channel, or (L + R) / 2
(Replica: its stereo widener's side signal cancels)."""
import array, sys, wave

mode, src, dst = sys.argv[1:4]
with wave.open(src, 'rb') as w:
    rate = w.getframerate()
    a = array.array('h', w.readframes(w.getnframes()))
if sys.byteorder != 'little': a.byteswap()
a = array.array('h', [(x + y) // 2 for x, y in zip(a[::2], a[1::2])] if mode == 'mix' else a[::2])
if sys.byteorder != 'little': a.byteswap()
with wave.open(dst, 'wb') as o:
    o.setparams((1, 2, rate, 0, 'NONE', 'not compressed'))
    o.writeframes(a.tobytes())
