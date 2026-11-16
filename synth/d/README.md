# es-synth, engine D: active sound design for the car's own diesel

Engine D does what the factory systems do (BMW Active Sound Design in the X7 / X5 xDrive40d with the B57
straight-six diesel, the Audi S6 / S7 TDI and SQ7 TDI sound actuator / active exhaust). It does **not** simulate a
foreign petrol engine. It **adds rpm-locked engine orders** to what the cabin already hears from the 320d's own B47.
It is driven live by telemetry: real rpm, crank-torque load, pedal, gear and speed. The aim is a bassy, burbly,
natural sound, "like the native solution".

C99, no heap allocation in the audio path, no samples, no extra thread. Output is 48 kHz S16 stereo (identical
L/R). The last stage is the shared speaker protection (`protect.h`, a copy of `synth/common/protect.h`), as in
engines A and B. `main.c` and `out_alsa.c/.h` are engine A's, unchanged.

**Note for listening to the samples:** `samples/*.wav` contain only the *added* sound. In the car the B47's own
sound is underneath (its order 2, combustion clatter, tyre and wind noise), so the renders will sound thinner and
more "tonal" than the result in the cabin.

## Build and test

```
make host            # build/es-synth-host
./test.sh            # internal checks, renders samples/, spectral checks, benchmark
make car             # zig cc -target x86_64-linux-gnu.2.27, loader /lib/ld-linux-x86-64.so.2 -> ../tool/lfb-enginesound/es-synth
./render_all.sh      # samples/{i6,v8,v10,v12}.wav: 40 s mono demo drives
```

The command line and the stdin protocol are those of `PROTOCOL.md` (`P/E/T/V/M/S/X/Q`, `--out alsa:|wav:|raw`,
`--demo`, `--script`, `--tone`, `--info`, `--bench`). Differences for D:

* `P <rpm> ...` carries the B47's **real** rpm (idle about 800, max about 4800-5000). The app does not map it.
  `--info` reports idle 800 / redline 5000 for every preset, with cylinders 6 / 8 / 6 / 6.
* `S` (a ZF 8HP shift) changes nothing in the sound. The shift is already in the telemetry rpm and torque. It
  only holds off the lift-off detector for 0.4 s, so a shift's torque dip never fires overrun pops.

## Design

```
telemetry rpm -> prediction along a consistent ramp -> 2 x 6 ms one-pole ---------> psi (720-degree cycle phase)
                                                                                    |
gain tables (rpm x load, dB) -> 12 ms up / 50 ms down -> order bank  sum_k g_k sin(k psi + phi_k), k = 1..24
                                                          |  half orders x slow random "lump" (burble)
                                                          |  x per-firing amplitude (bank / cylinder / random)
(trace of low-passed pulse noise, load > 0.35 only) ------+
                                                          v
                                    28 Hz HP -> bass shelf 120 Hz -> cabin LP (400-860 Hz by load)
                                    + (T 1) tonal turbo whistle + recirculation sigh + overrun thumps
                                                          -> volume, gate, soft ceiling -3 dBFS -> protect.h
```

* **Phase.** A single phase `psi` covers one 720-degree cycle (two crank revolutions). It is integrated in double
  precision from the smoothed rpm and never reset. Harmonic `k` of it is engine order `k/2`. Half orders (1.5,
  4.5, 7.5) therefore have no wrap discontinuity, and preset changes, shifts and mute never reset the phase. A
  phasor recurrence gives all harmonics per sample (one complex multiply each). It is resynchronised to
  `cos/sin(psi)` every 32 samples.
* **Rpm response.** There is no slew limit. The telemetry value goes through two cascaded 6 ms one-poles: a rounded
  step, 90 % after 23 ms. Along a *consistent* ramp (the last two telemetry intervals agree in slope) the target
  is extrapolated by at most one telemetry interval (at most 50 ms, slope capped at 15000 rpm/s). With 50 Hz
  telemetry, a ZF upshift ramp is followed without a staircase. A step is never extrapolated.
* **Order tables** (`presets.c`). Each order has a dB value at full positive load at 800 / 1600 / 2800 / 4300 rpm,
  plus an off-gas offset. These are interpolated linearly in dB over rpm and load
  (`w = clamp(load,0,1)^0.7`). On top come a common rpm trim (`RPM_TRIM`: the low band is held back for the bass
  limit, and the sound grows towards the top like an ASD) and an overrun trim (integer orders -0.5..-1 dB, half
  orders +1..+2.5 dB for the V presets: the overrun burble). Any order under 40..30 Hz fades out (no infrasound: at
  800 rpm order 1.5 is 20 Hz and order 3, at 40 Hz, is the lowest left). Engine off (rpm < 300-600) is silent.
