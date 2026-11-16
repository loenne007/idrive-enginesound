# Enhance sound engine (`es-synth-enhance`): active sound design for the car's own diesel

This engine does what the factory systems do (BMW Active Sound Design in the X7 / X5 xDrive40d with the B57
straight-six diesel, the Audi S6 / S7 TDI and SQ7 TDI sound actuator / active exhaust). It does **not** simulate a
foreign petrol engine. It **adds rpm-locked engine orders** to what the cabin already hears from the 320d's own B47.
It is driven live by telemetry: real rpm, crank-torque load, pedal, gear and speed. It is first of all a **bass
enhancer**: not another engine, but low, rpm-locked depth under the real diesel: 30-150 Hz, with only a little up to about 250 Hz for definition on
small speakers. The real B47 supplies the mids and highs.

C99, no heap allocation in the audio path, no samples, no extra thread. Output is 48 kHz S16 stereo (identical
L/R). The last stage is the shared speaker protection (`../common/protect.h`), as in every engine. `main.c` and
`out_alsa.c/.h` are the Waveguide engine's, unchanged.

**Note for listening to the samples:** `samples/*.wav` contain only the *added* sound. In the car the B47's own
sound is underneath (its order 2, combustion clatter, tyre and wind noise), so the renders will sound thinner and
more "tonal" than the result in the cabin.

## Build and test

```
make host            # build/es-synth-host
make test            # internal checks, renders samples/, spectral checks, benchmark
make car             # zig cc -target x86_64-linux-gnu.2.27, loader /lib/ld-linux-x86-64.so.2 -> ../../tool/lfb-enginesound/es-synth-enhance
make samples         # samples/{i6,v8,v10,v12}.wav: 40 s mono demo drives
```

The command line and the stdin protocol are those of `PROTOCOL.md` (`P/E/T/V/M/S/X/Q`, `--out alsa:|wav:|raw`,
`--demo`, `--script`, `--tone`, `--info`, `--bench`). Differences for this engine:

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
  (each order x 3rd-order roll-off at 140-200 Hz)  34 Hz HP (4th) -> bass shelf 120 Hz -> LP 2.2 x roll-off
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
  limit, and the sound grows towards the top like an ASD) and an overrun trim (integer orders 0..-1 dB, half
  orders +1..+2.5 dB for the V presets: the overrun burble).
  **Off-gas sits about 4 dB under full load** (idle, light cruise, coasting: 3.1-5.7 dB over 800-4300 rpm).
* **Lift-off glide.** The load that sets the level rises quickly (15 ms, about 50-70 ms to full level with the
  envelopes). It falls as a glide at 2 load-units per second, rounded with 60 ms, so a full lift takes about
  0.5 s. The overrun trim glides in the same way. A lift sounds like going into overrun, not like a fade-out:
  100 ms after it the level is within 0.5 dB of before, and after 300 ms it is down 1-3 dB. The pops, sigh and
  turbo detectors still use the fast load.
* **Bass enhancer.** Every order's gain is weighted by the magnitude of a 3rd-order low-pass at the preset's corner
  (`lp_lo` / `lp_hi`: 140 Hz off the gas, 185-200 Hz at full load). This is exact per order, with no filter phase,
  and comes on top of the tables. The tables give the lowest orders the weight: 1.5 and 3 for the six-cylinder
  presets, with order 1 joining from about 2000 rpm, and 1 / 3 / 4 for the V8. The roll-off moves the energy down
  as the rpm rises. So it is mostly 30-150 Hz at any rpm, spread over 2-3 orders, with the burble and irregularity
  on them. A 4th-order 34 Hz high-pass and a 2nd-order low-pass at 2.2x the corner follow (the 34 Hz high-pass also
  removes the 20 Hz sidebands that the per-firing modulation puts under order 3 at idle). Any order under 42..32 Hz
  fades out: at 800 rpm order 1.5 is 20 Hz and order 1 is 13 Hz, so order 3, at 40 Hz, is the lowest left. Engine
  off (rpm < 300-600) is silent.
* **Inside the speaker protection, not leaning on it.** At defaults the band under 100 Hz peaks at -10..-11 dBFS
  (limit -8) and the 3 s RMS stays at or under -15 dBFS (limit -14). The demo RMS is -17.7..-18.3 dBFS.
* **Blend with the B47.** The B47's own firing order is 2. The six-cylinder presets add **no** order 2. The V8 adds
  order 2 only about 25 dB under its orders 1 / 3 / 4, because an unsynchronised order 2 would beat against the
  real one.
