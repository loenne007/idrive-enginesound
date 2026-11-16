# es-synth

C99 port of the sound engine of [Engine_Sim](https://github.com/YigitSalihEmecen/Engine_Sim) (Yigit Salih
Emecen, MIT), the browser Web Audio procedural engine sound, driven by this app's car data instead of
Engine_Sim's simulated drivetrain. Output: 48,000 Hz, signed 16-bit little-endian, interleaved stereo. No
samples, no extra thread, no allocation after start-up, libc + libm only (libdl for ALSA on Linux).

The goal is fidelity to Engine_Sim's sound, not a new design: every module of its audio path is ported
line by line, and the Web Audio nodes it is built from are reimplemented with Chromium's semantics. Measured
against the original running in headless Chrome on the same control data, the long-term 1/3-octave spectra
agree within 0.15-0.33 dB on average (see Validation).

## Build and verify

```
make host          # build/es-synth-host
./test.sh          # unit + render checks, renders samples/, prints the benchmark
make car           # x86-64 ELF (zig cc, glibc <= 2.27, interpreter /lib/ld-linux-x86-64.so.2)
./render_all.sh    # samples/{i6,v8,v10,v12}.wav, 40 s demo each
```

`make car` uses `$(HOME)/.local/zig/zig-aarch64-macos-0.17.1/zig` (override `ZIG`) with
`-target x86_64-linux-gnu.2.27 -O2 -s -Wl,--dynamic-linker=/lib/ld-linux-x86-64.so.2` (the head unit has no
`/lib64`), copies the stripped ELF to `../../tool/lfb-enginesound/es-synth-b`, and checks the interpreter (`file`)
and the glibc symbol versions (`check_glibc.py`, details in `build/glibc-symbols.txt`).

`samples/*.wav` are 40-second **mono** previews ((L+R)/2 of the stereo render: the stereo widener's side
signal cancels exactly in that sum), 3,840,044 bytes each. Live and WAV output are stereo.

## Use

```
./es-synth --out alsa:plug:smartx:OAPGenericOverlay_CF_48_STEREO --preset v8 --volume 0.8
./build/es-synth-host --out wav:build/demo.wav --demo 40 --preset v10
./build/es-synth-host --out wav:build/events.wav --script build/drive.txt
./es-synth --out raw --preset i6
./es-synth --tone alsa:plug:smartx:OAPGenericOverlay_CF_48_STEREO
./es-synth --info
./build/es-synth-host --bench 60
```

`--info` emits JSON: `sample_rate`, and `presets`, each with `id`, `idle`, `redline`, `cylinders`,
`turbo_default` (unchanged: i6 700/7000, v8 650/7200, v10 900/8250, v12 700/7500; turbo on for i6/v8).
Logs use stderr; raw PCM uses stdout. Raw mode runs at wall-clock speed, ALSA at the device's pace, WAV offline.
`--tone` plays three short 880 Hz beeps per second for ten seconds. SIGINT/SIGTERM trigger the quit fade.
`--bench` renders the 40 s demo sequence (scaled) for every preset **with the turbo on** (the worst case).

Timed scripts contain `<seconds> <control line>`, in chronological order (P at least every 1.5 s, the
watchdog applies). The offline duration is the last timestamp plus one second. The demo: three seconds idle,
two free revs, acceleration through gears 1-5, overrun down through the gears, light cruise, idle, fade.

## Control protocol

Newline-terminated ASCII on stdin, typically 20-100 Hz:

