#!/usr/bin/env python3
"""Bass engine regression checks, dependency-free (Python 3 standard library only).

Samples: level, peaks, bass-band peak, infrasound, clicks, level through the demo's upshifts. Renders: every main
order's frequency against the rpm (1 %), load -> level and brightness, off-gas against full load (7-10 dB), the X
settings, transitions (preset switching, mute, X changes, shifts, malformed lines, stale watchdog, quit) and that a
gear change (S) changes nothing.
"""
import array, cmath, json, math, pathlib, subprocess, sys, wave

ROOT = pathlib.Path(__file__).resolve().parent
BIN = ROOT / 'build/es-synth-host'
SR = 48000
# the main orders of each preset (checked for frequency against rpm)
MAIN = {'i6': (1.5, 3, 6), 'v8': (3, 4, 6), 'v10': (1.5, 3, 4.5), 'v12': (1.5, 3, 4.5)}


def read(path):
    with wave.open(str(path), 'rb') as w:
        n = w.getnchannels()
        a = array.array('h', w.readframes(w.getnframes()))
        if sys.byteorder != 'little': a.byteswap()
        return [v / 32768 for v in a[::n]]


def fft(x):
    n = len(x)
    a = [complex(v) for v in x]
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j ^= bit
        if i < j: a[i], a[j] = a[j], a[i]
    m = 2
    while m <= n:
        root = cmath.exp(-2j * math.pi / m)
        h = m // 2
        tw = [root ** k for k in range(h)]
        for k in range(0, n, m):
            for j in range(h):
                u = a[k + j]
                v = tw[j] * a[k + j + h]
                a[k + j] = u + v
                a[k + j + h] = u - v
        m *= 2
    return [abs(v) ** 2 for v in a[:n // 2]]


def spectrum(a, t=2, n=32768):
    x = a[int(t * SR):int(t * SR) + n]
    assert len(x) == n
    return fft([v * (.5 - .5 * math.cos(2 * math.pi * i / (n - 1))) for i, v in enumerate(x)])


def energy(s, lo, hi, n=32768): return sum(s[int(lo * n / SR):int(hi * n / SR)])


def peak_freq(s, f, rel=.04, n=32768):
    """the strongest bin within +-rel of f, refined by a parabola on the log magnitude (Hann: < 0.05 bin)"""
    lo, hi = int(f * (1 - rel) * n / SR), int(f * (1 + rel) * n / SR) + 1
    k = max(range(lo, hi), key=s.__getitem__)
    l, c, r = (math.log(s[i] + 1e-30) for i in (k - 1, k, k + 1))
    d = .5 * (l - r) / (l - 2 * c + r) if l - 2 * c + r else 0
    return (k + d) * SR / n, s[k]


def render(p, name, lines, extra=()):
    script = ROOT / 'build' / f'{name}-{p}.txt'
    script.write_text('\n'.join(lines) + '\n')
    path = ROOT / 'build' / f'{name}-{p}.wav'
    subprocess.run([str(BIN), '--out', 'wav:' + str(path), '--preset', p, '--script', str(script), *extra], check=True)
    return read(path)


def level(a): return math.sqrt(sum(x * x for x in a) / len(a))


def db(x): return 20 * math.log10(x + 1e-12)


def clicks(a, what):
    # a sample step far above the surrounding 10 ms waveform slope (the Waveguide engine's detector)
    bad = 0
    worst = 0
    for i in range(480, len(a) - 480, 240):
        dif = [abs(a[k] - a[k - 1]) for k in range(i - 240, i + 240)]
        rms = math.sqrt(sum(x * x for x in dif) / len(dif))
        mx = max(dif)
        ratio = mx / max(rms, .0001)
        worst = max(worst, ratio)
        if mx > .035 and ratio > 14: bad += 1
    assert bad == 0, (what, 'clicks', bad, worst)
    return worst


def envelope(a, half):
    # 10 ms RMS every 5 ms, energy-averaged over (2 half + 1) x 5 ms, in dB
    hop = 240
    e = [sum(x * x for x in a[i:i + 480]) / 480 for i in range(0, len(a) - 480, hop)]
    return [10 * math.log10(sum(e[max(0, k - half):k + half + 1]) / len(e[max(0, k - half):k + half + 1]) + 1e-12)
            for k in range(len(e))]


def biquad(x, b0, b1, b2, a1, a2):
    y = []
    z1 = z2 = 0.
    for v in x:
        o = b0 * v + z1
        z1 = b1 * v - a1 * o + z2
        z2 = b2 * v - a2 * o
        y.append(o)
    return y


def lowpass(x, fc, q):
    w = 2 * math.pi * fc / SR
    c, al = math.cos(w), math.sin(w) / (2 * q)
    a0 = 1 + al
    return biquad(x, (1 - c) / 2 / a0, (1 - c) / a0, (1 - c) / 2 / a0, -2 * c / a0, (1 - al) / a0)


def bass_peak(a):
    # the speaker protection's detector: 4th-order Butterworth low-pass at 100 Hz, peak
    lo = lowpass(lowpass(a, 100, .5411961), 100, 1.3065630)
    return max(map(abs, lo[SR:]))


def steady(rpm, load, thr, gear=3, secs=4, kmh=60):
    return [f'{t / 20} P {rpm} {load} {thr} {gear} {kmh}' for t in range(int(secs * 20) + 1)]


info = json.loads(subprocess.check_output([str(BIN), '--info']))
ids = [p['id'] for p in info['presets']]
assert ids == ['i6', 'v8', 'v10', 'v12'], ids
assert [p['cylinders'] for p in info['presets']] == [6, 8, 6, 6]
assert all(p['idle'] == 800 and p['redline'] == 5000 for p in info['presets'])

# ---- the delivered samples ----
for name in ids:
    a = read(ROOT / 'samples' / f'{name}.wav')
    assert len(a) == 40 * SR
    assert all(math.isfinite(x) for x in a)
    peak = max(map(abs, a))
    rms = level(a)
    assert peak <= 10 ** (-3 / 20), (name, 'peak', db(peak))
    assert -21 <= db(rms) <= -16.5, (name, 'demo RMS', db(rms))
    # D is a bass enhancer that stays inside the speaker protection without leaning on it: the band under 100 Hz
    # (the protection's detector) peaks at -11..-9 dBFS (limit -8), the 3 s RMS stays under -15 dBFS (limit -14)
    bp = bass_peak(a)
    assert -11.5 <= db(bp) <= -9, (name, 'bass band peak: headroom under the protection limit', db(bp))
    cs = [0.]
    for x in a: cs.append(cs[-1] + x * x)
    rms3 = max(cs[i + 3 * SR] - cs[i] for i in range(0, len(a) - 3 * SR, 4800)) / (3 * SR)
    assert db(math.sqrt(rms3)) <= -15, (name, '3 s RMS reaches the power limiter', db(math.sqrt(rms3)))
    # energy split over the drive (13 windows of 0.68 s): mostly 30-250 Hz, nothing under 30 Hz, almost nothing
    # above 250 Hz
    band = [0.] * 5
    for t in range(1, 39, 3):
        s = spectrum(a, t)
        for j, (lo, hi) in enumerate(((1, 30), (30, 60), (60, 120), (120, 250), (250, 20000))):
            band[j] += energy(s, lo, hi)
    tot = sum(band)
    band = [100 * b / tot for b in band]
    assert band[0] < .03 and band[4] < 1.5 and band[1] + band[2] > 60, (name, 'band split', band)
    sub = band[0] / 100
    ratio = clicks(a, name)
    # demo upshifts (rpm falls ~1200-1450 over 350 ms): no gap or onset beyond the settled levels around them
    for ts in (4.6, 7.4, 10.6, 21.5, 29.5):
        e = envelope(a[int((ts - .5) * SR):int((ts + 1) * SR)], 8)
        k = lambda t: int((t + .5) * 200)
        pre = sorted(e[k(-.35):k(-.05)])[len(e[k(-.35):k(-.05)]) // 2]
        post = sorted(e[k(.5):k(.9)])[len(e[k(.5):k(.9)]) // 2]
        w = e[k(-.05):k(.5)]
        assert min(w) >= min(pre, post) - 3 and max(w) <= max(pre, post) + 3, (name, ts, 'shift gap/onset')
    print(f'{name}: demo RMS {db(rms):.1f} dBFS, peak {db(peak):.1f} dBFS, <100 Hz peak {db(bp):.1f} dBFS, '
          f'3 s RMS max {db(math.sqrt(rms3)):.1f} dBFS, <30 Hz {10 * math.log10(sub + 1e-15):.0f} dB, energy '
          f'30-60/60-120/120-250/>250 Hz ' + '/'.join(f'{b:.1f}' for b in band[1:]) + f' %, slope ratio {ratio:.1f}')

# ---- idle 800 rpm: order 1.5 is 20 Hz, order 1 13 Hz: nothing under 30 Hz; X bass 2 against the protection ----
for name in ids:
    s = spectrum(render(name, 'idle', steady(800, .05, 0, 1, 4, 0)), 2)
    sub = 10 * math.log10(energy(s, 1, 30) / energy(s, 1, 20000))
    assert sub < -40, (name, 'idle: content under 30 Hz', sub)
    b2 = bass_peak(render(name, 'bass2', ['0 X bass 2'] + steady(2000, 1, 1, 4, 4, 60)))
    b1 = bass_peak(render(name, 'bass1', steady(2000, 1, 1, 4, 4, 60)))
    print(f'{name}: idle under 30 Hz {sub:.0f} dB; 2000 rpm full load <100 Hz peak {db(b1):.1f} dBFS, with X bass 2 '
          f'{db(b2):.1f} dBFS (protection limit -8: {"limiting" if db(b2) > -8.6 else "not reached"})')

# ---- orders track the rpm, load, off-gas ----
for name in ids:
    errs = []
    for rpm in (1500, 2600, 4000):
        s = spectrum(render(name, f'steady{rpm}', steady(rpm, .7, .7)))
        top = max(s[int(25 * 32768 / SR):int(1200 * 32768 / SR)])
        for q in MAIN[name]:
            f = q * rpm / 60
            got, mag = peak_freq(s, f)
            assert mag > top * 10 ** (-4.5), (name, rpm, q, 'order missing', 10 * math.log10(mag / top))
            assert abs(got / f - 1) < .01, (name, rpm, q, got, f)
            errs.append(abs(got / f - 1))
    lv = {}
    br = {}
    for load in (1, .5, .05, 0, -.5):
        a = render(name, f'load{load}', steady(2500, load, max(load, 0)))[SR:]
        lv[load] = db(level(a))
        ss = [spectrum(a, t) for t in (.2, .9, 1.6, 2.2)]
        br[load] = sum(energy(s, 120, 400) for s in ss) / sum(energy(s, 30, 400) for s in ss)
    assert lv[1] > lv[.5] > lv[.05], (name, 'level does not rise with load', lv)
    assert br[1] > br[.05] * 1.1, (name, 'no brightness increase', br)  # mild: D stays a bass layer
    for off in (0, -.5):
        assert 3 <= lv[1] - lv[off] <= 5, (name, 'off-gas level', off, lv[1] - lv[off])
    print(f'{name}: orders {MAIN[name]} at 1500/2600/4000 rpm, worst frequency error {100 * max(errs):.3f} %; '
          f'2500 rpm level load 1/.5/.05/0/-0.5: ' + '/'.join(f'{lv[k]:.1f}' for k in (1, .5, .05, 0, -.5)) +
          f' dBFS (off-gas {lv[1] - lv[0]:.1f} / coast {lv[1] - lv[-.5]:.1f} dB under full load), '
          f'120-400 Hz share {100 * br[.05]:.1f} -> {100 * br[1]:.1f} %')

# ---- X settings ----
def lift_drive(xs, turbo=False):
    # full load at 3300 rpm, a lift at 2.5 s, coasting at the same rpm (clean order lines); X lines at t = 0
    lines = (['0 T 1'] if turbo else []) + [f'0 X {k} {v}' for k, v in xs]
    for i in range(101):
        t = i / 20
        if t < 2.5: lines.append(f'{t} P 3300 0.95 1 3 70')
        else: lines.append(f'{t} P 3300 -0.6 0 3 70')
    return lines


def non_tonal(s, rpm, lo, hi, n=32768, width=4):
    # energy between lo and hi Hz away from every half-order line of the bank (+-4 Hz)
    tot = 0
    for k in range(int(lo * n / SR), int(hi * n / SR)):
        f = k * SR / n
        q = f / (rpm / 120)
        if q > 25.5 or abs(q - round(q)) * rpm / 120 > width: tot += s[k]  # the bank ends at order 12
    return tot


# idle, cruise and coasting, every layer at its maximum (T 1, X 2): nothing above 1.5 kHz (>= 35 dB under the
# total) and no layer active (non-tonal energy above 500 Hz >= 50 dB under the total)
for name in ids:
    out = []
    for what, rpm, load, thr, g in (('idle', 800, .05, 0, 1), ('cruise', 2000, .2, .2, 6), ('coast', 2500, -.5, 0, 5)):
        a = render(name, f'quiet-{what}', ['0 T 1', '0 X turbo 2', '0 X bov 2', '0 X pops 2'] + steady(rpm, load, thr, g))
        s = spectrum(a, 2)
        tot = energy(s, 1, 20000)
        hi = 10 * math.log10(energy(s, 1500, 20000) / tot)
        nt = 10 * math.log10(non_tonal(s, rpm, 500, 20000) / tot)
        assert hi <= -35, (name, what, 'energy above 1.5 kHz', hi)
        assert nt <= -50, (name, what, 'a noise layer is active', nt)
        out.append(f'{what} >1.5 kHz {hi:.0f} dB, non-tonal >500 Hz {nt:.0f} dB')
    print(f'{name}: ' + '; '.join(out))

for name in ('v10', 'i6'):
    s_def = spectrum(render(name, 'x-default', lift_drive([])), 1.5)
    s_on = spectrum(render(name, 'x-turbo1', lift_drive([], True)), 1.5)
    s_zero = spectrum(render(name, 'x-turbo0', lift_drive([('turbo', 0)], True)), 1.5)
    t_def, t_on, t_zero = energy(s_def, 2000, 7000), energy(s_on, 2000, 7000), energy(s_zero, 2000, 7000)
    assert t_on > 1000 * t_zero and t_def < 2 * t_zero, (name, 'turbo whistle: T 1 on, default / X turbo 0 off', t_def, t_on, t_zero)
    # sigh: non-tonal 300-1500 Hz in the 0.68 s after the lift; pops (X pops 2): non-tonal 60-700 Hz
    def after(xs, n):
        return spectrum(render(name, 'x-' + '-'.join(f'{k}{v}' for k, v in xs), lift_drive(xs)), 2.5, n)
    # the sigh: the 170 ms after the lift; pops: the 0.68 s after it
    b_none = non_tonal(after([('bov', 0), ('pops', 0)], 8192), 3300, 300, 1500, 8192, 12)
    b_bov = non_tonal(after([('pops', 0)], 8192), 3300, 300, 1500, 8192, 12)
    p_none = non_tonal(after([('bov', 0), ('pops', 0)], 32768), 3300, 60, 700)
    p_pop = non_tonal(after([('bov', 0), ('pops', 2)], 32768), 3300, 60, 700)
    assert b_bov > 1.6 * b_none and p_pop > 1.15 * p_none, (name, 'bov / pops missing', b_none, b_bov, p_none, p_pop)
    lo = {}
    for bass in (0, 1, 2):
        s = spectrum(render(name, f'x-bass{bass}', [f'0 X bass {bass}'] + steady(2000, .5, .5)), 2)
        lo[bass] = 10 * math.log10(energy(s, 30, 80) / energy(s, 150, 400))
    assert lo[2] - lo[1] > 3 and lo[1] - lo[0] > 3, (name, 'bass', lo)
    og = {}
    for v in (.5, 1, 2):
        og[v] = (db(level(render(name, f'x-off{v}', [f'0 X offgas {v}'] + steady(2500, 0, 0))[SR:])),
                 db(level(render(name, f'x-offfull{v}', [f'0 X offgas {v}'] + steady(2500, 1, 1))[SR:])))
    assert 4 < og[1][0] - og[.5][0] < 8 and abs(og[1][1] - og[.5][1]) < .5, (name, 'offgas', og)
    assert abs(og[2][0] - og[2][1]) < 1 and abs(og[1][1] - og[2][1]) < .5, (name, 'offgas 2 is not as loud as full load', og)
    print(f'{name}: whistle 2-7 kHz T 1 vs default {10 * math.log10(t_on / t_def):.0f} dB, X turbo 0 '
          f'{10 * math.log10(t_zero / t_on):.0f} dB; after the lift non-tonal: sigh {10 * math.log10(b_bov / b_none):+.1f} dB '
          f'(0.3-1.5 kHz), pops 2 {10 * math.log10(p_pop / p_none):+.1f} dB (60-700 Hz); bass 0/1/2 low-vs-mid {lo[0]:.1f}/{lo[1]:.1f}/{lo[2]:.1f} dB; '
          f'offgas 0.5: off-gas {og[.5][0] - og[1][0]:+.1f} dB, full load {og[.5][1] - og[1][1]:+.1f} dB; '
          f'offgas 2: off-gas {og[2][0] - og[2][1]:+.1f} dB re full load')

# ---- lift-off glide and tip-in: a lift glides down over ~0.5 s (100 ms after it within 2 dB of before), a tip-in
# is back within 1.5 dB of the full-load level within 100 ms (measured with a 45 ms window). Constant 2500 rpm, 50 Hz telemetry like the car.
for name in ids:
    lines = []
    for i in range(141):
        t = i / 20
        l, th = (1, 1) if t < 2 or t >= 5 else (-.5, 0)
        lines.append(f'{t} P 2500 {l} {th} 4 80')
    a = render(name, 'glide', lines)
    e = envelope(a, 4)  # 45 ms windows every 5 ms; e[k] at k * 5 ms
    at = lambda t: e[int(t * 200)]
    seg = lambda t0, t1: db(level(a[int(t0 * SR):int(t1 * SR)]))
    before, after, settled = seg(1, 1.95), seg(3.5, 4.95), seg(6, 6.95)
    drop100, drop300 = before - seg(2.08, 2.12), before - seg(2.28, 2.32)
    tip = next(k for k in range(int(5 * 200), int(6 * 200)) if e[k] >= settled - 1.5) / 200 - 5
    assert drop100 <= 2, (name, 'lift-off drops too fast', drop100)
    assert 3 <= before - after <= 5, (name, 'lift-off end level', before - after)
    assert tip <= .1, (name, 'tip-in too slow', tip)  # incl. the 45 ms envelope window
    print(f'{name}: lift-off glide {-drop100:+.1f} dB after 100 ms, {-drop300:+.1f} dB after 300 ms, settles '
          f'{after - before:+.1f} dB; tip-in within 1.5 dB after {1000 * tip:.0f} ms')

# ---- transitions: preset switching, mute, X changes, shifts, malformed lines, stale watchdog, quit ----
lines = []
for i in range(160):
    t = i / 20
    lines.append(f'{t} P {1200 if t < 3 else 4200} {.6 if t < 6 else -.6} {.7 if t < 6 else 0} 3 60')
    if i in (20, 21, 22, 23): lines.append(f'{t} E ' + ['v8', 'v10', 'v12', 'i6'][i - 20])
    if i == 30: lines.append(f'{t} X bass 2')
    if i == 32: lines.append(f'{t} X offgas 0')
    if i == 34: lines.append(f'{t} X turbo 2')
    if i == 36: lines.append(f'{t} X pops 2')
    if i == 38: lines.append(f'{t} X bass 0')
    if i == 40: lines.append(f'{t} S 3 4')
    if i == 70: lines.append(f'{t} S 4 3')
    if i == 90: lines.append(f'{t} M 1')
    if i == 110: lines.append(f'{t} M 0')
    if i == 125: lines.append(f'{t} X offgas 2')
lines += ['8 P nan inf nan 3 50', '8 E invalid', '8 V nan', '8 X bass nan', '11 V 0.8', '12 Q']
a = render('v10', 'transitions', lines)
clicks(a, 'transitions')
assert level(a[int(10.5 * SR):int(11.5 * SR)]) < .0001, 'watchdog failed'
assert level(a[int(5.1 * SR):int(5.4 * SR)]) < .002, 'mute failed'
assert level(a[-4800:]) < .0001, 'quit fade failed'

# ---- gear changes trigger nothing: the same drive with and without S lines ----
def drive(kind, shift):
    lines = []
    for i in range(81):
        t = i / 20
        if kind == 'up':
            k = min(1, max(0, (t - 2) / .35))
            k = k * k * (3 - 2 * k)
            r, l, th, g0, g1 = 4400 + (3100 - 4400) * k, .9 - .15 * math.sin(math.pi * k), .95, 3, 4
        elif kind == 'coast-up':
            k = min(1, max(0, (t - 2) / .3))
            k = k * k * (3 - 2 * k)
            r, l, th, g0, g1 = 3600 + (2900 - 3600) * k, -.55, 0, 4, 5
        else:
            k = min(1, max(0, (t - 2) / .3))
            k = k * k * (3 - 2 * k)
            r, l, th, g0, g1 = 2000 + 500 * k, -.5, 0, 5, 4
        lines.append(f'{t} P {r:.1f} {l:.3f} {th} {g1 if t >= 2 else g0} 60')
        if shift and i == 40: lines.append(f'{t} S {g0} {g1}')
    return lines

worst = 0
for p in ids:
    for kind in ('up', 'coast-up', 'down'):
        a = envelope(render(p, f'shift-{kind}', drive(kind, True)), 5)
        b = envelope(render(p, f'shift-{kind}-ref', drive(kind, False)), 5)
        w = [abs(a[k] - b[k]) for k in range(int(1.9 * 200), int(3 * 200))]
        worst = max(worst, max(w))
        assert max(w) <= .5, (p, kind, 'a gear change changed the sound', max(w))
print(f'transitions: no clicks, mute, watchdog, quit; gear changes: max envelope change {worst:.2f} dB (limit 0.5)')
print('PASS: levels, bass, infrasound, order tracking, load / brightness, off-gas, X settings, transitions, shifts')