* **Blend with the B47.** The B47's own firing order is 2. The six-cylinder presets add **no** order 2. They lean on
  3 / 6 / 9 (i6) or on 1.5 / 3 / 4.5 / 7.5 (V6). The V8 adds order 2 only about 24 dB below its main order 4, as
  the report advises (an unsynchronised order 2 would beat against the real one).
* **Naturalness, not an organ:**
  - Per-firing amplitude irregularity. Each firing event (6 or 8 per cycle) draws a new amplitude: a bank pattern
    (V6: alternating banks, V8 1-5-4-8-6-3-7-2 = A B A B B A B A), a static per-cylinder spread, and a random
    spread of 4.5-6 %, larger at light load and on the overrun. It is smoothed over about a third of a firing
    interval. This puts correlated sidebands on the half-order grid.
  - A slow random "lump" on the half orders: 4 Hz, about 10-30 % depth, deeper off the gas and on the overrun,
    smaller above 2500 rpm. This is the V6 TDI burble.
  - A small common phase jitter (about 0.01 shaft rad, about 10 Hz wide, scaled by order).
  - Only a trace of noise. Noise rides on the firing pulses only under real load (it fades in from load 0.35 to
    0.9). It is band-passed at 150-450 Hz and low-passed at 450 Hz before the cabin low-pass, 60-90 dB under the
    orders. It is **exactly zero** at idle, cruise and coasting. Version 1 had about 22 dB more of it, band-passed
    up to 900 Hz and always on; in the car that was heard as a permanent hiss.
  - Load brightness. The higher orders gain more with load than order 3, and the cabin low-pass opens from about
    400 to about 800 Hz. At 2500 rpm the energy share above 350 Hz rises from load 0.05 to 1 (i6 0.1 % -> 0.6 %;
    the added sound is deliberately bass-heavy, the B47 supplies the mids).
* **Cabin.** A 28 Hz high-pass, a 120 Hz bass shelf (preset +2..+3.5 dB, `X bass` +-9 dB) and the load-dependent
  low-pass. Gears 7 / 8 at low load get -2 dB (cruise). Idle is 3 dB quieter than the rest of the off-gas range.
* **Boost proxy.** There is no boost telemetry. The proxy is `sstep(0.6, 0.95, load) x sstep(1600, 2600 rpm)`
  (500 ms up, 300 ms down), plus the time spent near full load (load > 0.8, > 1700 rpm).
* **Turbo** is **off in D's own default**: `turbo_default` is false, and `E` switches it off. With `T 1` it is a
  faint *tonal* whistle (a sine at 2.3-3.8 kHz with a slow ±0.4 % wander, no noise band). It fades in only at high
  boost (proxy 0.55..0.95) and sits about 35-38 dB under the engine at full load. It is zero at idle, cruise and
  coasting.
* **Recirculation sigh** (`X bov`, independent of `T`). It fires only on a genuine quick lift after real boost: at
  least 0.5 s near full load, a pedal peak > 70 % within the last ~150 ms, the pedal now < 10 % and torque gone. It
  is a band of noise falling from 1.1 to 0.6 kHz, low-passed at 1.2 kHz. It rises in 15 ms, decays with a 60 ms
  time constant (about 200 ms audible) and peaks about 24-28 dB under the full-load engine. It has a 3 s cool-down.
* **Overrun burble / crackle** (`pops`, Audi S-model style, gentle). Sustained positive load arms it. A genuine
  lift fires it: pedal < 5 %, torque negative for 60 ms, > 1300 rpm, > 10 km/h, not within 0.4 s of a shift. It
  opens a window of 0.6-1.3 s. During the window, events fall on firing slots at a decaying rate (6 per second x
  pops scale), at least 90 ms apart. Each event is a damped 85-155 Hz sine thump (3 ms rise, ~28 ms decay) with a
  little amplitude roughness, low-passed at 700 Hz. There is no crackle band any more. Nothing fires during a continuous descent, a shift or a torque oscillation (re-arming
  needs load > 0.25 again).
* **Output.** Volume (50 ms), gate (mute, Q, no `P` for 1.5 s; 40 ms fade), preset crossfade (80 ms; the order bank
  is shared, so only the gains move) and a soft ceiling at -3 dBFS. Then comes `protect.h` (30 Hz HP, bass
  limiter -8 dBFS, power limiter, peak limiter).

