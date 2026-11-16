# Simulate sound engine (`es-synth-simulate`), the default

Real-time procedural engine sound for the iDrive 7, C99. A pulse-and-pipe model built for three things:
sound as close to a real engine heard from the cabin as an objective comparison can get, **instant** response
to the car's telemetry, and very little CPU. Not a physics simulation. No samples, no heap allocation in the DSP,
no libm per sample, no extra thread, no compressor, no reverb. Output: 48 kHz S16LE stereo (identical channels).

Protocol and command line are exactly those of `../PROTOCOL.md`: `P`/`E`/`T`/`V`/`M`/`S`/`X`/`Q` lines on stdin;
`--out alsa:PCM|wav:FILE|raw`, `--preset`, `--volume`, `--demo SECONDS`, `--script FILE`, `--tone alsa:PCM`,
`--info`, `--bench SECONDS`. `main.c` and `out_alsa.c/.h` are the Waveguide engine's (proven in the car); `main.c`
only adds the `ES_SOLO` measurement hook and runs `--bench` with the turbo on. The shared speaker protection
(`../common/protect.h`) is the last stage.

## Build and verify

```
make host        # build/es-synth-host
make test        # test_core, render samples, check.py, bench
make car         # zig cc -target x86_64-linux-gnu.2.27, loader /lib/ld-linux-x86-64.so.2 -> ../../tool/lfb-enginesound/es-synth-simulate
make samples     # samples/{i6,v8,v10,v12}.wav: 40 s demo drives, mono (3.84 MB each)
```

`make car` copies the stripped ELF to `tool/lfb-enginesound/es-synth-simulate` and runs `tools/check_glibc.py` (all symbol
versions <= GLIBC_2.27 and the ELF interpreter is `/lib/ld-linux-x86-64.so.2`; the head unit has no `/lib64`).
`check.py` needs only python3's standard library.

## Behaviour

| | |
|---|---|
| Follow the rpm instantly | `P` rpm through two cascaded 6.5 ms one-poles: 90 % of a step in **26 ms**. No slew limit; the corners are rounded so a step never clicks. Load/throttle: 8 ms. |
| Shifts: crisp, no cut, no gap, nothing triggered | `S` upshift = a torque intervention, raised-cosine bump peaking at **70 ms**, gone at **180 ms**: pulses softer and less rasp (load x 0.68 for the timbre) and at most -1 dB of level. Combustion never stops. Downshift: a 160 ms rev-match accent. Pops, blow-off and tip-in bark are fired by the driver's *pedal* (which a ZF shift never moves) and additionally locked out for 0.4 s after any `S`. Measured: S events move the level by <= 1.5 dB; an upshift arriving as an rpm *step* stays within 1.5 dB of the settled levels. |
| Off the gas: not much quieter, and natural | Explicit level map: off-throttle (idle, coast, light load) **-4.4 dB** under full load at the same rpm. A lift-off goes like a real engine's: the **timbre** first (two one-poles, 30 ms: the edge, rasp and induction roar gone within ~100-150 ms, the motoring pulses rounder, knee down a third), then the **loudness** settles monotonically (two one-poles, 45 ms: 90 % in 160-210 ms), no dip-and-recover, no fade. On the overrun the motoring pulses get irregular per preset (V8 most, V12 least: higher variation, now and then a weak pulse). Fuel returns smoothly near idle (~120 ms), in ~25 ms on a tip-in; tip-in back to full level in 45-65 ms. Measured (16 averaged lifts from full load, 4000 rpm): drop 4.3-5.1 dB, 90 % in 160-210 ms, at most 0.2 dB under the settled level, the >1 kHz share 69-97 % of its fall done at 150 ms. |
| Turbo and blow-off subtle, no hiss | Turbo only under real load (boost demand starts above 0.55 load): **-40.3 dB** under the engine at full boost, silent at 0.6 load and at cruise; it adds 1.4 dB above 3 kHz at full boost. Turbo, whoosh and pops have their own random streams: `X turbo 0` removes the whistle and the spool air and leaves the engine sample-identical. The turbine's darkening of the exhaust follows spool and load (gas flow). Spool 0.35 s up, 0.4-0.8 s down. BMW uses recirculation valves: a single 200 ms soft whoosh with a 20 ms onset (band sweeping 2.7 -> 1.4 kHz), peak -19 dB re the engine, no flutter; `X bov 0` silences it. The pulse rasp's band now opens with load^2: its >3 kHz part is 7 dB lower at cruise and 5-6 dB lower at part load, unchanged at full load. |
| Pops & bangs on a genuine lift-off | After >= 0.6 s of load (average > 0.35) and a pedal release above 2000 rpm: a Poisson window of pressure kicks injected into the bank pipes (so they ring through the exhaust). `X pops` 0 = none, 1 = subtle (~2-4 per 1.5 s, around the coast level), 2 = sport exhaust (~16 per 1.5 s, up to +13 dB over the coast level, longer windows). Pops are high-passed at 150 Hz (thumps, not sub-bass) and keep their level relative to the overrun whatever the off-gas setting. |