* **Naturalness, not an organ:**
  - Per-firing amplitude irregularity. Each firing event (6 or 8 per cycle) draws a new amplitude: a bank pattern
    (V6: alternating banks, V8 1-5-4-8-6-3-7-2 = A B A B B A B A), a static per-cylinder spread, and a random
    spread of 4.5-6 %, larger at light load and on the overrun. It is smoothed over about a third of a firing
    interval. This puts correlated sidebands on the half-order grid.
  - A slow random "lump" on the half orders: 4 Hz, about 10-30 % depth, deeper off the gas and on the overrun,
    smaller above 2500 rpm. This is the V6 TDI burble.
  - A small common phase jitter (about 0.01 shaft rad, about 10 Hz wide, scaled by order).
  - Only a trace of noise. Noise rides on the firing pulses only under real load (it fades in from load 0.35 to
    0.9). It is band-passed at 80-250 Hz and low-passed at 200 Hz, 60-90 dB under the orders. It is **exactly zero** at idle, cruise and coasting (no permanent hiss).
  - Mild load definition. The roll-off corner moves from 140 to 185-200 Hz with load. At 2500 rpm the 120-400 Hz
    share rises from load 0.05 to 1 by 7-14 points (for example 49 % -> 57 %).
* **Cabin.** A 120 Hz bass shelf (preset +1..+2.5 dB, `X bass` +-9 dB). Gears 7 / 8 at low load get -2 dB (cruise). Idle is 1 dB quieter than the rest of the off-gas range.
* **Boost proxy.** There is no boost telemetry. The proxy is `sstep(0.6, 0.95, load) x sstep(1600, 2600 rpm)`
  (500 ms up, 300 ms down), plus the time spent near full load (load > 0.8, > 1700 rpm).
* **Turbo** is **off in this engine's own default**: `turbo_default` is false, and `E` switches it off. With `T 1` it is a
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
  little amplitude roughness, low-passed at 300 Hz, no crackle band. Nothing fires during a continuous descent, a shift or a torque oscillation (re-arming
  needs load > 0.25 again).
* **Output.** Volume (50 ms), gate (mute, Q, no `P` for 1.5 s; 40 ms fade), preset crossfade (80 ms; the order bank
  is shared, so only the gains move) and a soft ceiling at -3 dBFS. Then comes `protect.h` (30 Hz HP, bass
  limiter, power limiter, peak limiter).

`X` settings (0..2, 1 = default, smoothed 50 ms, landing exactly on 0):

| X | effect |
|---|---|
| `offgas` | the off-gas end of the tables (idle, light cruise, coasting): 0..1 scales it (`20 log10(offgas)` dB, 0 = silent), 1..2 moves it towards the full-load table (2 = as loud as full load). Full load is unchanged |
| `turbo` | the tonal whistle's level when `T 1` (0 = none; this engine's default is `T 0`) |
| `bov` | recirculation sigh level (0 = none; only after real boost, see above) |
| `pops` | overrun burble rate, window length and level (0 = none, 2 = more often and louder) |
| `bass` | 120 Hz shelf, `9 (bass - 1)` dB on top of the preset's own. `X bass 2` drives the band under 100 Hz into the protection's bass limiter (i6 / v10 / v12; v8 stays at about -10 dBFS) |

## Presets

The ids stay the ones the app sends. Gains are in dB relative to the common reference, at full load, at
800 / 1600 / 2800 / 4300 rpm, before the common `RPM_TRIM` of 0 / 0 / +1 / +2.5 dB and before the per-order roll-off.
At zero load every order is 4 dB lower, then the preset's `off_trim` is added (off-gas is about 4 dB under full
load). These are design targets, not recovered factory calibrations.

