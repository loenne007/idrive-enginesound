# Waveguide sound engine (`es-synth-waveguide`), legacy

Engine_Sim's sound path rewritten in C99 for the 320d's iDrive 7: combustion
events into delay-line waveguide pipes, muffler, airbox, a sub layer, a turbo
layer and the master chain, driven live by the car's telemetry. No recorded
samples, no ALSA build dependency, no heap allocation in the DSP, no
convolution, no extra thread. Output: 48,000 Hz, signed 16-bit little-endian,
interleaved stereo (identical L/R). The shared speaker protection
(`../common/protect.h`) is the last stage.

## Build and verify

```
make host
make test
make car
make samples
```

`make host` produces `build/es-synth-host`. `make car` uses
`$(HOME)/.local/zig/zig-aarch64-macos-0.17.1/zig` (override `ZIG`) with
`-target x86_64-linux-gnu.2.27 -O2 -s` and the loader `/lib/ld-linux-x86-64.so.2`,
then copies the stripped ELF to `../../tool/lfb-enginesound/es-synth-waveguide`
and checks it (`file`, `../../tools/check_glibc.py`: symbol versions <= 2.27,
the loader; the symbols go to `build/glibc-symbols.txt`).
Both targets use only libc and libm; Linux also links libdl. Python 3, without
third-party packages, supports offline verification and sample packaging.

`make test` checks internal floating-point DSP states (pulses, waveguides,
compressor) on extreme RPM/load/throttle sweeps with shifts and turbo,
limiter bounds, all four demo levels, relative sample-slope outliers, firing
pitch at 1,800 and 3,600 RPM, spectral brightening under load, V8 half orders /
I6 smoothness, the V8/V10 crackle window after a lift-off (crest factor right
after the lift against steady load), gear-shift level continuity (the same
drive rendered with and without `S` events, and an overrun downshift: the
envelope may not move more than 3 dB; an rpm step and every upshift of the four
demo renders stay within 3 / 3.5 dB of the settled levels), the `X` settings
(each changes what it should, 0 removes turbo/bov/pops, offgas leaves full load
alone, no clicks while changing them),
rapid engine switching, RPM jumps, malformed commands, mute, quit and stale-input fading. It then prints a 60-second benchmark for
all presets. FFT tests use Hann windows and are objective regressions, not a
substitute for listening. `build/` contains additional stereo test renders.

The four `samples/*.wav` are the 40-second demo drives as **mono** PCM
(3,840,044 bytes each): `make samples` renders the normal stereo file and drops
the identical right channel. Live and normal WAV output remain stereo.

## Use

```
es-synth-waveguide --out alsa:plug:smartx:CarLifeMedia_CF_48_STEREO --preset v8 --volume 0.8
./build/es-synth-host --out wav:build/demo.wav --demo 40 --preset v10
./build/es-synth-host --out wav:build/events.wav --script build/drive.txt
./build/es-synth-host --out raw --preset i6
es-synth-waveguide --tone alsa:plug:smartx:CarLifeMedia_CF_48_STEREO
./build/es-synth-host --info
./build/es-synth-host --bench 60
```

`--info` emits JSON: `sample_rate`, and `presets`, each with `id`, `idle`,
`redline`, `cylinders`, and `turbo_default`. Logs use stderr; raw PCM uses stdout.
Raw mode runs at wall-clock speed, ALSA at the device's pace; WAV renders offline.
`--tone` plays three short, softly gated 880 Hz beeps per second for ten seconds
at a conservative level. SIGINT/SIGTERM trigger the normal quit fade.

Timed scripts contain `<seconds> <control line>`, in chronological order:

```
0 E v8
0 V 0.8
0 P 650 0.05 0.03 0 0
0.05 P 660 0.05 0.03 0 0
```

Scripts use the **same stale-data watchdog**, so keep sending P updates at least
every 1.5 seconds. Script scheduling resolves to a 256-frame boundary (5.33 ms).
The offline duration is the last timestamp plus one second; Q fades out and can
end it earlier. Demo duration scales a 40-second sequence: three seconds idle,
two free revs, acceleration through gears 1–5 to 85% redline per gear, overrun
through 2nd, light cruise in 6th, then stop/idle. The final half second fades.

