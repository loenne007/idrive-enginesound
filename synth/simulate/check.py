#!/usr/bin/env python3
"""Pulse engine: objective regression checks (python3 stdlib only). Renders through build/es-synth-host --script.

Levels are dB of RMS; envelopes are 10 ms RMS every 5 ms, energy-averaged. FFTs use Hann windows. These are
objective regressions, not a substitute for listening."""
import array, cmath, json, math, os, pathlib, subprocess, sys, wave
ROOT = pathlib.Path(__file__).resolve().parent
BIN = ROOT / 'build/es-synth-host'
B = ROOT / 'build'
SR = 48000
fails = []


def check(cond, msg):
    if not cond:
        fails.append(msg)
        print('FAIL: ' + msg)


def read(path):
    with wave.open(str(path), 'rb') as w:
        n = w.getnchannels(); a = array.array('h', w.readframes(w.getnframes()))
    if sys.byteorder != 'little': a.byteswap()
    return [v / 32768 for v in a[::n]]


def render(p, name, lines, env=None):
    script = B / f'{name}-{p}.txt'; script.write_text('\n'.join(lines) + '\n')
    path = B / f'{name}-{p}.wav'
    e = dict(os.environ); e.update(env or {})
    subprocess.run([str(BIN), '--out', 'wav:' + str(path), '--preset', p, '--script', str(script)], check=True, env=e)
    return read(path)