`X` settings (0..2, 1 = default, smoothed 50 ms, landing exactly on 0):

| X | effect |
|---|---|
| `offgas` | scales the off-gas end of the tables (`20 log10(offgas)` dB): idle, light cruise and coasting. Full load is unchanged |
| `turbo` | the tonal whistle's level when `T 1` (0 = none; D's default is `T 0`) |
| `bov` | recirculation sigh level (0 = none; only after real boost, see above) |
| `pops` | overrun burble rate, window length and level (0 = none, 2 = more often and louder) |
| `bass` | 120 Hz shelf, `9 (bass - 1)` dB on top of the preset's own |

## Presets

The ids stay the ones the app sends. Gains are in dB relative to the common reference, at full load, at
800 / 1600 / 2800 / 4300 rpm (before the common `RPM_TRIM` of -1 / -1.2 / +0.8 / +5 dB). "off" is the offset at
zero load, before the preset's `off_trim`. The cells start from the research report's proposal and were tuned by
measurement and spectrogram. **They are design targets, not recovered factory calibrations.**

**`i6`: BMW B57 straight-six diesel ASD (X7 40d).** Smooth, deep, refined; order 3 carries it.

| order | 1 | 1.5 | 3 | 4.5 | 6 | 7.5 | 9 | 12 |
|---|---|---|---|---|---|---|---|---|
| full | -30/-29/-31/-36 | -27/-24/-26/-32 | -2/0/-1/-4 | -29/-26/-26/-30 | -13/-10/-9/-11 | -37/-34/-32/-36 | -29/-24/-21/-24 | -36/-30/-27/-30 |
| off | -3/-3/-4/-4 | -5/-6/-6/-6 | -6 | -8/-8/-9/-9 | -9/-10/-10/-11 | -11/-12/-12/-12 | -12/-13/-14/-14 | -14/-15/-16/-16 |

off_trim -1.5, random per firing 4.5 %, no bank pattern, half-order lump 0.10, pops 0.5, bass +2 dB, cabin LP
430-820 Hz.

**`v8`: Audi 4.0 V8 TDI (SQ7).** Cross-plane V8, order 4 main, deep 1 / 3 rumble from the uneven per-bank firing.

| order | 1 | 1.5 | 2 | 2.5 | 3 | 4 | 5 | 6 | 8 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|
| full | -20/-22/-26/-32 | -31/-29/-31/-36 | -26/-24/-27/-34 | -32/-30/-31/-36 | -19/-18/-20/-26 | -3/0/-1/-5 | -30/-27/-27/-31 | -20/-15/-15/-20 | -23/-15/-13/-17 | -36/-30/-28/-31 |
| off | -2/-3/-3/-3 | -3 | -3/-4/-4/-4 | -4 | -4/-4/-5/-5 | -6 | -8 | -9/-10/-10/-10 | -11/-12/-13/-13 | -13/-14/-15/-15 |

off_trim -1.5, bank pattern 5 %, random 5 %, lump 0.15, overrun half orders +1 dB, pops 0.8, bass +3 dB.

**`v10`: Audi 3.0 V6 TDI (S6 / S7) burble, the owner's main wish.** Two banks of three (firing 1-4-3-6-2-5) give the
1.5 / 4.5 / 7.5 half orders on top of order 3. They stay up off the gas and grow on the overrun.

| order | 1 | 1.5 | 3 | 4.5 | 6 | 7.5 | 9 | 10.5 |
|---|---|---|---|---|---|---|---|---|
| full | -26/-27/-31/-36 | -12/-9/-11/-19 | -3/0/-1/-5 | -11/-10/-12/-21 | -16/-12/-11/-15 | -22/-20/-22/-29 | -32/-27/-25/-29 | -34/-31/-31/-36 |
| off | -3/-3/-4/-4 | -3/-4/-5/-5 | -6 | -4/-5/-6/-6 | -9/-10/-10/-11 | -7/-8/-9/-9 | -12/-13/-14/-14 | -12/-13/-14/-14 |

off_trim -2, bank 4 %, random 5.5 %, lump 0.28, overrun half orders +2.5 dB, pops 1, bass +3 dB.

**`v12`: the same V6 TDI, "sport / dynamic" calibration.** More half orders and growl, less off-gas reduction,
deeper lump, brighter under load, more overrun burble.

