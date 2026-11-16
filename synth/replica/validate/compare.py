"""compare.py tag [tag...] [--png out.png] : C port (c_<tag>.wav) vs the browser original (js_<tag>.wav)."""
import sys, wave, json
import numpy as np

SR = 48000
GAIN_C = 1 / 0.89          # the port's final -1 dBFS scaling, taken back out
P = {'i6': (700, 7000, 6), 'v8': (650, 7200, 8), 'v10': (900, 8250, 10), 'v12': (700, 7500, 12)}

def read(path):
    with wave.open(path, 'rb') if False else open(path, 'rb') as f:
        data = f.read()
    fmt = int.from_bytes(data[20:22], 'little'); ch = int.from_bytes(data[22:24], 'little')
    bits = int.from_bytes(data[34:36], 'little')
    i = 12
    while data[i:i + 4] != b'data':
        i += 8 + int.from_bytes(data[i + 4:i + 8], 'little')
    n = int.from_bytes(data[i + 4:i + 8], 'little'); raw = data[i + 8:i + 8 + n]
    a = np.frombuffer(raw, dtype='<f4' if fmt == 3 else '<i2').astype(np.float64)
    if fmt != 3: a /= 32768
    return a.reshape(-1, ch)

def third_octave(x):
    f = np.fft.rfftfreq(len(x), 1 / SR)
    X = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    centers = 1000 * 2.0 ** (np.arange(-17, 14) / 3)   # 20 Hz .. 20 kHz
    out = []
    for c in centers:
        lo, hi = c / 2 ** (1 / 6), c * 2 ** (1 / 6)
        out.append(X[(f >= lo) & (f < hi)].sum())
    return centers, 10 * np.log10(np.array(out) + 1e-20)