## Control protocol

The shared protocol (`../PROTOCOL.md`), as this engine implements it.
Newline-terminated ASCII on stdin, typically 20–100 Hz:

| Command | Effect |
| --- | --- |
| `P rpm load throttle gear speed_kmh` | Virtual RPM; signed load -1..1; throttle 0..1; gear -1..10; speed km/h |
| `E i6\|v8\|v10\|v12` | 300 ms settling crossfade; selects that engine's turbo default |
| `T 0\|1` | Disable/enable subtle turbo, including on normally NA presets if explicitly requested |
| `V 0..1` | Smooth master level |
| `M 0\|1` | Unmute/mute, approximately 300 ms to 95% of destination |
| `S fromGear toGear` | A gear change of the car's automatic (ZF 8HP). Upshift: crisp 20 % torque reduction (peak at 72 ms, over by 180 ms); downshift: small rev-match blip. No cut, nothing restarts |
| `X name value` | A sound setting, see below |
| `Q` | Fade out, exit after 800 ms |

Malformed/nonfinite values and unknown engine names are ignored; finite P
parameters are clamped to safe ranges. Overlong input lines are discarded.
Only one bounded nonblocking read occurs per block; stdin never waits on a line.
No P for 1.5 seconds fades out, including on initial startup. New data fades back
in. RPM/load/throttle/speed and volume are smoothed every sample; shift
envelopes and source phase never reset on a parameter change. Speed/gear are
retained for the controller, not used to infer RPM: the external daemon owns
that mapping.

**Response.** The sound follows the `P` rpm with about 20 ms of smoothing: two
cascaded 5 ms one-poles, so a telemetry step reaches 90 % in 19.4 ms as a short
rounded ramp, not a click (load and throttle: two 6 ms one-poles, ~23 ms). No
slew limits: a gear change sounds as quick as it is in the car. The output buffer
adds the device's own latency (SmartX: 64 ms).

### Sound settings (`X`)

Values are multipliers, 0..2, 1 = the engine's own default. Changes are smoothed
(~60 ms, no clicks) and kept across `E`. Unknown names, values outside 0..2 and
non-finite values are ignored.

| Name | |
|---|---|
| `offgas` | the level off the throttle (idle, coasting, light load) against the level under load. Linear: 0 silences the off-throttle sound, 2 doubles it; the effect fades out as the load reaches ~0.6, so full load is unchanged |
| `turbo` | turbo whistle / spool level (0 = none; `T 0` also switches it off) |
| `bov` | blow-off / recirculation whoosh when lifting after boost (0 = none): a 1-3 kHz recirculation whoosh plus a short surge flutter |
| `pops` | pops, bangs and crackle on the overrun (0 = none, 1 = subtle, 2 = loud sport exhaust, more often: event rate x pops^2 plus a floor so every engine pops, louder events, a 2x longer window, lower rpm/heat thresholds) |
| `bass` | low end below about 150 Hz (a smooth low shelf at 150 Hz, about ±9 dB at 0 / 2) |

ALSA is loaded using `dlopen("libasound.so.2")` and manually declared opaque
function pointers, including open/set_params/writei/recover/delay/drop/close.
Open is nonblocking. `ES_ALSA_US` requests the buffer (default 64000; accepted
16000-200000); the device's real buffer comes from `snd_pcm_get_params` (SmartX:
3 x 1024 frames). `snd_pcm_set_params` starts playback once the buffer is full, so
the synth keeps it full: it renders a 256-frame block only while `snd_pcm_delay`
leaves room for it, and otherwise sleeps 2 ms. The device's own clock paces the
synth (no drift against the wall clock); the wall clock paces only while the device
is not open. EAGAIN (nobody reads: the AudioManager route not connected) drops the
block. Underruns are recovered; other errors reopen the device, retrying every two
seconds.

## Sound design

The sound path is a C99 re-implementation of the one in
[Engine_Sim](https://github.com/yigitsalihemecen/Engine_Sim) (the browser engine
simulator whose renders were the reference), adapted to telemetry input, BMW
firing geometry and a cabin perspective. Files: `engine.c` (one engine voice),
`synth.c` (protocol, crossfades, master chain), `presets.c`, `dsp.h`.

