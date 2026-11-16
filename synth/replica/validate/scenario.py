"""Write the validation drive script for a preset: steady states + a pull, a shift, a lift (pops), steady loads."""
import sys
P = {'i6': (700, 7000, 1), 'v8': (650, 7200, 1), 'v10': (900, 8250, 0), 'v12': (700, 7500, 0)}
name, out = sys.argv[1], sys.argv[2]
turbo = int(sys.argv[3]) if len(sys.argv) > 3 else P[name][2]
idle, red, _ = P[name]
lines = [f'0 E {name}', f'0 T {turbo}']
gear = 0
def state(t):
    if t < 4: return idle, .05, .03, 0
    if t < 8: return 2500, .4, .45, 3
    if t < 12:
        x = (t - 8) / 4
        return 2500 + x * (0.85 * red - 2500), .95, 1, 2
    if t < 15:
        x = (t - 12) / 3
        return 0.55 * red + x * 0.25 * red, .95, 1, 3
    if t < 19:
        x = (t - 15) / 4
        return 0.8 * red + x * (2000 - 0.8 * red), -.6, 0, 3
    if t < 23: return 4000, .7, .8, 4
    if t < 27: return 0.85 * red, 1, 1, 3
    return idle, .05, .03, 0
t = 0.0
while t < 30:
    r, l, th, g = state(t)
    if g != gear:
        lines.append(f'{t:.3f} S {gear} {g}')
        gear = g
    lines.append(f'{t:.3f} P {r:.1f} {l:.3f} {th:.3f} {g} {r * .02 * max(g, 0):.1f}')
    t += 0.02
open(out, 'w').write('\n'.join(lines) + '\n')