| order | 1 | 1.5 | 3 | 4.5 | 6 | 7.5 | 9 | 10.5 |
|---|---|---|---|---|---|---|---|---|
| full | -25/-25/-29/-34 | -10/-7/-8/-16 | -2/0/0/-3 | -9/-8/-9/-18 | -15/-10/-9/-12 | -20/-17/-18/-25 | -30/-24/-21/-25 | -32/-28/-27/-32 |
| off | -3 | -2/-3/-4/-4 | -5 | -3/-4/-5/-5 | -8/-9/-9/-10 | -6/-7/-8/-8 | -11/-12/-13/-13 | -11/-12/-13/-13 |

off_trim -3, bank 5 %, random 6 %, lump 0.32, overrun half orders +2.5 dB, pops 1.5, bass +3.5 dB, cabin LP 430-860 Hz,
output trim -2 dB.

## Measurements (test.sh, this host)

| | i6 | v8 | v10 | v12 |
|---|---|---|---|---|
| demo RMS / peak, dBFS | -19.2 / -9.8 | -19.4 / -9.0 | -19.2 / -9.4 | -19.0 / -9.2 |
| demo peak under 100 Hz (protection's detector), dBFS | -13.4 | -13.7 | -11.8 | -11.6 |
| energy under 28 Hz vs. total | -44 dB | -49 dB | -43 dB | -43 dB |
| worst order frequency error (main orders, 1500 / 2600 / 4000 rpm) | 0.031 % | 0.077 % | 0.073 % | 0.041 % |
| 2500 rpm, load 1 / 0.5 / 0.05 / 0 / -0.5, dBFS | -15.9/-19.0/-22.8/-23.8/-24.3 | -15.7/-18.8/-22.7/-23.8/-24.3 | -15.1/-18.3/-22.2/-23.2/-22.9 | -15.3/-18.5/-22.2/-23.2/-22.4 |
| off-gas / coasting under full load | 8.0 / 8.4 dB | 8.1 / 8.6 dB | 8.0 / 7.8 dB | 7.9 / 7.1 dB |
| `--bench 60`, x realtime (varies run to run) | 629 | 699 | 696 | 681 |

* Off-gas against full load is 7.6-9.0 dB from 1500 to 4300 rpm in every preset (idle about 10.5 dB).
* Rpm step 1500 -> 3000: 90 % after 23.3 ms, no overshoot. A 50 Hz telemetry ramp of -4000 rpm/s (ZF upshift):
  69 rpm tracking error (17 ms) and 4 rpm undershoot when the ramp stops.
* Gear changes: the same drives with and without `S` lines (full-load upshift, overrun upshift, coasting
  downshift) differ by 0.00 dB. The five demo upshifts stay within 3 dB of the levels around them.
* `X`: with `T 1` the whistle raises the 2-7 kHz band by 50 dB at full load. The default (`T 0`) and `X turbo 0`
  leave nothing there. After a lift, the non-tonal 0.3-1.5 kHz band rises about 4 dB with the sigh, and the
  non-tonal 60-700 Hz band rises about 4 dB with pops 2. `bass` 0 / 1 / 2 moves the low-vs-mid balance by about 6 dB per step. `offgas
  0.5` gives -6.0 dB off the gas and 0.0 dB at full load. `X pops/bov/turbo 0` remove those layers exactly, and `T 0`
  removes the whistle exactly (test_core).
* Quiet bands. These are measured at idle (800 rpm), cruise (2000 rpm, load 0.2) and coasting (2500 rpm, load
  -0.5), with every layer at its maximum (`T 1`, `X turbo/bov/pops 2`). Energy above 1.5 kHz is 71-79 dB under the
  total; check.py requires at least 35. Non-tonal energy above 500 Hz is 57-61 dB under the total; check.py requires
  at least 50. test_core asserts that the pulse noise, whistle, sigh and pops are exactly 0 there.
* Clicks: A's slope-outlier detector finds none in the samples, the transition script (preset switching every
  50 ms, all `X` changes, mute, shifts, malformed lines) or the lift renders.

### Noise and hiss: before / after (version 1 -> version 2, the owner heard "a lot of hiss")

These come from layer-isolated builds (`cc -DES_LAYERS=<mask>`: 1 pulse noise, 2 turbo, 4 sigh, 8 pops, 16
jitter, 32 orders). Each value is the layer's added non-tonal energy (or its energy above 1.5 kHz), in dB under the
total. The ranges cover all four presets, with the defaults of each version.