**Source: one combustion event at a time** (Engine_Sim `combustion-worklet.js`).
A double-precision crank phase advances by rpm/120 per second and is never
reset. Each cylinder has an exhaust event at its firing angle and an intake
event 360 degrees later, each with fixed per-cylinder trims (amplitude, decay,
angle). When the crank passes an event, a pressure pulse
`p(t) = A g(t) (1 - e^-at) e^-bt` (t in cycles) starts with a sub-sample onset
and a 60 us Gaussian attack; `A` carries cycle-to-cycle variation (COV ~1.5-4 %
at full load, ~3x at light load and idle), `a`/`b` harden with load, and the
next event gets timing jitter (0.7-1.7 degrees rms) on a fixed schedule, so
jitter never accumulates. Each firing kicks a crank-speed ripple
(dw/w ~ T(4pi/n)/(J w^2)): FM of every pulse at the firing rate, a few percent at
idle, negligible at high rpm. Per exhaust bank the pulses are summed,
differentiated per unit crank phase (what an open pipe radiates), low-passed at
24 engine orders (radiation knee) and calibrated at init to a fixed RMS over load.
Pulses end with a smooth taper instead of a truncation. Per-sample libm is
avoided: decay terms advance multiplicatively, the onset is a table, filter
coefficients and level maps update every 32 samples.

**Exhaust** (Engine_Sim `resonators.js`): per bank a feedback waveguide
(quarter-wave pipe: negative open-end reflection, wall-loss lowpass in the loop
letting ~1.8 modes x damping survive, delay phase-compensated so the mode lands
on c/4L, gas temperature nudging the delay slightly with load/rpm), a
fractional transit delay, a collector waveguide, a muffler of three feed-forward
chamber combs (delays decorrelated so their peaks do not coincide) plus packing
lowpass, and an asymmetric shock saturator whose drive rises with load^1.5, boost
and overrun (the load rasp). Exhaust flow noise (white, peaked at the bank
firing rate, sawtooth-AM'd at that rate, body lowpass opening with load and
rpm) is injected into the bank pipes, so the turbulence rides on the pulses and
rings in the same pipes. A turbine high shelf darkens the exhaust with spool.
Unlike Web Audio there is no 128-sample loop floor, so every pipe is a true
delay-line waveguide.

**Intake, sub, turbo** (Engine_Sim `resonators.js`, `character.js`,
`layers.js`): fixed-frequency airbox Helmholtz bandpass driven by the intake
events, a direct path, throttle/flow noise whose band moves with flow and a
coast hiss on the overrun; a saturated sub sine at the chest-band order; a
reduced turbo layer (shaft inertia, quiet blade tone kept under its blade-rate
noise band, airflow, rush, whoosh, a short surge flutter on fast closures, own
tone stage).

**Telemetry and transients.** The overrun (load < -0.05 above idle) scales
combustion down and adds coast noise and shaper drive. A lift-off from
sustained load (0.5 s average >= 0.25, once per lift) on a hot engine opens a 0.2-0.7 s crackle
window with a few Poisson events (unburnt fuel lighting in the pipe), injected
upstream of the pipes so they ring through them; V8/V10 crackle most, the V12
barely (`X pops` scales all of it). Shifts are modelled on a ZF 8HP automatic, which shifts without an
audible cut: an `S` upshift is a smooth u^2(1-u)^3 torque reduction starting on
the S line (-20 % load at its peak at 72 ms, over by 180 ms), combustion never stops; an `S` downshift is a small
rev-match blip (+0.2 load, 30 ms rise, 120 ms decay). The rpm comes from the
telemetry (the demo ramps it through each shift like the car does). A throttle
stab gives a short bark, but only from a low-load state, so the load recovering
after a shift never barks; neither a shift dip nor a blip can open a crackle
window, and the turbo flutter needs a real throttle closure. The whole voice follows load and rpm: off the throttle (idle,
coasting, light load) it sits ~7-10 dB under a full-load pull, with a +4 dB
off-throttle lift that `X offgas` scales (demo: idle 6.8-10.4 dB, overrun
7.1-8.7 dB under the pull; full load unchanged).

