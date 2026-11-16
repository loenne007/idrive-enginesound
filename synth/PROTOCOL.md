# Sound engine protocol

Every sound engine (`synth/<id>/`, binary `es-synth-<id>`: `simulate`, `enhance`, `waveguide`, `replica`) has the same command line
and reads the same control lines; the service (`tool/lfb-enginesound/esd.js`) runs the chosen one.

## Command line

```
es-synth-<id> --out alsa:<PCM>|wav:<file>|raw [--preset i6|v8|v10|v12] [--volume 0..2] [--demo <seconds>|--script <file>]
es-synth-<id> --tone alsa:<PCM>      # routing test: 10 s of soft 880 Hz beeps
es-synth-<id> --info                 # JSON: sample_rate, presets (id, idle, redline, cylinders, turbo_default)
es-synth-<id> --bench <seconds>      # render speed per preset
```

e.g. `es-synth-simulate --out alsa:plug:smartx:CarLifeMedia_CF_48_STEREO --preset v8` in the car, or
`synth/simulate/build/es-synth-host --out wav:v8.wav --demo 40 --preset v8` on a computer. Output: 48 kHz S16LE stereo.
An ALSA device whose name says another format (`..._CF_24_MONO`, the car's prompt channels) is opened in that format
and the output converted (`synth/common/out_alsa.c`). Logs go to stderr. A script has one `<seconds> <control line>` per line, in order (see
`synth/waveguide/README.md`). `ES_ALSA_US` sets the ALSA buffer (default 64000 us).

## Control lines

On stdin, one per line, typically 20-100 a second. Malformed lines are ignored; nothing may block the audio.

| Line | |
|---|---|
| `P <rpm> <load> <throttle> <gear> <speed>` | the engine's rpm (mapped to the preset's range by the app; `bass` gets the car's real rpm), load -1..1 (negative = overrun), throttle 0..1, gear (-1 R, 0 N/P, 1..10), km/h |
| `E <preset>` | `i6`, `v8`, `v10`, `v12`; crossfade, no clicks |
| `T 0\|1` | the turbo layer off / on |
| `V <0..2>` | master volume (the app sends 0.18-1.4: its 10-100 %) |
| `M 0\|1` | mute (fade) |
| `S <from> <to>` | a gear change of the car's automatic (ZF 8HP): smooth, no ignition cut |
| `X <name> <value>` | a sound setting (below) |
| `Q` | quit (fade out first) |

No `P` line for 1.5 s: fade to silence until they come back.

## Sound settings (`X`)

Values are multipliers, 0..2, 1 = the engine's own default. Changes are smoothed (no clicks) and kept across `E`.
Unknown names are ignored.

| Name | |
|---|---|
| `offgas` | the level off the throttle (idle, coasting, light load) against the level under load |
| `turbo` | turbo whistle / spool level (0 = none; `T 0` also switches it off) |
| `bov` | blow-off / recirculation whoosh when lifting after boost (0 = none) |
| `pops` | pops, bangs and crackle on the overrun (0 = none, 1 = subtle, 2 = loud sport exhaust, more often) |
| `bass` | low end below about 150 Hz (a smooth shelf, about ±9 dB at 0 / 2) |

## Response

The sound follows the `P` rpm with at most about 30 ms of smoothing (no slow slew limits): a gear change must
sound as quick as it is in the car. The output buffer adds the device's own latency (SmartX: 64 ms).

## Speaker protection

Every engine's `main.c` runs `synth/common/protect.h` as its last stage, before every output: on the car (ALSA, raw)
first a +6 dB live gain (radio loudness; `ES_LIVE_GAIN_DB` 0..20 overrides; WAV renders keep the engine's own level
unless `ES_LIVE_GAIN` is set), then a 4th-order 30 Hz high-pass (no DC / infrasound), a 16 kHz low-pass, a bass
limiter (below ~150 Hz held under -3 dBFS), a power limiter (3 s RMS under -8 dBFS), a 1 ms look-ahead peak limiter
and a soft clip under -1 dBFS, and a fade-in at start. `make -C synth test` tests it
(`common/test_protect.c`, with and without the live gain).