| layer | version 1 (defaults) | version 2 (defaults) |
|---|---|---|
| turbo whistle (noise band 1.9-4.5 kHz + hiss) | cruise -45..-49 dB, full load -30..-33 dB (all of it above 1.5 kHz) | off (`T 0`). With `T 1`: a sine, -35..-38 dB at full load only, nothing at cruise / coast / idle |
| pulse noise | -41..-47 dB at every load point (-43..-54 dB above 500 Hz), including idle and cruise | 0 at idle / cruise / coast; at full load at least -68 dB (-73..-94 dB above 500 Hz) |
| recirculation sigh | after any lift with spool > 0.35, ~0.3 s, up to 1.75 kHz, -39..-41 dB in the lift window | after ≥ 0.5 s near full load only, ~200 ms, < 1.2 kHz, 24-28 dB under the engine |
| pops | noise thump + 1.3 kHz crackle, -14..-29 dB in the lift window, up to -31 dB above 1.5 kHz | sine thump, -22..-29 dB in the lift window, -64..-75 dB above 1.5 kHz |
| phase jitter | ~70 Hz wide: -27..-34 dB of skirts around the lines | ~10 Hz wide: -28..-46 dB of low skirts, -42..-62 dB above 500 Hz |
| **total above 1.5 kHz**: idle / cruise / coast / full load | -69..-71 / -45..-49 / -74..-75 / -30..-33 dB | -71..-73 / -78..-79 / -75..-77 / -78..-81 dB |

What is left of the "non-tonal" energy (-17..-34 dB) is the burble itself: the per-firing irregularity and the
half-order lump. It lies around the order lines below 500 Hz and is not hiss.

## Sources

The research report `research4/REPORT.md` (16 Nov 2026) and the sources it cites:

* BMW technical information on ASD (actual engine sound supplemented with DSP-generated elements from vehicle data):
  press.bmwgroup.com T0054613EN p.36; BMW training ST1204 (F10 ASD), ST1856, ST1915 (ASD in the RAM, coupled to
  the driving modes); the BMW AIR functional description (rpm, speed, pedal, load and gear as inputs).
* Harman HALOsonic ESS and patent US11368785B2 (order channels with rpm/torque-mapped gain); Bose Active Sound
  Management / EHE and patents US9495953B2, US9812113B2 (harmonics as shaft frequency x order).
* Audi SSP603 pp.20-21 (active exhaust from rpm, load, speed: mapped harmonics; drive select) and SSP920223 p.84
  (structure-borne sound actuator); the Audi SQ7 TDI announcement (4.0 V8, not a V6).
* Pommerer, Koch, Krüger, AIA-DAGA 2013 (diesel exhaust sound shaped towards a sporty character).
* RWTH diesel noise study §2.2 (1.5 / 3 / 4.5 main-order family of a three-cylinder bank); Kim et al., SAE
  2017-01-1756 (six-cylinder order arrangements, booming / rumbling); SAE 2019-01-1416 (half-order modulation as
  diesel combustion noise: kept moderate here); Applied Sciences 12(9) 4486 (uncertain secondary paths).
* Open implementations inspected for structure only, with no code copied: ZhenxianLi/rota-order (GPL-3.0: rpm
  integration to shaft phase, order gains with AM/FM and noise) and rdoerfler/engine-order-analysis (order
  extraction, a way to measure future maps).

## Weaknesses

* No measured factory data. The ASD / Audi maps are not public, so every gain is a tuned design target and not a
  recovered calibration. Nobody has listened to it in the car yet. The real cabin result depends on the B47's own
  sound, the speakers and the head unit's 64 ms output latency.
* No phase lock to the real crank. The added orders cannot reinforce or cancel the B47 coherently. The V8's small
  order 2 may beat slowly against the real one; it can be set to 0 in `presets.c`.
* Every non-order layer is now nearly silent. If the cabin sounds too "clean" or organ-like, raise `am_rand` /
  `wobble` in `presets.c` (irregularity and burble), not the noise.
* Turbo whistle, sigh and pops are stylised approximations (no boost or turbo-speed telemetry). The B47's fuel-cut
  overrun does not really crackle; the pops are an Audi-S-style effect, gentle by default.
* Load is crank torque only. The off-gas / full-load balance (about 8 dB) assumes the app's load scale matches the
  test scale.
* Telemetry slower than about 20 Hz leaves rpm steps up to 50 ms old. The prediction only helps on steady ramps.