| order | `i6` B57 ASD | `v8` SQ7 4.0 V8 TDI | `v10` S6/S7 3.0 V6 TDI | `v12` V6 TDI sport |
|---|---|---|---|---|
| 1 | -14/-10/-6/-4 | -10/-6/-3/-3 | -12/-9/-6/-5 | -11/-8/-5/-4 |
| 1.5 | -10/-6/-4/-4 | -20/-18/-18/-20 | -4/-2/-2/-3 | -3/-1/-1/-2 |
| 2 | - | -26/-24/-26/-30 | - | - |
| 2.5 | - | -20/-18/-18/-20 | - | - |
| 3 | 0/0/-1/-2 | -6/-5/-5/-6 | -2/0/-1/-2 | -2/0/0/-1 |
| 4 | - | -1/-1/-2/-3 | - | - |
| 4.5 | -24/-22/-22/-24 | - | -9/-8/-9/-12 | -7/-6/-7/-10 |
| 6 | -12/-11/-11/-13 | -16/-14/-14/-16 | -15/-13/-13/-15 | -14/-12/-11/-13 |
| 7.5 | - | - | -22/-21/-22/-24 | -20/-19/-20/-22 |
| roll-off corner off / full load | 140 / 185 Hz | 140 / 185 Hz | 140 / 185 Hz | 145 / 200 Hz |
| off_trim, overrun half orders | +0.5, 0 dB | +0.7, 0 dB | 0, +1 dB | 0, +1 dB |
| bank / random per firing / lump | - / 4.5 % / 0.10 | 5 % / 5 % / 0.15 | 4 % / 5.5 % / 0.28 | 5 % / 6 % / 0.32 |
| bass shelf, pops, output trim | +1 dB, 0.5, +0.8 dB | +2 dB, 0.8, +0.8 dB | +2 dB, 1, -0.4 dB | +2.5 dB, 1.5, -2.5 dB |

* `i6`: smooth and deep. Order 3 carries it, 1.5 and (higher up) 1 add depth underneath, and a little 6 adds
  definition.
* `v8`: orders 4 / 3 / 1, the deep rumble of the uneven per-bank firing.
* `v10`: the burble. Order 1.5 is nearly as strong as 3, with 4.5 under them, all lumping
  slowly. It grows on the overrun.
* `v12`: the same V6 TDI with more half orders, a deeper lump, a little more definition under load, and more pops.

## Measurements (make test, this host)

| | i6 | v8 | v10 | v12 |
|---|---|---|---|---|
| demo RMS / peak, dBFS | -17.7 / -7.6 | -18.3 / -7.1 | -17.7 / -7.6 | -18.3 / -7.9 |
| demo peak under 100 Hz (protection's detector, limit -8), dBFS | -10.8 | -10.3 | -10.1 | -9.7 |
| demo 3 s RMS max (power limiter -14), dBFS | -15.4 | -16.1 | -15.4 | -16.1 |
| demo energy 30-60 / 60-120 / 120-250 / >250 Hz | 22 / 56 / 22 / 0.1 % | 31 / 32 / 37 / 0.5 % | 30 / 53 / 17 / 0.1 % | 31 / 51 / 18 / 0.2 % |
| energy under 30 Hz, demo / idle | -55 / -45 dB | -54 / -58 dB | -53 / -44 dB | -51 / -44 dB |
| worst order frequency error (main orders, 1500 / 2600 / 4000 rpm) | 0.059 % | 0.034 % | 0.061 % | 0.055 % |
| 2500 rpm, load 1 / 0.5 / 0.05 / 0 / -0.5, dBFS | -15.5/-17.1/-19.2/-19.8/-19.7 | -15.9/-17.7/-20.1/-20.7/-20.7 | -14.9/-16.6/-18.7/-19.3/-18.8 | -15.6/-17.4/-19.3/-19.9/-19.3 |
| off-gas / coasting under full load | 4.3 / 4.2 dB | 4.8 / 4.8 dB | 4.4 / 3.9 dB | 4.3 / 3.8 dB |
| lift at 2500 rpm: after 100 ms / 300 ms / settled | +0.2 / -1.4 / -4.5 dB | -0.5 / -1.0 / -4.9 dB | +0.1 / -1.7 / -4.7 dB | +0.3 / -4.1 / -4.9 dB |
| 2000 rpm full load, under 100 Hz peak: bass 1 / bass 2 | -11.3 / -7.7 dBFS (limits) | -15.4 / -9.8 dBFS | -9.0 / -7.6 dBFS (limits) | -10.0 / -7.4 dBFS (limits) |
| `--bench 60`, x realtime (varies run to run) | 860 | 823 | 763 | 764 |

Energy shares of the demo drive (whole 40 s) for 30-60 / 60-120 / 120-250 / >250 Hz are in the table above; the V8
keeps the most 120-250 Hz, because its order 4 is 133 Hz at 2000 rpm. For v10: 2000 rpm / load 0.2
48 / 46 / 5 / 0 %, 3500 rpm full load 27 / 49 / 24 / 0.6 %, coasting at 2500 rpm 9 / 69 / 22 / 0 %.

* Off-gas against full load is about 4 dB (3-5.5 dB from 800 to 4300 rpm) in every preset.
  check.py requires 3-5 dB at 2500 rpm, a lift within 2 dB after 100 ms, and a tip-in within 100 ms.
* Rpm step 1500 -> 3000: 90 % after 23.3 ms, no overshoot. A 50 Hz telemetry ramp of -4000 rpm/s (ZF upshift):
  69 rpm tracking error (17 ms) and 4 rpm undershoot when the ramp stops.
* Gear changes: the same drives with and without `S` lines (full-load upshift, overrun upshift, coasting
  downshift) differ by 0.00 dB. The five demo upshifts stay within 3 dB of the levels around them.
* `X`: with `T 1` the whistle raises the 2-7 kHz band by 50 dB at full load. The default (`T 0`) and `X turbo 0`
  leave nothing there. After a lift, the non-tonal 0.3-1.5 kHz band in the first 170 ms rises about 25 dB with the sigh (this engine has
  almost nothing there otherwise). The non-tonal 60-700 Hz band over 0.68 s rises 1.5 dB with pops 2. `bass` 0 / 1
  / 2 moves the 30-80 Hz vs 150-400 Hz balance by about 6-7 dB per step. `offgas
  0.5` gives -6.0 dB off the gas and 0.0 dB at full load; `offgas 2` makes off-gas as loud as full load (-0.1..-0.2 dB). `X pops/bov/turbo 0` remove those layers exactly, and `T 0`
  removes the whistle exactly (test_core).
* Quiet bands. These are measured at idle (800 rpm), cruise (2000 rpm, load 0.2) and coasting (2500 rpm, load
  -0.5), with every layer at its maximum (`T 1`, `X turbo/bov/pops 2`). Energy above 1.5 kHz is 71-79 dB under the
  total; check.py requires at least 35. Non-tonal energy above 500 Hz is 57-61 dB under the total; check.py requires
  at least 50. test_core asserts that the pulse noise, whistle, sigh and pops are exactly 0 there.
* Clicks: the Waveguide engine's slope-outlier detector finds none in the samples, the transition script (preset switching every
  50 ms, all `X` changes, mute, shifts, malformed lines) or the lift renders.