## Sound settings (`X`, 0..2, 1 = default, smoothed with 80 ms, kept across `E`)

| Name | Effect |
|---|---|
| `offgas` | multiplies the off-throttle level (0.60 of full load at 1, -4.4 dB): 0 = silent off the throttle, 2 = as loud as full load |
| `turbo` | turbo whistle/air level (0 = none, 2 = +6 dB); `T 0` switches the layer off |
| `bov` | recirculation whoosh level (0 = none) |
| `pops` | overrun pops: 0 none, 1 subtle, 2 loud and more often |
| `bass` | low shelf at 150 Hz, `9 * (bass - 1)` dB (+-9 dB at 0/2), redesigned at control rate (click-free) |

## Design

Files: `engine.c` (one engine voice), `synth.c` (protocol, smoothing, shifts, X, crossfades, master), `presets.c`,
`dsp.h` (biquads, delay lines, tanh/sine tables, limiter).

**Source (per voice).** A double crank phase (720-degree cycles, never reset) advances by rpm/120 per second,
with a crank-speed ripple (each firing kicks dw/w ~ T(4pi/n)/(J w^2): a few % at idle, nothing at high rpm).
A sorted event table holds, per cylinder, an exhaust event at its firing angle and an intake event 360 degrees
later, with fixed per-cylinder trims (strength, width, angle). When the crank passes an event a pulse is taken
from a small pool (no allocation). Each pulse reads a **crank-angle pressure table** (blowdown: Gaussian rise,
exponential fall, a small exhaust-stroke bump, smooth end taper; a light-load and a heavy-load shape blended by
load, so the harmonic content is in engine orders and the load changes the hardness). Cycle-to-cycle variation:
strength COV (preset value at load, ~3x at idle), width variation, timing jitter on a fixed schedule (never
accumulates). Per exhaust path the pressures are summed, differentiated per unit crank phase (what an open
pipe radiates) and low-passed at the radiation knee (44-64 engine half-orders).

**Turbulence rides on the pulses.** White noise, low-passed with load^2 (rasp band ~0.5-3 kHz) and high-passed at
200 Hz, is *multiplied* by the path's own summed pulse pressure (normalised) and injected with the pulses: the
noise comes and goes with every firing, irregular with them, and rings in the same pipes (the load rasp).
More with load and rpm, 60 % less on the overrun.

**Exhaust.** Per bank a quarter-wave feedback waveguide (negative open-end reflection, wall-loss lowpass in the
loop, delay phase-compensated so the mode lands on c/4L), a transit delay, a collector waveguide, three
muffler chambers as feed-forward combs (delays nudged apart), packing lowpass, an asymmetric soft shaper whose
drive rises with load^1.5, rpm and boost (shock steepening, staged so it adds rasp, not loudness), and for
turbo engines a turbine high shelf that darkens with spool.

**Idle.** Near idle and off load the idle-speed control hunts: the crank speed wanders +-1.1 % (0.35-0.85 s
steps, 180 ms smoothing), on top of the low-load cycle variation (cycle energy COV 7-12 %).

**Intake.** A Helmholtz airbox bandpass and a direct path driven by the intake pulses, plus induction noise (a
band that opens with throttle flow) riding on the intake pressure.