| Command | Effect |
| --- | --- |
| `P rpm load throttle gear speed_kmh` | rpm; signed load -1..1 (torque share, < 0 = engine braking); throttle (pedal) 0..1; gear -1..10; speed |
| `E i6\|v8\|v10\|v12` | switch engine: 300 ms crossfade between independent voices; selects that engine's turbo default |
| `T 0\|1` | turbo off (faded, as Engine_Sim's `release()`) / on |
| `V 0..1` | volume (Engine_Sim's master gain, 50 ms smoothing) |
| `M 0\|1` | unmute/mute, about 300 ms to 95 % |
| `S fromGear toGear` | gear change (both > 0): Engine_Sim's shift sequence with its events |
| `Q` | fade out, exit after 800 ms |

Malformed/nonfinite values and unknown engine names are ignored; P values are clamped. Overlong lines are
discarded; one bounded nonblocking read per block. No P for 1.5 s fades out (also at start-up) and new data
fades back in; while fully faded the DSP is skipped. ALSA handling (`out_alsa.c`: dlopen, device-paced writes
that keep the device buffer full, EAGAIN drop, underrun recovery, reopen every two seconds) is unchanged.

## Architecture

```
P/S lines -> control model (synth.c) -> params object every 768 frames (16 ms, an "animation frame")
          -> per engine voice (engine.c):
               combustion events (combustion-worklet.js) -> per-bank exhaust waveguides -> transit delays
                 -> collector waveguide -> muffler -> 4x-oversampled shock nonlinearity -> tremolo, turbine shelf
               + exhaust flow noise and pop/flutter sends into the pipes, intake Helmholtz + induction noise,
                 sub layer, transmission whine/rattle, turbo layer, transient bank
          -> mix bus (Engine_Sim's default mix) -> shared output stage (fx.c):
               infrasonic LR4, bass enhancer, tone shelves, cabin (exterior), air, convolution reverb,
               stereo widener, three-band compressor, limiter, volume, safety clipper -> x0.89 -> S16
```

Files: `wa.c/.h` Web Audio node equivalents; `engine.c/.h` one engine voice; `fx.c` output stage; `synth.c`
control model, voices, gains; `presets.c` profiles; `main.c`/`out_alsa.c` CLI and output (unchanged except the
benchmark enabling the turbo).

Web Audio semantics reproduced (`wa.c`): render quanta of 128 frames (k-rate params of the combustion source
are sampled per quantum); AudioParam `setTargetAtTime` as Chromium's per-sample one-pole, and an event timeline
for `setValueAtTime` / `linearRamp` / `exponentialRamp` / `cancelScheduledValues` (transients, blow-off valve);
BiquadFilterNode with Chromium's coefficient formulas (lowpass/highpass Q in dB, shelves S = 1), run as TDF2;
DelayNode with linear interpolation; WaveShaperNode with Chromium's 2x/4x Blackman-windowed-sinc up/down
samplers (128/256 taps) and curve interpolation; OscillatorNode/PeriodicWave as 36 band-limited 4096-point
tables per wave (normalised), chosen by fundamental and cross-faded like Chromium; looping buffer sources with
Engine_Sim's noise buffers (its seeded ones bit-exact, including the JavaScript sign-extending `>> 17` of its
xorshift); ConvolverNode with Chromium's IR normalisation, as a zero-latency two-stage partitioned FFT
convolution (128-frame partitions for the first 1024 taps, 1024-frame partitions for the rest of the 0.81 s IR);
DynamicsCompressorNode as a port of Chromium's `DynamicsCompressorKernel` (6 ms look-ahead, knee curve, adaptive
release polynomial, auto make-up, sin warp); the stereo ChannelMerger downmix.

One Chromium behaviour matters a lot and is reproduced deliberately: a feedback cycle through a DelayNode is
broken where the graph re-enters a node, so the waveguide loop reads its loop filter's output from the previous
render quantum. Each exhaust pipe therefore resonates at odd multiples of 1/(2(T + 128/48000)), not 1/(2T) as
Engine_Sim's comments assume. Without this the pipe modes land 25-30 % too high and steady-state levels
differed by up to 6 dB from the browser.

## Control model (replaces physics.js / shift.js / gearbox.js)

Engine_Sim derives its params object from a simulated drivetrain; here it is built from the car data:

- rpm: the P value smoothed with 30 ms (P arrives at ~50 Hz); `dRpm` from it, smoothed 50 ms; `f0 = rpm/120`.
- combustion load: `max(load, 0)` (torque share), zeroed by shift fuel cuts / replaced by the downshift blip,
  then smoothed exactly as physics.js (12/s). Pedal = the P throttle.
- overrun: physics.js's DFCO term with "wheels drive the engine" taken from `load < 0`, smoothed 8/s.
- exhaust heat, fuel cut and pops: physics.js `_stepExhaustThermal` (EGT with 5 s / 26 s time constants,
  lift-off bang probability from heat, revs and how hard the pedal was snapped shut, unburnt-fuel charge,
  `popIntensity` for the crackle stream, blip bang) - with fuel cut only after the overrun (load <= 0) has
  lasted 0.1 s.
- turbo: physics.js `_stepTurbo` boost pressure and blow-off event; the audible spool, whine, flutter etc. are
  the TurboLayer's own model, unchanged.
- shifts (`S a b`): shift.js's manual box for the 'sports' car (95 ms): cut 30 ms, open 35 ms (70 ms on a
  downshift, with a 0.85 rev-match blip), sync 18 ms, engage 50 ms (smoothstep clutch), lash, shuffle 360 ms;
  heat-gated probabilistic `evCut` bang, `evEngage` from the ratio step, `evShiftDone`, `evLash` from the pedal.
- gear ratio, propshaft rpm (from speed, wheel radius 0.33 m, final drive 3.73) and gear teeth: gearbox.js's
  design for each engine in the 'sports' vehicle; gears beyond the designed box reuse the top gear.

Sound settings are Engine_Sim's defaults, as used for its demo samples: default mix, tone 1/1/1, flat EQ,
reverb 0.32/0.14/4200 Hz, width 0.35, dynamics 1, exterior, live inputs 0, turbo config defaults.

## Validation

