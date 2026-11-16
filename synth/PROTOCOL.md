# Sound engine protocol

Every sound engine (`synth/<id>/`, binary `es-synth-<id>`) has the same command line (see `synth/a/README.md`:
`--out alsa:/wav:/raw`, `--demo`, `--script`, `--tone`, `--info`, `--bench`) and reads the same control lines on
stdin, one per line, typically 20-100 a second. Malformed lines are ignored; nothing may block the audio.

| Line | |
|---|---|
| `P <rpm> <load> <throttle> <gear> <speed>` | the engine's rpm (already mapped to the preset's range), load -1..1 (negative = overrun), throttle 0..1, gear (-1 R, 0 N/P, 1..10), km/h |
| `E <preset>` | `i6`, `v8`, `v10`, `v12`; crossfade, no clicks |
| `T 0\|1` | the turbo layer off / on |
| `V <0..1>` | master volume |
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