**Low foundation** (replaces the old sub layer, which faded with rpm: the low end collapsed as soon as the firing
order climbed past ~120 Hz, e.g. the i6 at 2500 rpm had -36 dBFS below 120 Hz against -20 at 1500). Engine orders
0.5/1/1.5/2, crank-locked but each with a slowly drifting amplitude (0.75-1.25) and a sliding phase (0.1-0.6
cycles/s), plus structure-borne rumble (noise riding on the firing pressure), all band-limited to 32-115 Hz
(4th-order): what a cabin hears through the mounts and the body. It grows with load (a third of it on the
overrun and at idle) and follows the loudness path, so lifts and tip-ins stay smooth. Its gain per rpm comes from
the calibration below, so the absolute level below 120 Hz at full load stays within ~2.5 dB of its 1500 rpm value
up to redline (no hole, no single droning resonance, nothing below 30 Hz). The sliding phases keep it from sitting
in a fixed, rpm-dependent constructive or destructive sum with the exhaust's own low orders.

**Organic detail.** Cylinder-to-cylinder differences persist and drift (each cylinder's strength +-2 % and timing
+-0.35 deg wander to new targets every 1-2.5 s), on top of the white cycle-to-cycle jitter. Each combustion gets
its own roughness (the noise riding on it is 0.3-2x as strong). At high revs under load the rasp grows and the
timing jitter smears the high orders (a rip, not a whine). Quiet injector/valvetrain ticks (4.3 kHz bursts on the
intake events, irregular) are heard at idle and light load only. The master has a few broad body/exhaust formants
fixed in Hz that the orders sweep through. Separate random streams for per-sample noise, per-event variation,
slow drifts, pops and turbo: a setting or a shift never reshuffles the rest.