def frames(x, n=4800):
    return x[:len(x) // n * n].reshape(-1, n)

def centroid(x):
    fr = frames(x, 4096)
    f = np.fft.rfftfreq(4096, 1 / SR)
    S = np.abs(np.fft.rfft(fr * np.hanning(4096), axis=1)) ** 2
    return (S * f).sum(1) / (S.sum(1) + 1e-20)

def orders(x, rpm, cyl, kmax=None):
    """level (dB) of half-orders k*f0/2 relative to the strongest, f0 = rpm/120"""
    n = len(x)
    X = np.abs(np.fft.rfft(x * np.hanning(n)))
    f0 = rpm / 120
    kmax = kmax or 2 * cyl
    res = []
    for k in range(1, kmax + 1):
        fk = k * f0
        b = int(round(fk * n / SR))
        res.append(X[max(b - 2, 0):b + 3].max())
    res = np.array(res)
    return 20 * np.log10(res / res.max() + 1e-12)

def analyse(tag):
    name = tag.rstrip('t')
    c = read(f'c_{tag}.wav') * GAIN_C
    j = read(f'js_{tag}.wav')
    n = min(len(c), len(j))
    c, j = c[:n], j[:n]
    cm, jm = c.mean(1), j.mean(1)
    sl = slice(int(1.0 * SR), int(29.0 * SR))
    fc, Lc = third_octave(cm[sl]); _, Lj = third_octave(jm[sl])
    ref = Lj.max()
    rep = {'tag': tag}
    band = [(round(float(f)), round(float(a - ref), 1), round(float(b - ref), 1), round(float(a - b), 1))
            for f, a, b in zip(fc, Lc, Lj)]
    rep['third_octave'] = band
    audible = [d for f, a, b, d in band if 31 <= f <= 12000 and b > -60]
    rep['third_octave_mean_abs_db'] = round(float(np.mean(np.abs(audible))), 2)
    rep['third_octave_max_abs_db'] = round(float(np.max(np.abs(audible))), 2)
    rc = np.sqrt((frames(cm) ** 2).mean(1)); rj = np.sqrt((frames(jm) ** 2).mean(1))
    k = slice(10, 290)
    d = 20 * np.log10((rc[k] + 1e-9) / (rj[k] + 1e-9))
    rep['rms_env_mean_db'] = round(float(d.mean()), 2)
    rep['rms_env_mean_abs_db'] = round(float(np.abs(d).mean()), 2)
    rep['rms_env_corr'] = round(float(np.corrcoef(20 * np.log10(rc[k] + 1e-9), 20 * np.log10(rj[k] + 1e-9))[0, 1]), 3)
    cc, cj = centroid(cm), centroid(jm)
    k2 = slice(12, len(cc) - 12)
    rep['centroid_c_mean_hz'] = round(float(cc[k2].mean()))
    rep['centroid_js_mean_hz'] = round(float(cj[k2].mean()))
    rep['centroid_mean_abs_rel'] = round(float(np.mean(np.abs(cc[k2] - cj[k2]) / cj[k2])), 3)
    rep['total_rms_db'] = [round(float(20 * np.log10(np.sqrt((cm[sl] ** 2).mean()))), 2),
                           round(float(20 * np.log10(np.sqrt((jm[sl] ** 2).mean()))), 2)]
    idle, red, cyl = P[name]
    steady = {'idle': (1, 4, idle), '2500': (5, 8, 2500), '4000': (20, 23, 4000), 'high': (24, 27, 0.85 * red)}
    rep['steady'] = {}
    for key, (a, b, rpm) in steady.items():
        s = slice(int(a * SR), int(b * SR))
        crest = lambda x: 20 * np.log10(np.abs(x).max() / np.sqrt((x ** 2).mean()))
        oc, oj = orders(cm[s], rpm, cyl), orders(jm[s], rpm, cyl)
        lc = 20 * np.log10(np.sqrt((cm[s] ** 2).mean())); lj = 20 * np.log10(np.sqrt((jm[s] ** 2).mean()))
        rep['steady'][key] = {'level_db': [round(float(lc), 1), round(float(lj), 1)],
                              'crest_db': [round(float(crest(cm[s])), 1), round(float(crest(jm[s])), 1)],
                              'centroid_hz': [round(float(centroid(cm[s]).mean())), round(float(centroid(jm[s]).mean()))],
                              'order_dev_db_mean_abs': round(float(np.mean(np.abs(np.maximum(oc, -40) - np.maximum(oj, -40)))), 1),
                              'orders_c': [round(float(v)) for v in oc[:2 * cyl]],
                              'orders_js': [round(float(v)) for v in oj[:2 * cyl]]}
    return rep, cm, jm

def spec(ax, x, title):
    import matplotlib
    ax.specgram(x, NFFT=4096, Fs=SR, noverlap=3072, cmap='magma', vmin=-130, vmax=-20, scale='dB')
    ax.set_yscale('symlog', linthresh=200); ax.set_ylim(30, 12000); ax.set_title(title, fontsize=9)

if __name__ == '__main__':
    args = sys.argv[1:]
    png = None
    if '--png' in args:
        i = args.index('--png'); png = args[i + 1]; del args[i:i + 2]
    out = []
    sigs = []
    for tag in args:
        rep, cm, jm = analyse(tag)
        out.append(rep); sigs.append((tag, cm, jm))
        print(json.dumps({k: v for k, v in rep.items() if k != 'third_octave'}))
        print('  1/3 oct C-JS dB:', ' '.join(f'{f}:{d:+.1f}' for f, a, b, d in rep['third_octave'] if 25 <= f <= 16000))
    json.dump(out, open('compare.json', 'w'), indent=1)
    if png:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        fig, axs = plt.subplots(len(sigs), 2, figsize=(16, 3.2 * len(sigs)), squeeze=False)
        for r, (tag, cm, jm) in enumerate(sigs):
            spec(axs[r][0], jm, f'{tag}: Engine_Sim (Chromium)')
            spec(axs[r][1], cm, f'{tag}: C port')
        plt.tight_layout(); plt.savefig(png, dpi=70)