`validate/` replays the port's own per-frame params (`ES_DUMP_PARAMS=file`) through the real Engine_Sim graph
in headless Chrome (OfflineAudioContext, its `_writeParams`), so both get identical control data, and compares
(`compare.py`): 1/3-octave long-term spectrum, RMS envelope (100 ms), spectral centroid, steady-state level,
crest factor and half-order harmonic levels. `ES_VALIDATE=1` disables the +3 dB render make-up for the
comparison; the port's final x0.89 is divided out. 30 s scenario: idle, 2500 rpm cruise, a pull, an upshift,
a lift with pops, 4000 rpm part load, 0.85 x redline full load. Results (C - browser):

| preset | 1/3-oct mean abs / max (31 Hz-16 kHz) | RMS envelope mean abs, corr | total level | steady states: level, crest, centroid |
| --- | --- | --- | --- | --- |
| i6 (turbo) | 0.16 / 0.6 dB | 0.47 dB, 0.991 | -19.27 / -19.29 dB | within 0.4 dB, 0.5 dB, 4 % |
| v8 (turbo) | 0.33 / 1.2 dB | 0.66 dB, 0.981 | -20.34 / -20.05 dB | within 0.5 dB, 0.5 dB, 8 % |
| v10 | 0.15 / 0.9 dB | 0.44 dB, 0.993 | -22.85 / -22.80 dB | within 0.4 dB, 0.1 dB, 2 % |
| v12 | 0.20 / 1.0 dB | 0.41 dB, 0.992 | -23.80 / -23.75 dB | within 0.3 dB, 0.6 dB, 2 % |
| v12 + turbo | 0.23 / 1.1 dB | 0.40 dB, 0.993 | -23.86 / -23.82 dB | within 0.3 dB, 0.7 dB, 9 % |

Half-order harmonic levels at the steady states differ by 0.4-2.6 dB on average (cycle-to-cycle combustion
variation is random in both, so individual harmonics wander). Spectrograms: `build/compare.png`.

## Performance

`--bench` on an Apple M4 Max core, every preset with the turbo on: i6 38.8x, v8 37.8x, v10 36.4x, v12 35.2x
realtime. The cost is dominated by Chromium's oversampling FIRs (4x on the exhaust and the turbo whine, 2x on
five more shapers) and the per-event combustion pulses. Optimisations that do not change the result: symmetric
FIR folding, fast exp/log polynomials (rel. error ~1e-7), coefficient refresh only when a parameter moved,
partitioned FFT reverb, FTZ/DAZ, and skipping work whose output gain is exactly zero (idle transient voices,
the wastegate/flutter shapers while gated off, the whole DSP while muted or without data).

## Deviations from Engine_Sim

- Idle/redline are this app's (the controller maps the car's rpm onto them); everything Engine_Sim derives from
  them (rpmNorm, sub-layer order, turbo onset, rattle, crackle thresholds) uses these values.
- The drivetrain, shift and gearbox simulations are replaced by the control model above.
- Biquad coefficients are refreshed every 16 frames (0.33 ms) instead of every sample when a parameter moves;
  audio-rate frequency modulation of two turbo flutter band-passes is sampled at that rate too.
- Oscillator band-limited table choice is made every 16 frames; noise buffers that Engine_Sim fills with
  `Math.random()` (flow noise, brown noise, reverb IR) use a fixed-seed generator instead.
- One mono convolution with the average of the stereo IR (exact, see fx.c).
- Engine switches crossfade over 300 ms between independent voices (Engine_Sim rebuilds the graph and cuts).
- Output: +3 dB into the limiter (Engine_Sim's published renders are peak-normalised by +0.2..+5 dB), volume
  as its master gain, then mute/watchdog/quit fades and x0.89 after the safety clipper, so peaks stay below
  -1 dBFS (0.99 x 0.89).
- Transmission driveshaft-scale auto detection is fixed to "propshaft rpm", which is what is supplied.

## Remaining weaknesses

- The control model only approximates Engine_Sim's physics: no clutch slip or driveline shuffle dynamics, the
  lash/engage magnitudes are estimates, and the virtual rpm is the car's scaled rpm (no simulated rev-limiter
  bounce, no rev-match rise on blips).
- Validation is objective (spectra, envelopes, harmonics), not a listening test, and covers one scenario per
  preset; actual Atom E3950 CPU load, SmartX latency and in-car loudness still need measuring on the car.

## Credits and license notice

Engine sound: a C port of [Engine_Sim](https://github.com/YigitSalihEmecen/Engine_Sim) by Yigit Salih Emecen
(src/combustion-worklet.js, pulse.js, profiles.js, resonators.js, character.js, layers.js, fx.js, the mapping in
engine-sim.js, and parts of physics.js / shift.js / gearbox.js for the control model), used under the MIT
license:

Copyright (c) Yigit Salih Emecen

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

Web Audio node behaviour follows the Web Audio API specification and Chromium's implementation (formulas only,
reimplemented).