**Loudness calibration.** At start-up each voice is measured offline on a copy at 24 rpm points from idle to
redline, at no load, mid load (0.4), full load and on the overrun, listening through the master's fixed tone (so
what comes out is what the level map says) with the preset's default turbo at its steady boost; each point sweeps
slowly across its cell, so a narrow pipe resonance between two points is averaged in. The gains are blended in dB
through the mid-load point (a 0.3-load timbre is much closer to full load than to no load: a linear blend made
part load at 6500 rpm as loud as full load). Then the low foundation is sized per rpm: the full-load engine's own
share below ~100 Hz is measured and the foundation fills it up to a target share (falling 8.5 dB towards
redline, where the level map adds loudness); the total is renormalised per load. The explicit level map then
decides the loudness: +8.5 dB from idle to redline at any load, off-gas -4.4 dB under full load. The
correction follows the rpm in 40 ms (the pipes' low modes need that long to build up after a fast rpm drop).
~290 ms of CPU for the four voices on an M-series core.

**Master.** 28 Hz 4th-order high-pass, the bass shelf, a light cabin colour (58 Hz +1.5 dB, 125 Hz body
+1.5 dB, a broad 600 Hz presence lift +4.5 dB for the 2nd-5th firing harmonics, formants at 235 Hz +2, 480 Hz
+1.8, 1180 Hz +2, 2.4 kHz -2 dB (Q <= 2.5), 8 kHz panel lowpass, -3 dB treble shelf: inside the car, not muffled).
A little width: the band above ~300 Hz through two different allpass pairs makes a side signal (L + side,
R - side), so the mono sum is exactly the engine and the bass stays mono (L/R correlation 0.90-0.98 under load,
~1 at idle). Output gain, then a look-ahead peak limiter (1.3 ms, ceiling -6.5 dBFS, 20 ms release) that
detects the peaks as the speaker protection will see them, after a replica of its 30 Hz high-pass (a clipped
waveform regrows its peaks by 2-4 dB in that high-pass), and a last-resort soft clip at -5 dBFS. Voices crossfade in ~300 ms
on `E`; idle voices are not computed. Denormals are flushed in every recursive state; x86 FTZ/DAZ is set.

| Preset | Firing order / angles (deg) | Exhaust paths | Idle/redline | Character |
| --- | --- | --- | --- | --- |
| i6 | 1-5-3-6-2-4 / 0:120:600 | 1 (twin scroll merged at the turbine) | 700/7000 | B58: smooth 3rd order, turbo |
| v8 | 1-5-4-8-6-3-7-2 / 0:90:630 | 1-4, 5-8 (bank 2 pulses 20 % weaker/wider) | 650/7200 | S63 cross-plane: bank A 0/180/450/630, B 90/270/360/540: burble, turbo |
| v10 | 1-6-5-10-2-7-3-8-4-9 / 0,90,144,234,288,378,432,522,576,666 | 1-5, 6-10 | 900/8250 | S85 90/54 intervals: hard short pulses, short pipes, strong induction: shriek, NA |
| v12 | 1-7-5-11-3-9-6-12-2-8-4-10 / 0:60:660 | 1-6, 7-12 | 700/7500 | dense even pulses, NA |

These are evocative synthetic presets, not measured BMW engines or recordings.

## Measurements

**Against the Engine_Sim reference MP3s and the Waveguide engine**: the reference rpm/throttle traces replayed through each
synth (full throttle where the reference has throttle > 0.5, otherwise overrun), metrics over the whole clip.
Band shares are % of energy; tonal = share of 30-3000 Hz energy within +-12 % of a cycle-order line (1 = pure
harmonics); cyc corr = median correlation of consecutive engine cycles (1 = perfectly periodic); cyc COV = cycle
energy variation.

| | centroid Hz | 20-100 | 100-300 | 300-1k | 1-3k | tonal | cen@4500 rpm | crest dB | cyc COV | cyc corr |
|---|---|---|---|---|---|---|---|---|---|---|
| i6-turbo ref | 254 | 18 | 75 | 4.3 | 2.1 | .29 | 331 | 13.3 | 12.2 % | .32 |
| i6-turbo Waveguide | 199 | 23 | 75 | 2.1 | 0.3 | .32 | 229 | 11.7 | 11.5 % | .47 |
| i6-turbo **Simulate** | **254** | 15 | 74 | 8.7 | 1.9 | .34 | 281 | 13.1 | 12.9 % | .53 |
| v8 (NA) ref | 214 | 30 | 39 | 29 | 1.4 | .35 | 283 | 17.6 | 16.6 % | .60 |
| v8 (NA) Waveguide | 181 | 47 | 33 | 19 | 0.9 | .29 | 215 | 13.3 | 22.9 % | .32 |
| v8 (NA) **Simulate** | **272** | 35 | 33 | 28 | 3.8 | .31 | 310 | 14.8 | 20.8 % | .25 |
| v10 ref | 357 | 31 | 34 | 26 | 8.9 | .26 | 387 | 14.8 | 9.8 % | .17 |
| v10 Waveguide | 367 | 14 | 41 | 41 | 4.4 | .30 | 363 | 13.6 | 13.1 % | .31 |
| v10 **Simulate** | **392** | 8 | 42 | 43 | 6.6 | .37 | 413 | 13.9 | 14.1 % | .41 |
| v12 ref | 354 | 35 | 31 | 28 | 5.8 | .19 | 423 | 20.3 | 21.0 % | -.29 |
| v12 Waveguide | 372 | 20 | 32 | 43 | 4.8 | .23 | 409 | 13.3 | 15.6 % | -.12 |
| v12 **Simulate** | **395** | 9 | 26 | 60 | 4.7 | .35 | 416 | 13.4 | 16.5 % | .28 |

Read: Simulate's i6 and V8 match the references' spectral balance (centroid, mid band, high-rpm centroid) better
than Waveguide, which is 1-2 octaves darker in 300 Hz-3 kHz (it gets its mids only from its compressor's makeup).
V10/V12 have less below 100 Hz than the references (Engine_Sim adds a strong sub layer and bass enhancer;
in Simulate the V10/V12 low end is their real firing content plus a moderate sub) and are a bit more tonal (V12)
than Engine_Sim's.
1-6 kHz share at full throttle (i6-turbo/v8-turbo/v10/v12), 2000-3500 rpm: ref 0.03/0.12/1.0/0.9 %,
Simulate 0.4/0.5/1.9/1.3 %; 5000-7000 rpm: ref 4.5/9.5/10.9/7.5 %, Simulate 2.9/2.1/8.9/7.1 % (the V8 is duller at high
rpm than Engine_Sim's turbo V8, whose turbo layer is loud).

**Level against rpm** (raw WAV, `--volume 0.9`, steady states; full load / load 0.3 / off-gas, dBFS):

| i6 rpm | 700 | 1500 | 2500 | 3500 | 4500 | 5500 | 6500 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| full, before | -20.3 | -19.2 | -19.0 | -17.5 | -16.3 | -15.8 | -14.9 |
| full, now | -23.1 | -21.6 | -20.8 | -19.6 | -18.5 | -16.2 | -15.4 |
| load 0.3, before / now | -23.7 / -26.1 | -22.1 / -25.2 | -21.5 / -23.9 | -21.4 / -22.9 | -19.8 / -22.2 | -17.4 / -20.1 | -15.9 / -18.4 |
| off-gas, before / now | -24.9 / -26.8 | -23.2 / -25.9 | -22.8 / -25.2 | -21.7 / -23.6 | -21.1 / -22.9 | -20.1 / -21.2 | -19.2 / -19.8 |
| below 120 Hz at full load, before | -20.7 | -20.0 | **-36.3** | -27.2 | -25.9 | -22.8 | -21.0 |
| below 120 Hz at full load, now | -23.8 | -22.7 | -25.0 | -24.5 | -23.7 | -23.7 | -22.8 |

V12 below 120 Hz at full load: before -23.1 (700) / **-36.1** (1500) / -32.0 / -25.1 / -22.8 / -21.9 / -22.8 /
-23.9 (7200); now -24.3 / -24.3 / -24.2 / -24.6 / -22.6 / -23.8 / -22.9 / -21.8. L/R correlation: before 1.000
everywhere; now 0.89-0.98 under load, ~1 at idle (bass mono). Every preset's full,
part-load and off-gas levels rise with rpm (1000 rpm steps never fall by more than 0.75 dB, +5 to +7 dB from 1500
rpm to redline), the low end stays within 4 dB of its 1500 rpm value (measured 1.9-3.7 dB), off-gas sits 3.9-4.7
dB under full load at 3000 rpm. Full load near redline: -15.1..-16.4 dBFS RMS, peaks -4.1..-5.2 dBFS, bass
(<150 Hz) peaks -8.2..-9.9 dBFS. With the car's +6 dB live gain the protection's power and bass limiters stay
idle; its peak limiter takes ~1 dB off the crests at redline (raw peaks would need to be <= -7.5 dBFS for it to
stay idle: more than this engine's crest factor allows without audible limiting).

**Demo renders** (`samples/`): RMS -21.0..-21.8 dBFS, peaks -4.9..-6.0 dBFS.

**Bench** (`--bench 60`, one Apple M-series core, turbo layer on for every preset): i6 275x, v8 242x,
v10 249x, **v12 231x** realtime (Waveguide on the same host: 160-193x). Start-up calibration: ~290 ms CPU.

`make test` checks: rpm-step response (26 ms to 90 %), finite internal states and the -3 dBFS bound across
extreme sweeps/preset switches/shifts/X settings; demo safety (finite, peak <= -3 dBFS, RMS window, no clicks
by a relative sample-slope test), every demo upshift within 3.5 dB of the settled levels (the Waveguide engine's demo limit; the V8's own roughness is ~3 dB there); firing pitch at
1800/3600 rpm, V8/V10 half-order character, i6 smoothness (orders above 150 Hz), brightening with load; the low
end (<120 Hz) at full load within 4 dB of its 1500 rpm value up to redline; full, part-load and off-gas levels
rising with rpm (steps >= -0.75 dB, >= +4 dB to redline); >3 kHz at cruise >= 30 dB
under the total; the lift-off (16 averaged lifts: never more than 1 dB under the settled level, no rise over 1 dB on the way
down, 90 % within 300 ms, the >1 kHz share >= 60 % of its fall at 150 ms, no clicks), tip-in within 100 ms; off-gas level (coast and
light load 3-5 dB under full load at 3000 rpm); an rpm step's new firing frequency 60 ms later; S events
against the same drive without S (<= 3 dB) for full-load, light-load and overrun shifts, and an upshift as an
instantaneous rpm step (<= 3.5 dB: the pipes' low end needs ~100 ms to rebuild after a 3200 rpm jump; the car's
telemetry ramps); preset switches, X changes, S, mute and malformed lines without clicks; mute, stale-input watchdog,
quit fade; each X setting (offgas, bass, turbo level at boost/0.6 load/cruise/T 0, X turbo 0 vs 1 at cruise
within 0.5 dB and at boost above 3 kHz, blow-off level/length and no
blow-off from a shift, pops 0/1/2 event counts and no pops from a shift). These are objective regressions and
do **not** establish subjective quality, Apollo Lake CPU use or SmartX latency: that needs listening in the car.

Measurement hook: `ES_SOLO=1` renders the engine only, `2` the turbo only, `3` the blow-off only, `4` the pulse
rasp only (through the exhaust), `5` the engine without the rasp, `6` the induction noise only, `8` the low
foundation only.

## Known weaknesses

- Not auditioned: every number above is measured, none is listened to.
- The low foundation is a designed layer (orders + rumble), not a measured mount/body transfer.
- The V8's half-order (burble) share rises with rpm (it comes from the two banks' different pulse shapes
  meeting slightly different pipes); at idle it is modest.
- Steady states between the calibration points: the off-gas gap measured 2.1-7.2 dB over 1500-6500 rpm
  (3-5 dB at most points; the 3000 rpm test point 3.4-4.5 dB).
- The V8's overrun burble and its exhaust resonance near 6300 rpm swing a single 100 ms envelope by ~3 dB, so its
  lift and demo-shift checks need averaging (16 lifts) or a 165 ms window; the strict shift test is unaffected.
- Start-up costs ~290 ms CPU here (perhaps 1-1.5 s on the head unit's Atom) before the first block.
- Loudness against rpm is smooth at 1000 rpm steps; at 500 rpm resolution it still ripples by up to ~0.8 dB
  (pipe/muffler colour and the slow drifts).
- A V8 upshift arriving as an instantaneous 3200 rpm telemetry step dips ~3.3 dB for ~100 ms (the strict shift test
  with the car's ramped rpm: <= 1.7 dB).
- Near redline the protection's peak limiter (after the car's +6 dB) takes ~1 dB off the crests.
- The turbo whistle is a narrow band at 1.4-4.6 kHz, now faint; the blow-off has no BMW-specific recording behind it.

## Credits and licenses

The Simulate engine is new code. Its ideas and several constants come from these MIT-licensed projects:

- [YigitSalihEmecen/Engine_Sim](https://github.com/yigitsalihemecen/Engine_Sim) (MIT, Copyright (c) 2026
  Yigit Salih Emecen): the event-per-cylinder source with sub-sample onsets and cycle variation, crank-speed
  ripple, pressure gradient per unit phase with a radiation knee, the quarter-wave pipe with negative open-end
  reflection and phase-compensated delay, the muffler chambers as feed-forward combs, the Helmholtz airbox, the
  saturated sub layer; pipe lengths, reflections, damping, chambers and airbox values of its i6, v8cross, v10,
  v12 profiles (as the Waveguide engine uses them). License text below.
- [DasEtwas/enginesound](https://github.com/DasEtwas/enginesound) (MIT) and
  [Antonio-R1/engine-sound-generator](https://github.com/Antonio-R1/engine-sound-generator) (MIT), after Baldan
  et al. 2015: crank-angle-domain pulse/valve functions with separate intake and exhaust events.
- [ange-yaghi/engine-sim](https://github.com/ange-yaghi/engine-sim) (MIT): noise multiplied onto the pressure
  signal rather than added as a bed.

No code was copied from these projects. The crank-angle pulse tables, the per-bank pulse asymmetry, the
start-up loudness calibration, the level map, the shift/pedal logic, pops, recirculation whoosh, turbo layer,
master chain and BMW firing geometry are original to this engine; `main.c` and `out_alsa.c/.h` are the Waveguide
engine's.

Engine_Sim license:

Copyright (c) 2026 Yigit Salih Emecen

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
