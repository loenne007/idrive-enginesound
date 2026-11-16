# Engine Sound for iDrive 7

A synthesized petrol engine on your BMW's speakers, driven live by the car's **real rpm, gear and engine load**.
Pick an **inline-6** (B58 / S58 style), a cross-plane **V8**, a high-revving **V10** or a **V12**. The sound is
generated in real time on the head unit, not played back from samples. Each cylinder fires at its crank angle, through
exhaust resonators, intake roar and turbo, with burble on the overrun.

Made for the BMW iDrive 7 head unit (MGU), installed through the [MGU Store](https://github.com/robertnisipeanu/mgu-store).
Developed for a G20 320d.

> **Use at your own risk.** An unofficial hobby project, not affiliated with BMW. It changes nothing in the car's
> control units; it only plays audio. Keep the volume sensible: you still need to hear the road, sirens and the
> car's warnings.

## Install

1. In the MGU Store: **Settings → Add source**, type `loenne007/idrive-enginesound`, choose the branch **main**.
2. Install **Engine Sound**. The Store installs its service (**Engine Sound service**, `lfb-enginesound`) with it.
3. Restart the head unit when the Store asks (the new service starts at the next start).
4. Start the engine. The app starts with the head unit; open its tile to pick the engine and the audio mode.

## How it works

```
 car (SOME/IP)                     app lfb_enginesound (Node 8, autostart)            service lfb-enginesound
 rpm, gear, torque, speed,  ──▶   maps the car's rpm onto the engine's rev range,  ──▶  esd.js ──▶ es-synth-<id> (C)
 driving mode                      load from torque, on/off logic, audio routing         Unix socket    │
                                   (GENIVI AudioManager: CarLifeMedia → speakers)                             ▼
                                                                                    ALSA → SmartX → car speakers
```

- **rpm**: the diesel's idle-to-max range (about 800–4800 rpm) is stretched over the chosen engine's
  idle-to-redline range, so a V10 really screams at the top. *Settings → Rev range → The car's real rpm* turns it off.
- **load**: crankshaft torque against the car's max torque (*Car's max torque*, 400 Nm for a 320d). Pulling hard
  makes it louder and harder-edged; negative torque (engine braking) gives the overrun burble.
- **gear**: a gear change (ZF 8HP) gives the short torque dip and the downshift character.

## Audio

The engine plays as its own source on the main speakers: the AudioManager source `CarLifeMedia` (China-only, free
elsewhere), the route Moonride uses on iDrive 7. The music pauses while the engine sound plays and comes back a few
seconds after it stops (engine off, switched off, or not in Sport with *Only in Sport mode*). Mixing over the music
(as a navigation prompt does) is not supported yet.

**Test sound** plays 10 s of beeps through the same route, also with the engine off.

## Sound engines

Several synthesizers ("sound engines") are installed side by side, each playing all four engines, to compare them in
the car: *Settings → Sound engine* (or the buttons under the engine list). Each one is a folder `synth/<id>/` with its
own C source and tests, built into `tool/lfb-enginesound/es-synth-<id>`; the service runs the chosen one.

| Id | |
|---|---|
| `a` | rewritten around Engine_Sim's sound path (agent A) |
| `b` | Engine_Sim ported 1:1 (agent B) |
| `c` | light and quick (agent C): exhaust + intake pulses per cylinder with rasp riding on each pulse, pipes, muffler chambers; ~26 ms response |
| `d` | Active Sound Design emulation: enriches the car's own diesel with rpm-locked engine orders (BMW B57 / Audi V6, V8 TDI style); gets the real rpm |

A new engine: a new `synth/<id>/` (same command line and control lines, see [synth/PROTOCOL.md](synth/PROTOCOL.md)), its binary in
the tool's `component.json` `files`, its entry in `SOUND_ENGINES` (`apps/lfb_enginesound/core.js`) and in the
`soundEngine` setting (`app.json`).

## Sound page

Sliders (0–200 %, 100 % = the engine's own default) for **Off the gas** (idle, coasting, light load), **Turbo**,
**Blow-off**, **Pops & bangs** and **Bass**; also in Settings. Whatever they are set to, the speaker protection
(below) keeps the output safe.

## Speaker protection

The last stage of every sound engine (`synth/common/protect.h`): no DC or infrasound (30 Hz high-pass), nothing
above 16 kHz, the bass below ~100 Hz held under -8 dBFS, the long-term power held under -14 dBFS RMS, peaks under
-1 dBFS, and a fade-in at start.

## Settings

| Setting | |
|---|---|
| Engine sound | on / off (also the **On / Off** button) |
| Engine | inline-6, V8, V10, V12 (also **Next engine**) |
| Sound engine | which synthesizer plays (above) |
| Volume | 10–100 % of the synth's level (the car's own volume knob applies on top) |
| Only in Sport mode | plays only while the driving mode is Sport |
| Turbo sound | the inline-6's and V8's turbo whine and blow-off |
| Rev range, Car's max rpm, Car's max torque | the mapping above |

## Repository

| Path | |
|---|---|
| `apps/lfb_enginesound/` | the app: `main.js` (UI, background), `core.js` (mapping, on/off), `route.js` (AudioManager), `daemon.js` (socket client) |
| `tool/lfb-enginesound/` | the service: `esd.js`, the prebuilt `es-synth-<id>` per sound engine (x86-64, glibc ≤ 2.27), its systemd unit and `component.json` |
| `synth/<id>/` | each sound engine's C source, Makefile, tests, sample renders (e.g. [synth/a/README.md](synth/a/README.md)) |
| `kit/` | the MGU app kit from [idrive7-apps](https://github.com/cauliflowermossgrape/idrive7-apps) (MIT) |
| `mgu-repo.json` | the Store's repo-tree manifest: the head unit builds the app from source |

Develop on a computer:

```sh
node test/core.test.js                       # the app's mapping and on/off logic
make -C synth test                           # every sound engine, rendered and checked offline
synth/a/build/es-synth-host --out wav:v8.wav --demo 40 --preset v8   # listen to a demo drive
make -C synth car                            # the head unit's binaries (needs zig 0.17)
```

## Credits

- Sound design ideas and engine constants (firing orders, crank angles): [Engine_Sim](https://github.com/YigitSalihEmecen/Engine_Sim)
  by Yigit Salih Emecen (MIT).
- App kit: [idrive7-apps](https://github.com/cauliflowermossgrape/idrive7-apps) by cauliflowermossgrape (MIT).
- The audio route: [Moonride](https://github.com/JustAnotherPU/moonride) and [RideTube](https://github.com/JustAnotherPU/ridetube)
  by JustAnotherPU showed the SmartX / AudioManager path; [NamelsMissing/mgu-apps](https://github.com/NamelsMissing/mgu-apps)
  published the AudioManager's source list.

## License

MIT (`LICENSE`). The kit keeps its own notice (`LICENSE.idrive7-apps`).