def fft(x):
    n = len(x); a = [complex(v) for v in x]; j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit: j ^= bit; bit >>= 1
        j ^= bit
        if i < j: a[i], a[j] = a[j], a[i]
    m = 2
    while m <= n:
        root = cmath.exp(-2j * math.pi / m); h = m // 2
        tw = [1]
        for _ in range(h - 1): tw.append(tw[-1] * root)
        for k in range(0, n, m):
            for j in range(h):
                u = a[k + j]; v = tw[j] * a[k + j + h]; a[k + j] = u + v; a[k + j + h] = u - v
        m *= 2
    return [abs(v) ** 2 for v in a[:n // 2]]


N = 32768


def spectrum(a, t):
    x = a[int(t * SR):int(t * SR) + N]; assert len(x) == N
    return fft([v * (.5 - .5 * math.cos(2 * math.pi * i / (N - 1))) for i, v in enumerate(x)])


def energy(s, lo, hi): return sum(s[int(lo * N / SR):int(hi * N / SR)]) + 1e-30
def rms(a): return math.sqrt(sum(x * x for x in a) / len(a)) + 1e-12
def db(x): return 20 * math.log10(x)
def lvl(a, t0, t1): return db(rms(a[int(t0 * SR):int(t1 * SR)]))


def clicks(a):
    """sample-slope outliers against the surrounding 10 ms slope RMS"""
    bad = 0; worst = 0
    for i in range(480, len(a) - 480, 240):
        dif = [abs(a[k] - a[k - 1]) for k in range(i - 240, i + 240)]
        r = math.sqrt(sum(x * x for x in dif) / len(dif)); mx = max(dif); ratio = mx / max(r, .0001)
        worst = max(worst, ratio)
        if mx > .035 and ratio > 14: bad += 1
    return bad, worst


def envelope(a, half):
    hop = 240; e = [sum(x * x for x in a[i:i + 480]) / 480 for i in range(0, len(a) - 480, hop)]
    return [10 * math.log10(sum(e[max(0, k - half):k + half + 1]) / len(e[max(0, k - half):k + half + 1]) + 1e-12)
            for k in range(len(e))]


def steady(rpm, load, thr, secs=2.0, gear=3):
    return [f'{t / 50:.2f} P {rpm} {load} {thr} {gear} 60' for t in range(int(secs * 50) + 1)]


def window_ok(e, ts, lim, pre=(-.35, -.05), post=(.6, .9), span=(-.05, .6)):
    """envelope e sampled every 5 ms starting at ts-0.5 s: within lim dB of the settled levels around ts"""
    k = lambda t: int((t + .5) * 200)
    med = lambda v: sorted(v)[len(v) // 2]
    a = med(e[k(pre[0]):k(pre[1])]); b = med(e[k(post[0]):k(post[1])]); w = e[k(span[0]):k(span[1])]
    return min(w) - min(a, b), max(w) - max(a, b)


info = json.loads(subprocess.check_output([str(BIN), '--info']))
want = {'i6': (700, 7000), 'v8': (650, 7200), 'v10': (900, 8250), 'v12': (700, 7500)}
check({p['id']: (p['idle'], p['redline']) for p in info['presets']} == want, '--info idle/redline numbers')

# ---- demo renders: safety, clicks, shift continuity -------------------------------------------------------------
for p in info['presets']:
    name = p['id']; a = read(ROOT / 'samples' / f'{name}.wav')
    check(all(math.isfinite(x) for x in a), f'{name}: non-finite samples')
    peak = max(map(abs, a)); check(peak <= 10 ** (-3 / 20), f'{name}: peak {db(peak):.1f} dBFS > -3')
    r = db(rms(a)); check(-24 < r < -16, f'{name}: demo RMS {r:.1f} dBFS')
    bad, worst = clicks(a); check(bad == 0, f'{name}: {bad} clicks (worst slope ratio {worst:.1f})')
    worst_shift = 0
    for ts in (8.6, 10.8, 13.6, 17.0, 27.5, 29.5, 33.2):  # the demo's upshifts (full load, light load, after kickdown)
        # ~165 ms: the demo cannot separate a shift from the V8's roughness and its exhaust resonance near 6300 rpm
        # (the strict test, the same drive with and without S, is further below)
        e = envelope(a[int((ts - .5) * SR):int((ts + 1) * SR)], 16)
        lo, hi = window_ok(e, .5, 3)
        worst_shift = max(worst_shift, -lo, hi)
        # 3.5 dB (the Waveguide engine's demo limit): the V8's own roughness alone moves this 100 ms envelope by ~3 dB; the
        # strict 3 dB test compares the same drive with and without S below
        check(lo >= -3.5 and hi <= 3.5, f'{name}: demo upshift at {ts}s gap/onset {lo:.1f}/{hi:.1f} dB')
    print(f'{name}: demo RMS {r:.1f} dBFS, peak {db(peak):.1f} dBFS, slope ratio {worst:.1f}, '
          f'upshift envelope within {worst_shift:.1f} dB of settled levels')

# ---- steady states: firing pitch, half orders, brightness with load, off-gas level -------------------------------
for p in info['presets']:
    name = p['id']; n = p['cylinders']; pitches = []
    for rpm in (1800, 3600):
        a = render(name, f'steady{rpm}', steady(rpm, .7, .8)); s = spectrum(a, 1.2)
        exp = rpm / 60 * n / 2; lo, hi = int(exp * .55 * N / SR), int(exp * 1.45 * N / SR)
        k = max(range(lo, hi), key=s.__getitem__); got = k * SR / N
        check(abs(got / exp - 1) < .05, f'{name}: firing peak {got:.1f} Hz at {rpm} rpm, expected {exp:.1f}')
        pitches.append(round(got, 1))
        if rpm == 3600:
            cyc = [sum(s[round(rpm / 120 * h * N / SR) - 1:round(rpm / 120 * h * N / SR) + 2]) for h in range(1, 3 * n)]
            nonfiring = sum(v for h, v in enumerate(cyc, 1) if h % n) / sum(cyc)
            if name == 'v8': check(nonfiring > .01, 'v8: cross-plane bank character lost')
            if name == 'v10': check(nonfiring > .01, 'v10: 90/54 degree character lost')
            # (the orders below ~120 Hz are the low foundation's 0.5/1/1.5/2: judged from 150 Hz up)
            cyc_hi = [v for h, v in enumerate(cyc, 1) if rpm / 120 * h >= 150]
            nf_hi = sum(v for h, v in enumerate(cyc, 1) if h % n and rpm / 120 * h >= 150) / sum(cyc_hi)
            if name == 'i6': check(nf_hi < .03, f'i6: inline six lost its smoothness ({100 * nf_hi:.1f} % non-firing orders above 150 Hz)')
    sh = []
    for load in (.12, .95):
        a = render(name, f'load{load}', steady(3000, load, load)); s = spectrum(a, 1.2)
        sh.append(energy(s, 1000, 6000) / energy(s, 60, 6000))
    check(sh[1] > sh[0] * 1.15, f'{name}: no brightening with load {sh}')
    # 3 s states, the last 2 s measured (a V8 wanders +-1 dB over a second)
    a = render(name, 'offgas', steady(3000, .95, 1, 3) +
               [f'{3 + t / 50:.2f} P 3000 -0.6 0 3 60' for t in range(151)] +
               [f'{6 + t / 50:.2f} P 3000 0.12 0.12 3 60' for t in range(151)] +
               [f'{9 + t / 50:.2f} P {p["idle"]} 0.05 0 1 0' for t in range(151)] +
               [f'{12 + t / 50:.2f} P 2500 0.95 1 3 50' for t in range(151)])
    full, coast, light, idle, f25 = (lvl(a, 1, 3), lvl(a, 4, 6), lvl(a, 7, 9), lvl(a, 10, 12), lvl(a, 13, 15))
    check(3 <= full - coast <= 5, f'{name}: coast {full - coast:.1f} dB under full load (want 3-5)')
    check(3 <= full - light <= 5, f'{name}: light load {full - light:.1f} dB under full load (want 3-5)')
    # a lift-off from full load at 4000 rpm (rpm then falling 300 rpm/s): the level settles smoothly and
    # monotonically (100 ms RMS every 10 ms: never more than 1 dB under the settled level, no rise of more than
    # 1 dB on the way down, 90 % of the drop within 300 ms); the timbre goes first (the centroid has made >= 60 %
    # of its fall by 150 ms)
    # sixteen lifts in one render, each after a different load duration (so each lands at another point of the
    # random stream), envelopes (100 ms RMS every 10 ms) and centroids averaged at the lift: the transition, not
    # the engine's own cycle-to-cycle randomness
    lift = ['0 X pops 0', '0 X bov 0']  # the engine itself (pops and the whoosh are checked below)
    T0 = []; t0 = 0
    for n8 in range(16):
        dur = 1.6 + .071 * n8
        lift += [f'{t0 + t / 50:.2f} P 4000 .95 1 3 60' for t in range(int(dur * 50))]
        lift += [f'{t0 + dur + t / 50:.2f} P {4000 - 300 * t / 50:.0f} -0.6 0 3 60' for t in range(60)]
        T0.append(t0 + dur); t0 += dur + 1.2
    b = render(name, 'lift', lift)
    env = [10 * math.log10(sum(10 ** (lvl(b, L0 + k / 100 - .05, L0 + k / 100 + .05) / 10) for L0 in T0) / 16)
           for k in range(-40, 100)]  # t = -0.4 .. 0.99 s
    at = lambda t: env[int(round(t * 100)) + 40]
    pre = sum(env[:35]) / 35; settled = sum(at(t / 100) for t in range(50, 90)) / 40
    way = [at(t / 100) for t in range(0, 40)]
    dip = settled - min(way); rise = max(way[k] - min(way[:k + 1]) for k in range(len(way)))
    t90 = next((t / 100 for t in range(0, 99) if at(t / 100) <= pre - .9 * (pre - settled)), 1)
    def spec(t):
        x = b[int(t * SR) - 2048:int(t * SR) + 2048]
        s = fft([v * (.5 - .5 * math.cos(2 * math.pi * i / 4095)) for i, v in enumerate(x)])
        return sum(i * SR / 4096 * v for i, v in enumerate(s)) / sum(s), 10 * math.log10(sum(s[86:]) / sum(s))
    def avg(ts):
        r = [spec(t) for t in ts]; return sum(x[0] for x in r) / len(r), sum(x[1] for x in r) / len(r)
    (c0, h0), (c1, h1) = avg([L0 - t for L0 in T0 for t in (.1, .25)]), avg([L0 + t for L0 in T0 for t in (.5, .8)])
    c15, h15 = avg([L0 + .15 for L0 in T0])
    # the edge and rasp: the share of energy above 1 kHz (the centroid alone is the firing fundamental falling
    # with the rpm)
    cfall = (h0 - h15) / (h0 - h1) if h0 - h1 > 3 else 1
    bad, _ = clicks(b[int((T0[0] - .5) * SR):int((T0[0] + 1) * SR)]); check(bad == 0, f'{name}: clicks at the lift-off')
    # tip-in from the overrun back to full load: 90 % of the level within ~50-80 ms
    tip = steady(3000, -.6, 0, 1) + [f'{1 + t / 50:.2f} P 3000 .95 1 3 60' for t in range(51)]
    tb_ = render(name, 'tipin', ['0 X pops 0'] + tip)
    tl0, tl1 = lvl(tb_, .5, .95), lvl(tb_, 1.5, 1.95)
    tin = next((k / 1000 for k in range(0, 500, 5) if lvl(tb_, 1 + k / 1000 - .02, 1 + k / 1000 + .02) >= tl0 + .9 * (tl1 - tl0)), 1)
    check(tin <= .1, f'{name}: tip-in reaches 90 % only after {tin * 1000:.0f} ms')
    check(dip <= 1, f'{name}: lift-off dips {dip:.1f} dB under the settled level')
    check(rise <= 1, f'{name}: lift-off level not monotonic (rises {rise:.1f} dB on the way down)')
    check(t90 <= .3, f'{name}: lift-off settles (90 %) only after {t90 * 1000:.0f} ms')
    check(cfall >= .6, f'{name}: the >1 kHz share made only {100 * cfall:.0f} % of its fall 150 ms after the lift')
    glide = f'lift: {pre - settled:.1f} dB in {t90 * 1000:.0f} ms (dip {max(dip, 0):.1f}, rise {rise:.1f} dB), centroid {c0:.0f} -> {c1:.0f} Hz, >1 kHz share {h0:.0f} -> {h1:.0f} dB ({100 * cfall:.0f} % of it at 150 ms); tip-in 90 % in {tin * 1000:.0f} ms'
    # cruise: no hiss, the band above 3 kHz at least 30 dB under the total
    cr = render(name, 'cruise', steady(3000, .2, .2)); s = spectrum(cr, 1.2)
    hf = 10 * math.log10(energy(s, 3000, 23900) / energy(s, 1, 23900))
    check(hf <= -30, f'{name}: >3 kHz at cruise only {hf:.1f} dB under the total')
    print(f'{name}: firing Hz {pitches}, 1-6 kHz share {sh[0]:.3f} -> {sh[1]:.3f} with load; at 3000 rpm coast '
          f'{full - coast:.1f} dB and light load {full - light:.1f} dB under full load; {glide}; idle {f25 - idle:.1f} dB under full load at 2500; >3 kHz at cruise {hf:.1f} dB')

# ---- low foundation and loudness against rpm (raw output at --volume 0.9, 3 s steady states, the last 2 s: a V8's
# overrun alone wanders +-1 dB over a second) -----
def biquad_lp(x, fc, q):
    w = 2 * math.pi * fc / SR; c = math.cos(w); al = math.sin(w) / (2 * q); a0 = 1 + al
    b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; a1 = -2 * c / a0; a2 = (1 - al) / a0
    y = []; z1 = z2 = 0.0
    for v in x:
        o = b0 * v + z1; z1 = b1 * v - a1 * o + z2; z2 = b0 * v - a2 * o; y.append(o)
    return y
def low_band(x):  # 8th-order Butterworth at 112 Hz: ~ "below 120 Hz"
    for q in (.5098, .6013, .9, 2.5629): x = biquad_lp(x, 112, q)
    return x
def render_v(p, name, lines):
    script = B / f'{name}-{p}.txt'; script.write_text('\n'.join(lines) + '\n'); path = B / f'{name}-{p}.wav'
    subprocess.run([str(BIN), '--out', 'wav:' + str(path), '--preset', p, '--volume', '0.9', '--script', str(script)], check=True)
    return read(path)
for p in info['presets']:
    name = p['id']; red = p['redline']
    rpms = [1500 + 1000 * k for k in range(8) if 1500 + 1000 * k <= red - 400] + [red - 300]
    out = {}
    for tag, l, th in (('full', .95, 1), ('part', .3, .3), ('off', -.6, 0)):
        lines = []
        for k, r in enumerate(rpms):
            lines += [f'{3 * k + t / 50:.2f} P {r} {l} {th} 3 60' for t in range(150)]
        a = render_v(name, f'rpmlvl-{tag}', lines)
        segs = [a[int((3 * k + .95) * SR):int((3 * k + 2.95) * SR)] for k in range(len(rpms))]
        out[tag] = [db(rms(s)) for s in segs]
        if tag == 'full': bass = [db(rms(low_band(s)[4800:])) for s in segs]
    fl = out['full']
    check(max(abs(b - bass[0]) for b in bass) <= 4, f'{name}: the low end (<120 Hz) at full load moves {[round(b - bass[0], 1) for b in bass]} dB against 1500 rpm (want within 4)')
    for tag in ('full', 'part', 'off'):
        L = out[tag]; steps = [L[k + 1] - L[k] for k in range(len(L) - 1)]
        check(min(steps) >= -.75 and L[-1] - L[0] >= 4, f'{name}: {tag} level vs rpm {[round(x, 1) for x in L]} (want rising, steps >= -0.75 dB, >= +4 dB to redline)')
    print(f'{name}: full load {fl[0]:.1f} -> {fl[-1]:.1f} dBFS from 1500 to {rpms[-1]} rpm (part load {out["part"][0]:.1f} -> {out["part"][-1]:.1f}, '
          f'off-gas {out["off"][0]:.1f} -> {out["off"][-1]:.1f}); below 120 Hz {min(bass):.1f}..{max(bass):.1f} dBFS (1500 rpm: {bass[0]:.1f})')

# ---- response: an rpm step reaches 90 % of the firing-frequency change within ~30 ms -----------------------------
# (zero crossings of the 0.5..1.5x firing band are too slow to resolve 30 ms; test_core checks the smoothed rpm
# state directly. Here: the audio after a 2000 -> 5000 step has the new firing peak 60 ms later.)
for name in ('i6', 'v12'):  # (a V8's 1.5 order can outweigh its firing order at 5000 rpm: no pitch test there)
    n = 6 if name == 'i6' else 12
    a = render(name, 'rpmstep', [f'{t / 50:.2f} P {2000 if t < 50 else 5000} .7 .8 3 60' for t in range(101)])
    x = a[int(1.06 * SR):int(1.06 * SR) + 4096] + [0] * (N - 4096)
    s = fft([v * (.5 - .5 * math.cos(2 * math.pi * i / 4095)) if i < 4096 else 0 for i, v in enumerate(x)])
    exp = 5000 / 60 * n / 2; k = max(range(int(exp * .7 * N / SR), int(exp * 1.3 * N / SR)), key=s.__getitem__)
    check(abs(k * SR / N / exp - 1) < .06, f'{name}: 60-145 ms after an rpm step the firing peak is {k * SR / N:.0f} Hz, want {exp:.0f}')

# ---- shifts: S events cost no level (same drive with and without S), rpm steps neither ---------------------------
def drive(kind, shift, step=False):
    lines = []
    for i in range(81):
        t = i / 20
        if kind == 'up':
            k = min(1, max(0, (t - 2) / .3)); k = k * k * (3 - 2 * k); r = 6000 + (2800 - 6000) * k; l, th, g0, g1 = .9, .94, 3, 4
            if step: r = 6000 if t < 2 else 2800
        elif kind == 'light':
            k = min(1, max(0, (t - 2) / .3)); k = k * k * (3 - 2 * k); r = 2200 + (1700 - 2200) * k; l, th, g0, g1 = .2, .2, 4, 5
        else:
            k = min(1, max(0, (t - 2) / .3)); k = k * k * (3 - 2 * k); r = 3000 + 1000 * k; l, th, g0, g1 = -.6, 0, 4, 3
        lines.append(f'{t} P {r:.1f} {l} {th} {g1 if t >= 2 else g0} 60')
        if shift and i == 40: lines.append(f'{t} S {g0} {g1}')
    return lines


worst = 0; worst_step = 0
for p in ('i6', 'v8', 'v10', 'v12'):
    for kind in ('up', 'light', 'down'):
        a = envelope(render(p, f'shift-{kind}', drive(kind, True)), 5)
        b = envelope(render(p, f'shift-{kind}-ref', drive(kind, False)), 5)
        w = max(abs(a[k] - b[k]) for k in range(int(1.9 * 200), int(2.8 * 200)))
        worst = max(worst, w)
        check(w <= 3, f'{p}: {kind}shift changes the level by {w:.1f} dB')
    a = render(p, 'shift-step', drive('up', True, True))
    e = envelope(a[int(1.5 * SR):int(3 * SR)], 8)
    lo, hi = window_ok(e, .5, 3)
    worst_step = max(worst_step, -lo, hi)
    # 3.5 dB (the demo's limit): an instantaneous 3200 rpm jump leaves the pipes' low end ~100 ms to rebuild; the car's
    # telemetry ramps over 250-400 ms (the strict S test above, and the demo shifts, keep 3 dB)
    check(lo >= -3.5 and hi <= 3.5, f'{p}: upshift as an rpm STEP: gap/onset {lo:.1f}/{hi:.1f} dB')
print(f'shifts: S events move the envelope by at most {worst:.1f} dB (limit 3); an rpm-step upshift stays within '
      f'{worst_step:.1f} dB of the settled levels')

# ---- transitions: preset switches, S, mute, X changes, malformed input, watchdog, quit ---------------------------
lines = []
for i in range(160):
    t = i / 20; lines.append(f'{t} P {1200 if t < 3 else 5500} .6 .7 3 60')
    if i in (20, 21, 22, 23): lines.append(f'{t} E ' + ['v8', 'v10', 'v12', 'i6'][i - 20])
    if i == 30: lines += [f'{t} X bass 2', f'{t} X offgas 0', f'{t} X pops 2', f'{t} X turbo 2']
    if i == 34: lines += [f'{t} X bass 0', f'{t} X offgas 2']
    if i == 38: lines += [f'{t} X bass 1', f'{t} X offgas 1', f'{t} X unknown 1', f'{t} X bass nan']
    if i == 40: lines.append(f'{t} S 3 4')
    if i == 70: lines.append(f'{t} S 4 3')
    if i == 90: lines.append(f'{t} M 1')
    if i == 110: lines.append(f'{t} M 0')
lines += ['8 P nan inf nan 3 50', '8 E invalid', '8 V nan', '8 X', '11 V 0.8', '12 Q']
a = render('v8', 'transitions', lines)
bad, w = clicks(a); check(bad == 0, f'transitions: {bad} clicks (slope ratio {w:.1f})')
check(lvl(a, 10.5, 11.5) < -80, 'stale-input watchdog did not fade out')
check(lvl(a, 5.1, 5.4) < -50, 'mute failed')
check(db(rms(a[-4800:])) < -80, 'quit fade failed')
print(f'transitions: preset switches, X changes, S, mute, malformed lines: no clicks (slope ratio {w:.1f}); '
      f'mute, watchdog and quit fade')

# ---- X settings -------------------------------------------------------------------------------------------------
def coast_level(x):
    a = render('i6', f'offgas{x}', [f'0 X offgas {x}'] + steady(3000, -.6, 0, 2))
    return lvl(a, 1, 2)


o = [coast_level(x) for x in (0, 1, 2)]
check(o[0] < o[1] - 10 and o[2] > o[1] + 2.5, f'X offgas: coast level {o}')
bassl = []
for x in (0, 1, 2):
    a = render('v8', f'bass{x}', [f'0 X bass {x}'] + steady(2500, .6, .6))
    s = spectrum(a, 1.2); bassl.append(10 * math.log10(energy(s, 30, 120) / energy(s, 400, 2000)))
check(bassl[2] - bassl[0] > 12 and bassl[0] < bassl[1] < bassl[2], f'X bass: low/mid balance {bassl}')
# turbo: solo renders (ES_SOLO=1 engine only, 2 turbo only) at full boost and at cruise
boost = [f'{t / 50:.2f} P {1500 + 50 * t if t < 50 else 4000} .95 1 3 80' for t in range(301)]
cruise = [f'{t / 50:.2f} P 2000 .2 .18 6 90' for t in range(301)]
eng = render('i6', 'tb-eng', boost, {'ES_SOLO': '1'}); ec = render('i6', 'tb-engc', cruise, {'ES_SOLO': '1'})
tl = []
for x in (0, 1, 2):
    t = render('i6', f'tb{x}', [f'0 X turbo {x}'] + boost, {'ES_SOLO': '2'})
    tl.append(lvl(t, 4, 6) - lvl(eng, 4, 6))
tc = render('i6', 'tbc', cruise, {'ES_SOLO': '2'})
toff = render('i6', 'tboff', ['0 T 0'] + boost, {'ES_SOLO': '2'})
check(-46 <= tl[1] <= -36, f'turbo at full boost {tl[1]:.1f} dB under the engine (want -46..-36: a faint whistle)')
part = [f'{t / 50:.2f} P {1500 + 50 * t if t < 50 else 4000} .6 .6 3 80' for t in range(301)]
tp = lvl(render('i6', 'tbp', part, {'ES_SOLO': '2'}), 4, 6)
check(tp < -80, f'turbo audible at 0.6 load: {tp:.1f} dBFS')
# X turbo 0 removes everything turbo-related; at cruise it changes nothing at all
cr3 = steady(3000, .2, .2, 6)
c0 = lvl(render('i6', 'cr-t0', ['0 X turbo 0'] + cr3), 2, 6); c1 = lvl(render('i6', 'cr-t1', cr3), 2, 6)
check(abs(c0 - c1) <= .5, f'X turbo 0 vs 1 at cruise: {c0 - c1:.2f} dB')
f0 = render('i6', 'full-t0', ['0 X turbo 0'] + boost); f1 = render('i6', 'full-t1', boost)
s0, s1 = spectrum(f0, 4.5), spectrum(f1, 4.5)
hfd = 10 * math.log10(energy(s1, 3000, 23900) / energy(s0, 3000, 23900))
check(tl[0] < -80 and 4 < tl[2] - tl[1] < 8, f'X turbo 0/1/2: {tl}')
check(lvl(tc, 4, 6) - lvl(ec, 4, 6) < -40, f'turbo audible at cruise: {lvl(tc, 4, 6) - lvl(ec, 4, 6):.1f} dB')
check(lvl(toff, 4, 6) < -80, 'T 0 does not silence the turbo')
check(0 <= hfd < 3, f'X turbo 1 adds {hfd:.1f} dB above 3 kHz at full boost (X turbo 0 must remove it, 1 only a faint whistle)')
# blow-off (recirculation): a lift after boost; a full-load upshift (pedal stays down) must not trigger it
lift = boost[:251] + [f'{5 + t / 50:.2f} P {4000 - 400 * t / 50} -.5 0 3 80' for t in range(101)]
bl = []
for x in (0, 1):
    bv = render('i6', f'bov{x}', [f'0 X bov {x}'] + lift, {'ES_SOLO': '3'})
    e = envelope(bv[int(4.9 * SR):int(5.8 * SR)], 1)
    pk = max(e); bl.append(pk - lvl(eng, 4, 5))
    dur = sum(1 for v in e if v > pk - 20) * .005
check(bl[0] < -80, 'X bov 0 does not silence the blow-off')
check(-20 <= bl[1] <= -8 and dur < .3, f'blow-off peak {bl[1]:.1f} dB re engine, {dur * 1000:.0f} ms (want -20..-8 dB, < 300 ms)')
up = boost[:251] + ['5 S 3 4'] + [f'{5 + t / 50:.2f} P {4000 - 1300 * min(1, t / 15):.0f} .95 1 4 80' for t in range(101)]
bvs = render('i6', 'bov-shift', up, {'ES_SOLO': '3'})
check(lvl(bvs, 4.9, 7) < -80, 'a gear shift triggered the blow-off')
# pops: V8 lift-off after a pull; transient count in the window after the lift
def pops(x, shift=False):
    """pops have their own random stream: the difference against the pops-0 render is exactly the pops"""
    lines = [f'0 X pops {x}'] + [f'{t / 50:.2f} P {3000 + 30 * t:.0f} .95 1 3 80' for t in range(101)]
    if shift: lines += ['2 S 3 4'] + [f'{2 + t / 50:.2f} P {6000 - 2000 * min(1, t / 15):.0f} .95 1 4 80' for t in range(101)]
    else: lines += [f'{2 + t / 50:.2f} P {6000 - 1500 * t / 50:.0f} -.6 0 3 80' for t in range(101)]
    return render('v8', f'pops{x}{int(shift)}', lines)


def pop_events(a, b):
    """events: rising edges of the 5 ms RMS of a - b above half the engine's own RMS after the lift"""
    seg = slice(int(2.05 * SR), int(3.55 * SR)); d = [x - y for x, y in zip(a[seg], b[seg])]
    ref = .5 * rms(b[seg]); e = [rms(d[i:i + 240]) for i in range(0, len(d) - 240, 240)]
    ev = sum(1 for k, v in enumerate(e) if v > ref and (k == 0 or e[k - 1] <= ref))
    return ev, db(max(e) / rms(b[seg]))


p0 = pops(0); pc = [pop_events(pops(x), p0) for x in (0, 1, 2)]
check(pc[0][0] == 0 and pc[1][0] >= 1 and pc[2][0] > pc[1][0] and pc[2][1] > pc[1][1] + 3, f'X pops 0/1/2 (events, peak dB) {pc}')
ps = pop_events(pops(2, True), pops(0, True))
check(ps[0] == 0, f'a gear shift triggered pops {ps}')
print(f'turbo: {tl[1]:.1f} dB under the engine at full boost, adds {hfd:.1f} dB above 3 kHz; silent at 0.6 load; '
      f'X turbo 0 vs 1 at cruise {c0 - c1:+.2f} dB')
print(f'X: offgas coast {o[0]:.1f}/{o[1]:.1f}/{o[2]:.1f} dBFS; bass low/mid {bassl[0]:.1f}/{bassl[1]:.1f}/'
      f'{bassl[2]:.1f} dB; turbo {tl[1]:.1f} dB under the engine at full boost (x2: {tl[2]:.1f}), '
      f'{lvl(tc, 4, 6) - lvl(ec, 4, 6):.1f} dB at cruise; blow-off peak {bl[1]:.1f} dB, {dur * 1000:.0f} ms; '
      f'pops 0/1/2: {pc[0][0]}/{pc[1][0]}/{pc[2][0]} events in 1.5 s, loudest {pc[1][1]:.1f}/{pc[2][1]:.1f} dB re the engine')
if fails:
    print(f'{len(fails)} FAILED'); sys.exit(1)
print('PASS: levels, pitch, brightness, off-gas level, response, shifts, transitions, X settings, watchdog, quit')