**Master chain** (Engine_Sim `engine-sim.js`, `fx.js`): infrasonic guard (36 Hz,
4th order), bass enhancer, rumble shelf, 78 Hz body bump, presence/air cuts,
cabin filter at 30 % of Engine_Sim's interior setting (62 Hz boom, 155 Hz
floorpan, panel lowpass ~7.5 kHz, -3.3 dB treble shelf: in the car, not
muffled), a small cabin-sized FDN room (mix 0.10, ~0.15 s; a 0.5 s room had sharp modes
that swelled the engine by 4-5 dB when the firing harmonic crossed one, heard as
an onset in upshift sweeps), the three-band dynamics
(Linkwitz-Riley 220 Hz / 2 kHz, mid band +6.5 dB makeup) and the rational soft
limiter (asymptote 0.89, peaks below -1 dBFS). Voices crossfade at audio rate
(~300 ms) on `E`; idle voices are not computed. Denormals are flushed in all
recursive states and x86 FTZ/DAZ is set.

| Preset | Firing order / angles (deg) | Banks | Idle/redline | Character |
| --- | --- | --- | --- | --- |
| i6 | 1-5-3-6-2-4 / 0:120:600 | 1 | 700/7000 | smooth 3rd order, turbo (B58-like) |
| v8 | 1-5-4-8-6-3-7-2 / 0:90:630 | 1-4, 5-8 | 650/7200 | cross-plane: bank A 0/180/450/630, B 90/270/360/540 = burble, turbo |
| v10 | 1-6-5-10-2-7-3-8-4-9 / 0,90,144,234,288,378,432,522,576,666 | 1-5, 6-10 | 900/8250 | S85 90/54 intervals: strong 2.5 order, hard pulses, NA |
| v12 | 1-7-5-11-3-9-6-12-2-8-4-10 / 0:60:660 | 1-6, 7-12 | 700/7500 | dense smooth pulses, NA |

Pipe lengths, pulse law, damping, muffler chambers and airbox are Engine_Sim's
values for its i6, v8cross, v10 and v12 engines. These are evocative synthetic
presets, not measured BMW engine/exhaust models or recordings.

**Measured against the Engine_Sim reference MP3s** (same rpm/throttle traces
replayed through this engine):
spectral centroid i6-turbo 197 Hz (ref 254), v8 182 (214), v10 365 (357), v12 376
(354); share of energy below 300 Hz 97/78/56/51 % (ref 93/69/65/66 %);
cycle-to-cycle energy variation 12-22 % (ref 10-21 %). It is about 3 dB
darker than the references in 1-3 kHz (cabin filter, and no gearbox-whine layer)
and its turbo has much less 3-8 kHz air. Bench on an Apple M-series core:
163-198x realtime per preset.

The build and automated spectral checks do **not** establish subjective sound
quality, actual Apollo Lake CPU usage, hardware routing, or SmartX latency.
Those require on-car audition and measurement. Start with low vehicle volume.

## Credits and license notice

The sound path re-implements, in C, the design and constants of
[Engine_Sim](https://github.com/yigitsalihemecen/Engine_Sim) by Yigit Salih
Emecen (MIT): `src/combustion-worklet.js` and `pulse.js` (event source,
variation, crank ripple, calibration), `resonators.js` (waveguide, muffler,
intake, nonlinearity, cabin), `character.js` (exhaust noise, sub layer),
`layers.js` (turbo levels), `fx.js` and `engine-sim.js` (tone stage, dynamics,
bass enhancer), `profiles.js` (pulse law, pipes, airbox and level of its i6,
v8cross, v10 and v12). Ideas were also taken from
[DasEtwas/enginesound](https://github.com/DasEtwas/enginesound) (MIT; valve and
waveguide engine model after Baldan et al. 2015),
[Antonio-R1/engine-sound-generator](https://github.com/Antonio-R1/engine-sound-generator)
(MIT; same model) and [ange-yaghi/engine-sim](https://github.com/ange-yaghi/engine-sim)
(MIT; noise multiplied onto the pressure signal rather than added as a bed).
No code was copied from those three. BMW firing geometry (S85 90/54-degree V10,
cross-plane V8 bank angles), the telemetry mapping, crackle window, cabin level
curve and the C structure are original.

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