### Noise and hiss

Measured with layer-isolated builds (`cc -DES_LAYERS=<mask>`: 1 pulse noise, 2 turbo, 4 sigh, 8 pops, 16 jitter,
32 orders), all four presets, defaults; each value is the layer's added non-tonal energy (or its energy above
1.5 kHz) under the total:

| layer | |
|---|---|
| turbo whistle | off (`T 0`). With `T 1`: a sine, -35..-38 dB at full load only, nothing at cruise / coast / idle |
| pulse noise | 0 at idle / cruise / coast; at full load at least -68 dB (-73..-94 dB above 500 Hz) |
| recirculation sigh | after >= 0.5 s near full load only, ~200 ms, < 1.2 kHz, 24-28 dB under the engine |
| pops | sine thump, -22..-29 dB in the lift window, -64..-75 dB above 1.5 kHz |
| phase jitter | ~10 Hz wide: -28..-46 dB of low skirts, -42..-62 dB above 500 Hz |
| **total above 1.5 kHz**: idle / cruise / coast / full load | -71..-73 / -78..-79 / -75..-77 / -78..-81 dB |

What is left of the "non-tonal" energy (-17..-34 dB) is the burble itself: the per-firing irregularity and the
half-order lump. It lies around the order lines below 500 Hz and is not hiss.

## Sources

The design follows these sources:

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
* The recirculation sigh (0.6-1.1 kHz, about 25 dB under the engine, after a full-load lift only) is now the
  only thing this engine adds above 250 Hz. Set `X bov 0` if it sticks out over the bass layer.
* Every non-order layer is now nearly silent. If the cabin sounds too "clean" or organ-like, raise `am_rand` /
  `wobble` in `presets.c` (irregularity and burble), not the noise.
* Turbo whistle, sigh and pops are stylised approximations (no boost or turbo-speed telemetry). The B47's fuel-cut
  overrun does not really crackle; the pops are an Audi-S-style effect, gentle by default.
* Load is crank torque only. The off-gas / full-load balance (about 4 dB) assumes the app's load scale matches the
  test scale.
* Telemetry slower than about 20 Hz leaves rpm steps up to 50 ms old. The prediction only helps on steady ramps.
